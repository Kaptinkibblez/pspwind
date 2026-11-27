/*
 * audio.h - software mixer: cached IMA ADPCM effects, positional voices,
 * streamed music and speech.
 *
 * sounds.dpak holds the effects (DSND, IMA ADPCM, mono); clips are loaded on
 * first use into a buddy-allocated cache in the AUDIO_UI region and evicted
 * least-recently-used when it fills (never while a voice plays them).
 * music.dpak holds music and voices: WAV-born ADPCM streams are read in
 * chunks, MP3 frames are decoded by the PSP's Media Engine (sceMp3); the
 * host build has no MP3 decoder and plays MP3 entries as silence of the
 * right length, so SayDone and playlists keep their timing in tests.
 *
 * Every voice is resampled (16.16 fixed point, linear) to 44.1 kHz stereo.
 * Positional voices use the SOUN ranges like OpenMW: inside min distance at
 * full volume, then min/d, faded out over the last tenth before max; pan
 * follows the listener's facing. Gains ramp across each mix buffer.
 *
 * Threading: on the PSP a dedicated thread mixes into two hardware buffers
 * (sceAudioOutputBlocking); the game thread calls everything else. A
 * semaphore guards the voice table; clip loads happen outside the lock.
 * On the host, audio_mix() is called directly (tests).
 */
#ifndef PSPWIND_AUDIO_H
#define PSPWIND_AUDIO_H

#include "common.h"
#include "fmt.h"
#include "mem.h"
#include "pak.h"
#include "rpgdb.h"

#define AUDIO_RATE 44100
#define AUDIO_VOICES 24
#define AUDIO_CLIPS 128
#define AUDIO_CACHE_BYTES PW_MB(2)
#define AUDIO_BLOCK 1024            /* frames per mix buffer (multiple of 64) */
#define AUDIO_DECODE_MAX 1152       /* frames per decoded source block */
#define AUDIO_STREAM_CHUNK 16384    /* ADPCM stream read size (bytes) */
#define AUDIO_TRACKS 64

enum { AUDIO_CAT_MASTER = 0, AUDIO_CAT_EFFECTS, AUDIO_CAT_FOOTSTEPS, AUDIO_CAT_VOICE, AUDIO_CAT_MUSIC,
       AUDIO_CAT_COUNT };
enum { AUDIO_STREAM_MUSIC = 0, AUDIO_STREAM_SPEECH = 1, AUDIO_STREAM_COUNT };
enum { AUDIO_MUSIC_OFF = 0, AUDIO_MUSIC_EXPLORE, AUDIO_MUSIC_BATTLE };

typedef struct {
    uint32_t file;            /* pack entry hash, 0 = free slot */
    uint8_t *data;            /* DSND header + payload in the cache */
    uint32_t size;
    uint32_t stamp;           /* last use (LRU) */
    uint16_t refs;            /* voices playing it */
    uint16_t pad;
} AudioClip;

typedef struct {
    const DPakEntry *entry;   /* in music.dpak */
    DSndHeader hdr;
    int32_t chunk;            /* loaded ADPCM chunk index, -1 none */
    uint32_t chunk_bytes;     /* bytes per chunk (whole block groups) */
    uint8_t *buf;             /* AUDIO_STREAM_CHUNK bytes */
    uint32_t mp3_pos;         /* MP3: next frame byte to feed */
    uint32_t mp3_done;        /* host MP3: frames already "decoded" */
    int32_t mp3_handle;       /* PSP sceMp3 handle or -1 */
    uint8_t *mp3_buf;         /* PSP: sceMp3 stream buffer */
    int16_t *mp3_pcm;         /* PSP: sceMp3 output buffer */
    int active;
} AudioStream;

typedef struct {
    int active;
    uint32_t serial;          /* handle = slot | serial << 8 */
    int16_t clip;             /* cache slot or -1 */
    int8_t stream;            /* AUDIO_STREAM_* or -1 */
    int8_t cat;               /* AUDIO_CAT_* */
    uint32_t sound;           /* SOUN hash (GetSoundPlaying / StopSound), 0 = file */
    uint32_t owner;           /* reference key, 0 = none */
    float volume;             /* SOUN volume x caller volume */
    float pitch;
    int loop;
    int positional;
    float pos[3];
    float min_d, max_d;
    float fade;               /* per-second change of the fade gain (negative: fading out) */
    float fade_gain;
    /* source state (mixer thread) */
    const DSndHeader *hdr;
    const uint8_t *payload;
    uint32_t rate;
    uint8_t channels;
    uint32_t frames;          /* total frames per channel */
    uint32_t pos_int;         /* current frame */
    uint32_t pos_frac;        /* 16.16 fraction */
    uint32_t step;            /* 16.16 frames per output frame */
    int32_t blk_start;        /* first frame in pcm, -1 none */
    uint32_t blk_len;
    int16_t pcm[AUDIO_DECODE_MAX * 2];
    /* gains in Q12: target set by the game thread, current ramped by the mixer */
    int32_t gl, gr, tgl, tgr;
} AudioVoice;

typedef struct {
    uint32_t key;             /* music.dpak entry hash */
    uint8_t category;         /* AUDIO_MUSIC_* */
} AudioTrack;

typedef struct {
    uint32_t owner;           /* 0 = no owner */
    const float *pos;         /* NULL = not positional (2D) */
    float volume;             /* 0..1 multiplier (0 means 1) */
    float pitch;              /* 0 means 1 */
    int loop;
    int cat;                  /* AUDIO_CAT_* (0 means effects) */
} AudioPlayOpts;

typedef struct Audio {
    int ready;
    Pak sounds, music;
    RpgDb *db;
    MemBuddy heap;
    AudioClip clips[AUDIO_CLIPS];
    AudioVoice voices[AUDIO_VOICES];
    AudioStream streams[AUDIO_STREAM_COUNT];
    uint32_t stamp;
    uint32_t serial;
    float listener[3];
    float listener_yaw;
    float volume[AUDIO_CAT_COUNT];
    float min_mult, max_mult, def_min, def_max;
    int32_t mix[AUDIO_BLOCK * 2];
    /* music */
    AudioTrack tracks[AUDIO_TRACKS];
    uint32_t track_count;
    int music_mode;           /* requested AUDIO_MUSIC_* */
    int music_playing_mode;
    uint32_t music_voice;     /* handle or 0 */
    uint32_t music_override;  /* StreamMusic entry hash pending, 0 none */
    uint32_t rng;
    /* game-side position lookups for owner-bound voices */
    int (*locate)(void *user, uint32_t owner, float pos[3]);
    void *locate_user;
    /* diagnostics */
    uint32_t loads, evictions, misses, steals;
    uint32_t frames_mixed;
    /* PSP backend */
    int thread;
    int sema;
    volatile int running;
    int hw_channel;
    int mp3_ready;
} Audio;

/* Opens <data_dir>/sounds.dpak and music.dpak (either may be missing) and
 * reserves the cache. Returns 0 when at least one pack opened. */
int audio_init(Audio *a, const char *data_dir, RpgDb *db, char *err, uint32_t errlen);
/* PSP: starts the output thread (and the MP3 decoder). No-op on the host. */
int audio_start(Audio *a);
void audio_shutdown(Audio *a);

/* Plays a SOUN record (id hash); returns a handle or 0. */
uint32_t audio_play_sound(Audio *a, uint32_t sound, const AudioPlayOpts *o);
/* Plays a sounds.dpak / music.dpak entry directly (min/max in game units;
 * 0 = defaults). Voices and music stream; effects come from the cache. */
uint32_t audio_play_file(Audio *a, uint32_t file, const AudioPlayOpts *o, float min_d, float max_d);
void audio_stop_handle(Audio *a, uint32_t handle);
/* Stops voices matching owner / sound (0 = any). */
void audio_stop(Audio *a, uint32_t owner, uint32_t sound);
int audio_playing(Audio *a, uint32_t owner, uint32_t sound);
int audio_handle_playing(Audio *a, uint32_t handle);

/* Speech (Say): one voice at a time on the speech stream. */
uint32_t audio_say(Audio *a, uint32_t owner, uint32_t file, const float *pos);
int audio_saying(Audio *a, uint32_t owner);

/* Music: playlist mode (explore / battle / off), StreamMusic override. */
void audio_music_mode(Audio *a, int mode);
void audio_stream_music(Audio *a, uint32_t file);

void audio_set_listener(Audio *a, const float pos[3], float yaw);
void audio_set_volume(Audio *a, int cat, float v);
/* Game thread, once per frame: positions, gains, fades, music. */
void audio_update(Audio *a, float dt);
/* Mixes frames of interleaved stereo int16 (mixer thread / tests). */
void audio_mix(Audio *a, int16_t *out, uint32_t frames);

/* Sound-file helpers shared with tests: decodes one IMA ADPCM channel block. */
void audio_ima_decode(const uint8_t *block, uint32_t count, int16_t *out, int stride);

#endif /* PSPWIND_AUDIO_H */
