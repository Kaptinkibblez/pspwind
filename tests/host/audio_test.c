/*
 * audio_test: the mixer on the cooked test world (sounds.dpak, music.dpak,
 * rpg.dpak SOUN records): ADPCM decoding, resampling, 2D and positional
 * gains, panning, loops, stopping, voice stealing, cache eviction, music
 * playlists and the timing of MP3 speech (silent on the host).
 *
 *   audio_test <test world data dir>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "mem.h"
#include "rpgdb.h"

static int g_fail, g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL: ");                                               \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static RpgDb g_db;
static Audio g_audio;
static int16_t g_buf[AUDIO_RATE * 2];

typedef struct {
    double rms_l, rms_r;
    int peak;
} Level;

/* mixes `seconds` of audio in game-sized steps and measures it */
static Level run(float seconds)
{
    Level lv = { 0.0, 0.0, 0 };
    uint32_t total = (uint32_t)(seconds * AUDIO_RATE), done = 0;
    double sl = 0.0, sr = 0.0;
    while (done < total) {
        uint32_t n = total - done > 735 ? 735 : total - done, i; /* 1/60 s */
        audio_update(&g_audio, (float)n / AUDIO_RATE);
        audio_mix(&g_audio, g_buf, n);
        for (i = 0; i < n; ++i) {
            int l = g_buf[2 * i], r = g_buf[2 * i + 1];
            sl += (double)l * l;
            sr += (double)r * r;
            if (abs(l) > lv.peak)
                lv.peak = abs(l);
            if (abs(r) > lv.peak)
                lv.peak = abs(r);
        }
        done += n;
    }
    lv.rms_l = sqrt(sl / (total ? total : 1));
    lv.rms_r = sqrt(sr / (total ? total : 1));
    return lv;
}

static int active_voices(void)
{
    int i, n = 0;
    for (i = 0; i < AUDIO_VOICES; ++i)
        n += g_audio.voices[i].active;
    return n;
}

int main(int argc, char **argv)
{
    char err[200], path[300];
    AudioPlayOpts o;
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t h;
    Level lv;
    if (argc < 2) {
        fprintf(stderr, "usage: audio_test <data dir>\n");
        return 2;
    }
    mem_init();
    snprintf(path, sizeof(path), "%s/rpg.dpak", argv[1]);
    if (rpgdb_open(&g_db, path, err, sizeof(err)) != 0) {
        printf("rpgdb_open failed: %s\n", err);
        return 1;
    }
    CHECK(audio_init(&g_audio, argv[1], &g_db, err, sizeof(err)) == 0, "audio_init (%s)", err);
    CHECK(audio_start(&g_audio) == 0, "start is a no-op on the host");
    CHECK(g_audio.track_count == 2, "explore + battle playlist (%u)", (unsigned)g_audio.track_count);
    audio_set_listener(&g_audio, origin, 0.0f);
    memset(&o, 0, sizeof(o));

    /* ADPCM decoder: block header sample is exact, nibbles follow IMA */
    {
        uint8_t block[8] = { 0x10, 0x00, 0, 0, 0x07, 0x08, 0, 0 };
        int16_t out[5];
        audio_ima_decode(block, 5, out, 1);
        /* code 7 at step 7: +0+7+3+1; index +8 -> step 16, code 0: +2 */
        CHECK(out[0] == 16 && out[1] == 27 && out[2] == 29, "IMA nibbles (%d %d %d)", out[0], out[1], out[2]);
    }

    /* silence before anything plays */
    lv = run(0.1f);
    CHECK(lv.peak == 0, "silent mixer (%d)", lv.peak);

    /* a 2D effect: the swish (11 kHz source -> 4x resampling) */
    h = audio_play_sound(&g_audio, pw_hash_name("Weapon Swish"), &o);
    CHECK(h != 0 && audio_playing(&g_audio, 0, pw_hash_name("Weapon Swish")), "swish playing");
    lv = run(0.15f);
    CHECK(lv.peak > 3000 && fabs(lv.rms_l - lv.rms_r) < 1.0, "swish audible and centred (peak %d, %.0f/%.0f)",
          lv.peak, lv.rms_l, lv.rms_r);
    lv = run(0.3f);
    CHECK(!audio_handle_playing(&g_audio, h) && active_voices() == 0, "one-shot ended after its length");
    CHECK(g_audio.loads >= 1, "clip loaded once (%u)", (unsigned)g_audio.loads);

    /* positional: near, far, beyond max range, left/right */
    {
        float right[3] = { 150.0f, 0.0f, 0.0f }, near[3] = { 0.0f, 150.0f, 0.0f };
        float far[3] = { 0.0f, 400.0f, 0.0f }, gone[3] = { 0.0f, 5000.0f, 0.0f };
        Level a1, a2, a3, a4;
        o.loop = 1;
        o.pos = near;
        h = audio_play_sound(&g_audio, pw_hash_name("Fire"), &o);  /* min 20, max 500 units */
        CHECK(g_audio.voices[h & 0xFF].min_d == 20.0f && g_audio.voices[h & 0xFF].max_d == 500.0f,
              "SOUN ranges x GMST multipliers");
        a1 = run(0.5f);
        audio_stop_handle(&g_audio, h);
        o.pos = far;
        h = audio_play_sound(&g_audio, pw_hash_name("Fire"), &o);
        a2 = run(0.5f);
        audio_stop_handle(&g_audio, h);
        o.pos = gone;
        h = audio_play_sound(&g_audio, pw_hash_name("Fire"), &o);
        a3 = run(0.5f);
        CHECK(audio_handle_playing(&g_audio, h), "out-of-range loop keeps running");
        audio_stop_handle(&g_audio, h);
        o.pos = right;
        h = audio_play_sound(&g_audio, pw_hash_name("Fire"), &o);
        a4 = run(0.5f);
        audio_stop_handle(&g_audio, h);
        CHECK(a1.rms_l > 2.0 * a2.rms_l && a2.rms_l > 0.0, "inverse distance falloff (%.0f > %.0f)", a1.rms_l,
              a2.rms_l);
        CHECK(a3.peak == 0, "silent beyond max range (%d)", a3.peak);
        CHECK(a4.rms_r > 4.0 * a4.rms_l, "source on the right pans right (%.0f / %.0f)", a4.rms_l, a4.rms_r);
        /* turning to face it centres it */
        audio_set_listener(&g_audio, origin, 1.5707963f);
        h = audio_play_sound(&g_audio, pw_hash_name("Fire"), &o);
        a4 = run(0.5f);
        audio_stop_handle(&g_audio, h);
        audio_set_listener(&g_audio, origin, 0.0f);
        CHECK(fabs(a4.rms_l - a4.rms_r) < 0.05 * a4.rms_r + 1.0, "facing the source centres it (%.0f / %.0f)",
              a4.rms_l, a4.rms_r);
        o.loop = 0;
        o.pos = NULL;
    }
    CHECK(active_voices() == 0, "all stopped");

    /* loops repeat; stop by owner and sound */
    o.owner = 1234;
    o.loop = 1;
    h = audio_play_sound(&g_audio, pw_hash_name("tw waves"), &o);
    lv = run(3.0f); /* the clip is 2 s long */
    CHECK(audio_handle_playing(&g_audio, h) && lv.rms_l > 100.0, "loop still playing after 3 s");
    audio_stop(&g_audio, 1234, 0);
    CHECK(!audio_playing(&g_audio, 1234, 0), "stopped by owner");
    o.owner = 0;
    o.loop = 0;

    /* volume categories */
    audio_set_volume(&g_audio, AUDIO_CAT_EFFECTS, 0.0f);
    audio_play_sound(&g_audio, pw_hash_name("Health Damage"), &o);
    lv = run(0.1f);
    CHECK(lv.peak == 0, "effects volume 0 mutes (%d)", lv.peak);
    audio_set_volume(&g_audio, AUDIO_CAT_EFFECTS, 1.0f);
    run(0.3f);

    /* voice stealing: more one-shots than voices */
    {
        int i;
        uint32_t steals = g_audio.steals;
        for (i = 0; i < AUDIO_VOICES + 6; ++i)
            audio_play_sound(&g_audio, pw_hash_name("tw critter moan"), &o);
        CHECK(active_voices() == AUDIO_VOICES && g_audio.steals == steals + 6, "voices stolen (%u)",
              (unsigned)(g_audio.steals - steals));
        lv = run(1.0f);
        CHECK(active_voices() == 0 && lv.peak == 32768, "24 voices saturate cleanly and end (%d left)", active_voices());
    }

    /* music playlist: explore, then battle fades the track out */
    audio_music_mode(&g_audio, AUDIO_MUSIC_EXPLORE);
    lv = run(0.5f);
    CHECK(g_audio.music_voice && audio_handle_playing(&g_audio, g_audio.music_voice) && lv.rms_l > 100.0,
          "explore music (rms %.0f)", lv.rms_l);
    CHECK(lv.rms_l > lv.rms_r * 1.1, "stereo music keeps its channels (%.0f / %.0f)", lv.rms_l, lv.rms_r);
    {
        uint32_t first = g_audio.music_voice;
        audio_music_mode(&g_audio, AUDIO_MUSIC_BATTLE);
        run(1.5f);
        CHECK(!audio_handle_playing(&g_audio, first) && g_audio.music_voice != first &&
              g_audio.music_playing_mode == AUDIO_MUSIC_BATTLE, "battle music after a fade");
        audio_music_mode(&g_audio, AUDIO_MUSIC_OFF);
        audio_stop_handle(&g_audio, g_audio.music_voice);
        run(0.1f);
        CHECK(!audio_playing(&g_audio, 0, 0), "music off");
    }

    /* speech: MP3 is silent on the host but lasts its length (60 frames) */
    h = audio_say(&g_audio, 77, pw_hash_name("sound/vo/tw/hello"), NULL);
    CHECK(h && audio_saying(&g_audio, 77), "saying");
    run(1.0f);
    CHECK(audio_saying(&g_audio, 77), "still saying after 1 s (1.57 s clip)");
    run(0.7f);
    CHECK(!audio_saying(&g_audio, 77), "SayDone after the clip");

    /* missing sounds fail cleanly */
    CHECK(audio_play_sound(&g_audio, pw_hash_name("no such sound"), &o) == 0, "unknown SOUN");
    CHECK(audio_play_file(&g_audio, pw_hash_name("sound/none"), &o, 0, 0) == 0, "unknown file");

    /* cache pressure: refill the cache many times over */
    {
        static const char *ids[] = { "FootBareLeft", "Weapon Swish", "Health Damage", "Door Wood Open",
                                     "destruction cast", "Menu Click", "tw waves", "Fire", "tw critter moan",
                                     "tw critter roar" };
        int i;
        uint32_t ev = g_audio.evictions;
        for (i = 0; i < AUDIO_CLIPS; ++i)
            g_audio.clips[i].stamp = 0;
        /* shrink the effective cache by holding most of the heap */
        {
            void *hold[64];
            int k = 0;
            while (k < 64 && (hold[k] = mem_buddy_alloc(&g_audio.heap, PW_KB(32))) != NULL)
                ++k;
            for (i = 0; i < 40; ++i) {
                audio_play_sound(&g_audio, pw_hash_name(ids[i % 10]), &o);
                run(0.05f);
            }
            while (k > 0)
                mem_buddy_free(&g_audio.heap, hold[--k]);
        }
        CHECK(g_audio.evictions > ev, "clips evicted under pressure (%u)", (unsigned)(g_audio.evictions - ev));
        CHECK(mem_buddy_check(&g_audio.heap) == 0, "cache heap consistent");
    }
    run(1.0f);
    audio_shutdown(&g_audio);
    printf("audio_test: %d/%d checks passed (loads %u, evictions %u, steals %u)\n", g_checks - g_fail, g_checks,
           (unsigned)g_audio.loads, (unsigned)g_audio.evictions, (unsigned)g_audio.steals);
    return g_fail ? 1 : 0;
}
