"""sound_demaker.py: WAV decoding, IMA ADPCM, resampling, MP3 frames, packs."""

import math
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import dmcommon  # noqa: E402
import make_testworld  # noqa: E402
import sound_demaker as S  # noqa: E402


def tone(n, freq=440.0, rate=22050, amp=12000):
    return [int(amp * math.sin(2 * math.pi * freq * i / rate)) for i in range(n)]


def snr_db(ref, got):
    sig = sum(v * v for v in ref) or 1
    err = sum((a - b) ** 2 for a, b in zip(ref, got)) or 1
    return 10 * math.log10(sig / err)


class AdpcmTests(unittest.TestCase):
    def test_round_trip_quality(self):
        src = tone(3000)
        enc = S.ima_encode_channel(src)
        self.assertEqual(len(enc) % S.BLOCK_BYTES, 0)
        self.assertEqual(len(enc) // S.BLOCK_BYTES, math.ceil(3000 / S.BLOCK_SAMPLES))
        out = S.ima_decode(enc, 1, 3000)[0]
        self.assertEqual(len(out), 3000)
        self.assertGreater(snr_db(src, out), 25.0)

    def test_block_header_is_exact_sample(self):
        src = tone(1200, amp=20000)
        enc = S.ima_encode_channel(src)
        out = S.ima_decode(enc, 1, 1200)[0]
        for b in range(0, 1200, S.BLOCK_SAMPLES):
            self.assertEqual(out[b], src[b])

    def test_stereo_interleaves_blocks(self):
        left, right = tone(600, 300), tone(600, 900)
        entry = S.encode_adpcm([left, right], 22050)
        magic, version, codec, channels, rate, samples, bb, bs, size, _r = struct.unpack_from(
            S.DSND_HEADER_FMT, entry, 0)
        self.assertEqual((magic, codec, channels, rate, samples, bb, bs),
                         (S.DSND_MAGIC, S.CODEC_ADPCM, 2, 22050, 600, S.BLOCK_BYTES, S.BLOCK_SAMPLES))
        l2, r2 = S.ima_decode(entry[S.DSND_HEADER_SIZE:], 2, 600)
        self.assertGreater(snr_db(left, l2), 20.0)
        self.assertGreater(snr_db(right, r2), 15.0)  # 900 Hz: IMA is ~19 dB there

    def test_extremes_do_not_overflow(self):
        src = [32767, -32768] * 300
        out = S.ima_decode(S.ima_encode_channel(src), 1, len(src))[0]
        self.assertTrue(all(-32768 <= v <= 32767 for v in out))


class WavTests(unittest.TestCase):
    def test_pcm16_and_8(self):
        src = tone(500)
        rate, chans = S.read_wav(make_testworld._wav(src, 22050))
        self.assertEqual((rate, chans[0]), (22050, src))
        rate, chans = S.read_wav(make_testworld._wav(src, 11025, bits=8))
        self.assertEqual(rate, 11025)
        self.assertTrue(all(abs(a - b) < 300 for a, b in zip(src, chans[0])))

    def test_stereo_downmix_and_resample(self):
        left, right = tone(4410, rate=44100), [0] * 4410
        inter = [v for pair in zip(left, right) for v in pair]
        data, info = S.cook_sound(make_testworld._wav(inter, 44100, channels=2), ".wav", False, 22050)
        self.assertEqual((info["rate"], info["channels"]), (22050, 1))
        samples = struct.unpack_from("<I", data, 12)[0]
        self.assertEqual(samples, 2205)

    def test_ima_wav_input(self):
        src = tone(1010)
        enc = S.ima_encode_channel(src)
        # repack our block (first sample + 504 nibbles) as a Microsoft IMA block
        block = enc[:S.BLOCK_BYTES]
        fmt = struct.pack("<HHIIHHHH", 0x11, 1, 22050, 11025, S.BLOCK_BYTES, 4, 2, S.BLOCK_SAMPLES)
        body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + \
            struct.pack("<I", len(block)) + block
        rate, chans = S.read_wav(b"RIFF" + struct.pack("<I", len(body)) + body)
        ref = S.ima_decode_block(enc, 0, S.BLOCK_SAMPLES)
        self.assertEqual(chans[0][:S.BLOCK_SAMPLES], ref)

    def test_ms_adpcm_header_samples(self):
        # one block: predictor 0, delta 16, sample1 1000, sample2 500, then zero nibbles
        block = struct.pack("<Bhhh", 0, 16, 1000, 500) + bytes(8)
        fmt = struct.pack("<HHIIHHHH", 2, 1, 22050, 11025, len(block), 4, 2, 32)
        body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + \
            struct.pack("<I", len(block)) + block
        _rate, chans = S.read_wav(b"RIFF" + struct.pack("<I", len(body)) + body)
        self.assertEqual(chans[0][:2], [500, 1000])
        self.assertEqual(len(chans[0]), 2 + 16)

    def test_bad_input(self):
        with self.assertRaises(S.SoundError):
            S.read_wav(b"not a wav")


class Mp3Tests(unittest.TestCase):
    def test_silent_frames(self):
        rate, channels, samples, frames = S.mp3_frames(make_testworld._silent_mp3(10))
        self.assertEqual((rate, channels, samples), (44100, 1, 11520))
        self.assertEqual(len(frames), 10 * (144 * 64000 // 44100))
        self.assertEqual(frames[:2], b"\xff\xfb")

    def test_garbage_rejected(self):
        with self.assertRaises(S.SoundError):
            S.mp3_frames(b"\x00" * 400)


class PackTests(unittest.TestCase):
    def test_cook_test_world(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = make_testworld.make(tmp)
            out = os.path.join(tmp, "cooked")
            self.assertEqual(S.main(["--data", data, "--out", out, "--jobs", "1"]), 0)
            sounds = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "sounds.dpak")))
            music = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "music.dpak")))
            step = sounds.get("sound/fx/tw/step")
            self.assertEqual(struct.unpack_from("<IHBB", step, 0), (S.DSND_MAGIC, 1, S.CODEC_ADPCM, 1))
            swish = sounds.get("sound/fx/tw/swish")
            self.assertEqual(struct.unpack_from("<I", swish, 8)[0], 11025)
            voice = music.get("sound/vo/tw/hello")
            self.assertEqual(voice[6], S.CODEC_MP3)
            theme = music.get("music/explore/tw_theme")
            self.assertEqual(theme[7], 2)  # stereo music
            index = music.get(S.MUSIC_INDEX).decode().split("\n")
            self.assertIn("explore music/explore/tw_theme", index)
            self.assertIn("battle music/battle/tw_battle", index)


if __name__ == "__main__":
    unittest.main()
