#!/usr/bin/env python3
"""sound_demaker - Morrowind Sound/ and Music/ -> sounds.dpak + music.dpak.

    sounds.dpak   every Sound/**/*.wav as 4-bit IMA ADPCM (mono, at most
                  --rate Hz): the effects the mixer keeps in its cache
    music.dpak    Music/**/*.mp3|wav and Sound/**/*.mp3 (voices): MP3 frames
                  are stored untouched for the PSP's Media Engine decoder
                  (sceMp3); WAV music becomes stereo IMA ADPCM streams.
                  "music/index" lists "<folder> <key>" lines (the
                  explore/battle playlists).

Entry names are dmcommon.sound_key() paths ("sound/fx/foot/walk",
"music/explore/mx_explore_1", "sound/vo/d/m/hlo_dm019"), so SOUN records,
Say/StreamMusic paths and INFO voice files all hash the same way.

Each entry is a DSND header (see src/fmt.h) + payload:
  codec 0 IMA ADPCM: blocks of block_samples frames; per channel a 4-byte
          header (int16 first sample, uint8 step index, 0) then
          (block_samples - 1) / 2 bytes of nibbles, low nibble first;
          stereo stores the left then the right channel block
  codec 1 MP3: the MPEG audio frames (ID3 tags stripped)

Input WAVs may be PCM 8/16/24/32-bit, IEEE float, IMA ADPCM or MS ADPCM.

    python3 sound_demaker.py --data "/path/Morrowind/Data Files" --out build/cooked
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from typing import Dict, List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import dmcommon  # noqa: E402
from dmcommon import LOG  # noqa: E402

DSND_MAGIC = 0x444E5344  # "DSND"
DSND_VERSION = 1
DSND_HEADER_FMT = "<IHBBIIHHII"   # 28 bytes + 4 reserved = 32
DSND_HEADER_SIZE = 32
CODEC_ADPCM, CODEC_MP3 = 0, 1
BLOCK_SAMPLES = 505               # 4-byte header + 252 bytes of nibbles
BLOCK_BYTES = 4 + (BLOCK_SAMPLES - 1) // 2
SIZES = {"DSndHeader": DSND_HEADER_SIZE}
MUSIC_INDEX = "music/index"   # text: "<category> <key>" per line (explore, battle, special...)

IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767]
assert len(IMA_STEP) == 89

MS_ADAPT = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]
MS_COEFS = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]


class SoundError(ValueError):
    pass


def _clamp16(v: int) -> int:
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


# --------------------------------------------------------------------------
# IMA ADPCM
# --------------------------------------------------------------------------
def ima_encode_channel(samples: Sequence[int]) -> bytes:
    """One channel -> blocks of BLOCK_SAMPLES (last block zero padded)."""
    out = bytearray()
    n = len(samples)
    index = 0
    pos = 0
    while pos < n or pos == 0:
        block = list(samples[pos:pos + BLOCK_SAMPLES])
        block += [0] * (BLOCK_SAMPLES - len(block))
        pred = block[0]
        out += struct.pack("<hBB", pred, index, 0)
        nib = 0
        for i in range(1, BLOCK_SAMPLES):
            step = IMA_STEP[index]
            diff = block[i] - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            delta = step >> 3
            if diff >= step:
                code |= 4
                diff -= step
                delta += step
            step >>= 1
            if diff >= step:
                code |= 2
                diff -= step
                delta += step
            step >>= 1
            if diff >= step:
                code |= 1
                delta += step
            pred = _clamp16(pred - delta if code & 8 else pred + delta)
            index = max(0, min(88, index + IMA_INDEX[code]))
            if i & 1:
                nib = code
            else:
                out.append(nib | (code << 4))
        pos += BLOCK_SAMPLES
        if pos >= n:
            break
    return bytes(out)


def ima_decode_block(data: bytes, ofs: int, count: int) -> List[int]:
    """Reference decoder (tests): count samples of one channel block."""
    pred, index = struct.unpack_from("<hB", data, ofs)
    out = [pred]
    p = ofs + 4
    for i in range(1, count):
        byte = data[p + (i - 1) // 2]
        code = byte & 0x0F if (i & 1) else byte >> 4
        step = IMA_STEP[index]
        delta = step >> 3
        if code & 4:
            delta += step
        if code & 2:
            delta += step >> 1
        if code & 1:
            delta += step >> 2
        pred = _clamp16(pred - delta if code & 8 else pred + delta)
        index = max(0, min(88, index + IMA_INDEX[code]))
        out.append(pred)
    return out


def ima_decode(payload: bytes, channels: int, samples: int) -> List[List[int]]:
    chans: List[List[int]] = [[] for _ in range(channels)]
    blocks = (samples + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES
    for b in range(blocks):
        for c in range(channels):
            ofs = (b * channels + c) * BLOCK_BYTES
            chans[c].extend(ima_decode_block(payload, ofs, BLOCK_SAMPLES))
    return [ch[:samples] for ch in chans]


# --------------------------------------------------------------------------
# WAV input
# --------------------------------------------------------------------------
def _ima_wav_decode(data: bytes, channels: int, block_align: int) -> List[List[int]]:
    """Microsoft IMA ADPCM (format 0x11): 4-byte headers per channel, then
    4-byte groups of 8 nibbles interleaved per channel."""
    chans: List[List[int]] = [[] for _ in range(channels)]
    for bofs in range(0, len(data) - 4 * channels + 1, block_align):
        block = data[bofs:bofs + block_align]
        preds, idxs = [], []
        for c in range(channels):
            p, i = struct.unpack_from("<hB", block, 4 * c)
            preds.append(p)
            idxs.append(min(88, i))
            chans[c].append(p)
        p = 4 * channels
        while p + 4 * channels <= len(block):
            for c in range(channels):
                for byte in block[p + 4 * c:p + 4 * c + 4]:
                    for code in (byte & 0x0F, byte >> 4):
                        step = IMA_STEP[idxs[c]]
                        delta = step >> 3
                        if code & 4:
                            delta += step
                        if code & 2:
                            delta += step >> 1
                        if code & 1:
                            delta += step >> 2
                        preds[c] = _clamp16(preds[c] - delta if code & 8 else preds[c] + delta)
                        idxs[c] = max(0, min(88, idxs[c] + IMA_INDEX[code]))
                        chans[c].append(preds[c])
            p += 4 * channels
    return chans


def _ms_adpcm_decode(data: bytes, channels: int, block_align: int,
                     coefs: List[Tuple[int, int]]) -> List[List[int]]:
    chans: List[List[int]] = [[] for _ in range(channels)]
    hdr = 7 * channels
    for bofs in range(0, len(data) - hdr + 1, block_align):
        block = data[bofs:bofs + block_align]
        pred = list(block[:channels])
        delta = list(struct.unpack_from(f"<{channels}h", block, channels))
        s1 = list(struct.unpack_from(f"<{channels}h", block, 3 * channels))
        s2 = list(struct.unpack_from(f"<{channels}h", block, 5 * channels))
        c1 = [coefs[min(p, len(coefs) - 1)][0] for p in pred]
        c2 = [coefs[min(p, len(coefs) - 1)][1] for p in pred]
        for c in range(channels):
            chans[c].append(s2[c])
            chans[c].append(s1[c])
        ch = 0
        for byte in block[hdr:]:
            for code in (byte >> 4, byte & 0x0F):
                signed = code - 16 if code & 8 else code
                p = (s1[ch] * c1[ch] + s2[ch] * c2[ch]) // 256 + signed * delta[ch]
                p = _clamp16(p)
                s2[ch], s1[ch] = s1[ch], p
                delta[ch] = max(16, (MS_ADAPT[code] * delta[ch]) // 256)
                chans[ch].append(p)
                ch = (ch + 1) % channels
    return chans


def read_wav(data: bytes) -> Tuple[int, List[List[int]]]:
    """-> (rate, per-channel int16 samples)."""
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SoundError("not a RIFF WAVE file")
    fmt = None
    pcm = None
    pos = 12
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None or len(fmt) < 16:
        raise SoundError("missing fmt or data chunk")
    tag, channels, rate, _bps, block_align, bits = struct.unpack_from("<HHIIHH", fmt, 0)
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if channels < 1 or channels > 8 or rate < 1000 or rate > 192000:
        raise SoundError(f"unsupported layout ({channels} ch, {rate} Hz)")
    if tag == 1:
        width = bits // 8
        if width not in (1, 2, 3, 4):
            raise SoundError(f"unsupported PCM width {bits}")
        frames = len(pcm) // (width * channels)
        chans: List[List[int]] = [[] for _ in range(channels)]
        if width == 2:
            vals = struct.unpack_from(f"<{frames * channels}h", pcm, 0)
            for c in range(channels):
                chans[c] = list(vals[c::channels])
        else:
            for f in range(frames):
                for c in range(channels):
                    o = (f * channels + c) * width
                    if width == 1:
                        v = (pcm[o] - 128) << 8
                    elif width == 3:
                        v = int.from_bytes(pcm[o:o + 3], "little", signed=True) >> 8
                    else:
                        v = struct.unpack_from("<i", pcm, o)[0] >> 16
                    chans[c].append(v)
        return rate, chans
    if tag == 3:
        if bits != 32:
            raise SoundError("only 32-bit float supported")
        frames = len(pcm) // (4 * channels)
        vals = struct.unpack_from(f"<{frames * channels}f", pcm, 0)
        return rate, [[_clamp16(int(v * 32767.0)) for v in vals[c::channels]] for c in range(channels)]
    if tag == 0x11:
        return rate, _ima_wav_decode(pcm, channels, block_align)
    if tag == 2:
        coefs = list(MS_COEFS)
        if len(fmt) >= 22:
            ncoef = struct.unpack_from("<H", fmt, 20)[0]
            if 0 < ncoef <= 32 and len(fmt) >= 22 + 4 * ncoef:
                coefs = [struct.unpack_from("<hh", fmt, 22 + 4 * i) for i in range(ncoef)]
        return rate, _ms_adpcm_decode(pcm, channels, block_align, coefs)
    raise SoundError(f"unsupported WAVE format tag 0x{tag:04x}")


def to_channels(chans: List[List[int]], want: int) -> List[List[int]]:
    if want == 1 and len(chans) > 1:
        n = min(len(c) for c in chans)
        k = len(chans)
        return [[sum(c[i] for c in chans) // k for i in range(n)]]
    if want == 2 and len(chans) == 1:
        return [chans[0], list(chans[0])]
    return chans[:want]


def resample(samples: List[int], src: int, dst: int) -> List[int]:
    """Linear interpolation; when shrinking, a box pre-filter over the
    decimation span keeps aliasing down."""
    if src == dst or not samples:
        return samples
    ratio = src / dst
    if ratio > 1.0:
        span = int(ratio)
        if span > 1:
            acc = []
            run = 0
            for i, v in enumerate(samples):
                run += v
                if i >= span:
                    run -= samples[i - span]
                acc.append(run // min(i + 1, span))
            samples = acc
    n = max(1, int(len(samples) / ratio))
    out = []
    last = len(samples) - 1
    for i in range(n):
        x = i * ratio
        j = int(x)
        f = x - j
        a = samples[min(j, last)]
        b = samples[min(j + 1, last)]
        out.append(int(a + (b - a) * f))
    return out


# --------------------------------------------------------------------------
# MP3 pass-through
# --------------------------------------------------------------------------
_MP3_BITRATES = {
    1: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],   # MPEG-1 layer III
    2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],        # MPEG-2/2.5 layer III
}
_MP3_RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def mp3_frames(data: bytes) -> Tuple[int, int, int, bytes]:
    """-> (rate, channels, samples, frame bytes). Skips ID3v2/v1 tags."""
    pos = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        size = (data[6] << 21) | (data[7] << 14) | (data[8] << 7) | data[9]
        pos = 10 + size
    end = len(data)
    if end >= 128 and data[end - 128:end - 125] == b"TAG":
        end -= 128
    first = None
    rate = channels = 0
    samples = 0
    while pos + 4 <= end:
        h = struct.unpack_from(">I", data, pos)[0]
        if (h >> 21) & 0x7FF != 0x7FF:
            if first is not None:
                break
            pos += 1
            continue
        version = (h >> 19) & 3
        layer = (h >> 17) & 3
        br_idx = (h >> 12) & 0xF
        sr_idx = (h >> 10) & 3
        pad = (h >> 9) & 1
        mode = (h >> 6) & 3
        if version == 1 or layer != 1 or br_idx in (0, 15) or sr_idx == 3:
            if first is not None:
                break
            pos += 1
            continue
        sr = _MP3_RATES[version][sr_idx]
        br = _MP3_BITRATES[1 if version == 3 else 2][br_idx] * 1000
        size = (144 if version == 3 else 72) * br // sr + pad
        if size < 4 or pos + size > end:
            break
        if first is None:
            first = pos
            rate = sr
            channels = 1 if mode == 3 else 2
        samples += 1152 if version == 3 else 576
        pos += size
    if first is None or samples == 0:
        raise SoundError("no MPEG layer III frames")
    return rate, channels, samples, data[first:pos]


# --------------------------------------------------------------------------
# entries
# --------------------------------------------------------------------------
def dsnd_header(codec: int, channels: int, rate: int, samples: int, payload: int) -> bytes:
    return struct.pack(DSND_HEADER_FMT, DSND_MAGIC, DSND_VERSION, codec, channels, rate, samples,
                       BLOCK_BYTES if codec == CODEC_ADPCM else 0,
                       BLOCK_SAMPLES if codec == CODEC_ADPCM else 0, payload, 0) + bytes(4)


def encode_adpcm(chans: List[List[int]], rate: int) -> bytes:
    samples = len(chans[0]) if chans else 0
    encoded = [ima_encode_channel(c) for c in chans]
    blocks = len(encoded[0]) // BLOCK_BYTES
    payload = bytearray()
    for b in range(blocks):
        for enc in encoded:
            payload += enc[b * BLOCK_BYTES:(b + 1) * BLOCK_BYTES]
    return dsnd_header(CODEC_ADPCM, len(chans), rate, samples, len(payload)) + bytes(payload)


def cook_sound(data: bytes, ext: str, music: bool, max_rate: int) -> Tuple[bytes, dict]:
    if ext == ".mp3":
        rate, channels, samples, frames = mp3_frames(data)
        info = {"codec": "mp3", "rate": rate, "channels": channels, "seconds": round(samples / rate, 2)}
        return dsnd_header(CODEC_MP3, channels, rate, samples, len(frames)) + frames, info
    rate, chans = read_wav(data)
    chans = to_channels(chans, 2 if (music and len(chans) > 1) else 1)
    target = rate
    while target > max_rate:
        target //= 2
    if target != rate:
        chans = [resample(c, rate, target) for c in chans]
    out = encode_adpcm(chans, target)
    info = {"codec": "adpcm", "rate": target, "channels": len(chans),
            "seconds": round(len(chans[0]) / target, 2) if chans and chans[0] else 0.0}
    return out, info


def _worker_ctx(arg):
    data_cfg, max_rate, music_rate = arg
    return (dmcommon.DataSource(data_cfg), max_rate, music_rate)


def _cook_one(ctx, item):
    source, max_rate, music_rate = ctx
    key, path, pack = item
    try:
        ext = os.path.splitext(path)[1]
        data, info = cook_sound(source.read(path), ext, pack == "music",
                                music_rate if pack == "music" else max_rate)
        info["key"] = key
        return key, pack, data, info, None
    except Exception as exc:  # noqa: BLE001 - reported per file
        return key, pack, None, None, f"{type(exc).__name__}: {exc}"


def collect(source: dmcommon.DataSource, voices: bool, music: bool) -> List[Tuple[str, str, str]]:
    items = []
    for p in source.list("sound/", (".wav", ".mp3")):
        if p.endswith(".mp3"):
            if voices:
                items.append((dmcommon.sound_key(p), p, "music"))
        else:
            items.append((dmcommon.sound_key(p), p, "sounds"))
    if music:
        for p in source.list("music/", (".wav", ".mp3")):
            items.append((dmcommon.sound_key(p), p, "music"))
    return items


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", action="append", default=[])
    ap.add_argument("--bsa", action="append", default=[])
    ap.add_argument("--out", required=True)
    ap.add_argument("--rate", type=int, default=22050, help="highest effect sample rate")
    ap.add_argument("--music-rate", type=int, default=44100, help="highest WAV music sample rate")
    ap.add_argument("--no-voices", action="store_true", help="skip Sound/Vo MP3 voices")
    ap.add_argument("--no-music", action="store_true")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    dmcommon.setup_logging(args.verbose)
    if not args.data and not args.bsa:
        ap.error("give at least one --data directory or --bsa archive")
    data_cfg = dmcommon.DataConfig(tuple(args.data), tuple(args.bsa))
    source = dmcommon.DataSource(data_cfg)
    items = collect(source, not args.no_voices, not args.no_music)
    LOG.info("sounds: %d files", len(items))
    t0 = time.time()
    packs: Dict[str, dmcommon.DPakWriter] = {"sounds": dmcommon.DPakWriter(dmcommon.DPAK_KIND_GENERIC),
                                              "music": dmcommon.DPakWriter(dmcommon.DPAK_KIND_GENERIC)}
    manifest = {"sounds": [], "music": [], "failures": []}
    seen = set()
    for key, pack, data, info, err in dmcommon.parallel_map(
            _cook_one, items, _worker_ctx, (data_cfg, args.rate, args.music_rate), args.jobs,
            dmcommon.progress_printer("sounds")):
        if err:
            LOG.warning("sound %s failed: %s", key, err)
            manifest["failures"].append({"key": key, "error": err})
            continue
        if key in seen:
            continue  # same name as .wav and .mp3: the first one wins
        seen.add(key)
        packs[pack].add(key, data)
        manifest[pack].append(info)
    # playlists: the runtime cannot list pack entries by name
    lines = sorted("%s %s" % (info["key"].split("/")[1], info["key"]) for info in manifest["music"]
                   if info["key"].startswith("music/") and info["key"].count("/") >= 2)
    packs["music"].add(MUSIC_INDEX, ("\n".join(lines) + "\n").encode("latin-1") if lines else b"")
    os.makedirs(args.out, exist_ok=True)
    for name, pak in packs.items():
        size = pak.write(os.path.join(args.out, name + ".dpak"))
        manifest[name + "_bytes"] = size
        LOG.info("wrote %s.dpak: %d entries, %.1f KB", name, len(pak.items), size / 1024.0)
    with open(os.path.join(args.out, "sounds_manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    LOG.info("sounds done in %.1fs, %d failed", time.time() - t0, len(manifest["failures"]))
    return 1 if (args.strict and manifest["failures"]) else 0


if __name__ == "__main__":
    sys.exit(main())
