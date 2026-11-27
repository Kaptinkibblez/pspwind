/*
 * audio.c - software mixer (see audio.h).
 */
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if PW_PSP
#include <pspaudio.h>
#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility.h>
#include <pspthreadman.h>
#endif

#define MP3_BUF_BYTES 16384
#define MP3_PCM_BYTES 9216

static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

void audio_ima_decode(const uint8_t *block, uint32_t count, int16_t *out, int stride)
{
    int32_t pred = (int16_t)(block[0] | (block[1] << 8));
    int idx = block[2] > 88 ? 88 : block[2];
    const uint8_t *p = block + 4;
    uint32_t i;
    if (!count)
        return;
    out[0] = (int16_t)pred;
    for (i = 1; i < count; ++i) {
        uint8_t byte = p[(i - 1) >> 1];
        int code = (i & 1) ? (byte & 15) : (byte >> 4);
        int32_t step = ima_step[idx], delta = step >> 3;
        if (code & 4)
            delta += step;
        if (code & 2)
            delta += step >> 1;
        if (code & 1)
            delta += step >> 2;
        pred = (code & 8) ? pred - delta : pred + delta;
        pred = PW_CLAMP(pred, -32768, 32767);
        idx += ima_index[code];
        idx = PW_CLAMP(idx, 0, 88);
        out[i * (uint32_t)stride] = (int16_t)pred;
    }
}

/* ------------------------------------------------------------------------ */
/* locking                                                                  */
/* ------------------------------------------------------------------------ */
static void lock(Audio *a)
{
#if PW_PSP
    if (a->sema > 0)
        sceKernelWaitSema(a->sema, 1, NULL);
#else
    (void)a;
#endif
}

static void unlock(Audio *a)
{
#if PW_PSP
    if (a->sema > 0)
        sceKernelSignalSema(a->sema, 1);
#else
    (void)a;
#endif
}

/* ------------------------------------------------------------------------ */
/* DSND validation                                                          */
/* ------------------------------------------------------------------------ */
static int dsnd_valid(const DSndHeader *h, uint32_t size)
{
    uint64_t need;
    if (size < sizeof(DSndHeader) || h->magic != DSND_MAGIC || h->version != 1)
        return 0;
    if (h->channels < 1 || h->channels > 2 || h->rate < 4000 || h->rate > 96000)
        return 0;
    if (h->data_size > size - sizeof(DSndHeader))
        return 0;
    if (h->codec == DSND_CODEC_MP3)
        return h->samples > 0;
    if (h->codec != DSND_CODEC_ADPCM || h->block_samples < 2 || h->block_samples > AUDIO_DECODE_MAX ||
        h->block_bytes != 4 + (h->block_samples - 1) / 2)
        return 0;
    need = (uint64_t)((h->samples + h->block_samples - 1) / h->block_samples) * h->block_bytes * h->channels;
    return need <= h->data_size;
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */
/* ------------------------------------------------------------------------ */
static float gmst(Audio *a, const char *id, float fallback)
{
    return a->db && a->db->hdr ? rpgdb_gmst_float(a->db, id, fallback) : fallback;
}

static void read_playlist(Audio *a)
{
    const DPakEntry *e = a->music.handle ? pak_find(&a->music, pw_hash_name("music/index")) : NULL;
    MemMark m;
    char *text, *p, *end;
    if (!e || !e->size || e->size > PW_KB(64))
        return;
    m = mem_mark_hi(MEM_REGION_AUDIO_UI);
    text = (char *)mem_try_alloc_hi(MEM_REGION_AUDIO_UI, e->size + 1, 4);
    if (text && pak_read(&a->music, e, text) == 0) {
        text[e->size] = '\0';
        for (p = text; *p && a->track_count < AUDIO_TRACKS; p = end) {
            char *sp = p;
            int cat;
            end = p;
            while (*end && *end != '\n')
                ++end;
            if (*end)
                *end++ = '\0';
            while (*sp && *sp != ' ')
                ++sp;
            if (!*sp)
                continue;
            *sp++ = '\0';
            cat = strcmp(p, "explore") == 0 ? AUDIO_MUSIC_EXPLORE : strcmp(p, "battle") == 0 ? AUDIO_MUSIC_BATTLE : 0;
            if (!cat)
                continue;
            a->tracks[a->track_count].key = pw_hash_name(sp);
            a->tracks[a->track_count].category = (uint8_t)cat;
            a->track_count++;
        }
    }
    mem_release_hi(MEM_REGION_AUDIO_UI, m);
}

int audio_init(Audio *a, const char *data_dir, RpgDb *db, char *err, uint32_t errlen)
{
    char path[200];
    int i, ok = 0;
    memset(a, 0, sizeof(*a));
    a->db = db;
    a->sema = -1;
    a->thread = -1;
    a->hw_channel = -1;
    a->rng = 0x2545F491u;
    for (i = 0; i < AUDIO_CAT_COUNT; ++i)
        a->volume[i] = 1.0f;
    a->volume[AUDIO_CAT_FOOTSTEPS] = 0.6f;
    a->volume[AUDIO_CAT_MUSIC] = 0.5f;
    a->min_mult = gmst(a, "fAudioMinDistanceMult", 20.0f);
    a->max_mult = gmst(a, "fAudioMaxDistanceMult", 50.0f);
    a->def_min = gmst(a, "fAudioDefaultMinDistance", 5.0f);
    a->def_max = gmst(a, "fAudioDefaultMaxDistance", 40.0f);
    snprintf(path, sizeof(path), "%s/sounds.dpak", data_dir);
    if (pak_open(&a->sounds, path, MEM_REGION_AUDIO_UI, NULL, 0) == 0)
        ok = 1;
    snprintf(path, sizeof(path), "%s/music.dpak", data_dir);
    if (pak_open(&a->music, path, MEM_REGION_AUDIO_UI, NULL, 0) == 0)
        ok = 1;
    if (!ok) {
        snprintf(err, errlen, "no sounds.dpak / music.dpak in %s", data_dir);
        return -1;
    }
    for (i = 0; i < AUDIO_STREAM_COUNT; ++i) {
        AudioStream *s = &a->streams[i];
        s->buf = (uint8_t *)mem_try_alloc(MEM_REGION_AUDIO_UI, AUDIO_STREAM_CHUNK, 64);
        s->mp3_buf = (uint8_t *)mem_try_alloc(MEM_REGION_AUDIO_UI, MP3_BUF_BYTES, 64);
        s->mp3_pcm = (int16_t *)mem_try_alloc(MEM_REGION_AUDIO_UI, MP3_PCM_BYTES, 64);
        s->mp3_handle = -1;
        s->chunk = -1;
        if (!s->buf || !s->mp3_buf || !s->mp3_pcm) {
            snprintf(err, errlen, "audio region too small for stream buffers");
            return -1;
        }
    }
    if (mem_available(MEM_REGION_AUDIO_UI) < AUDIO_CACHE_BYTES + PW_KB(64)) {
        snprintf(err, errlen, "audio region too small for the clip cache");
        return -1;
    }
    mem_buddy_init(&a->heap, MEM_REGION_AUDIO_UI, AUDIO_CACHE_BYTES, 256);
    read_playlist(a);
#if PW_PSP
    a->sema = sceKernelCreateSema("audio", 0, 1, 1, NULL);
#endif
    a->ready = 1;
    return 0;
}

#if PW_PSP
static int audio_thread(SceSize args, void *argp)
{
    static int16_t out[2][AUDIO_BLOCK * 2] __attribute__((aligned(64)));
    Audio *a = *(Audio **)argp;
    int buf = 0;
    (void)args;
    while (a->running) {
        audio_mix(a, out[buf], AUDIO_BLOCK);
        sceAudioOutputBlocking(a->hw_channel, PSP_AUDIO_VOLUME_MAX, out[buf]);
        buf ^= 1;
    }
    sceKernelExitThread(0);
    return 0;
}
#endif

int audio_start(Audio *a)
{
#if PW_PSP
    Audio *self = a;
    int r;
    if (!a->ready || a->running)
        return -1;
    /* the Media Engine MP3 decoder (music, voices); effects work without it */
    r = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    if (r >= 0 || r == (int)0x80020139) {
        r = sceUtilityLoadModule(PSP_MODULE_AV_MP3);
        if ((r >= 0 || r == (int)0x80020139) && sceMp3InitResource() >= 0)
            a->mp3_ready = 1;
    }
    a->hw_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_BLOCK, PSP_AUDIO_FORMAT_STEREO);
    if (a->hw_channel < 0)
        return -1;
    a->running = 1;
    a->thread = sceKernelCreateThread("audio", audio_thread, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (a->thread < 0) {
        a->running = 0;
        return -1;
    }
    sceKernelStartThread(a->thread, sizeof(self), &self);
    return 0;
#else
    (void)a;
    return 0;
#endif
}

static void end_voice(Audio *a, AudioVoice *v);

void audio_shutdown(Audio *a)
{
    int i;
    if (!a->ready)
        return;
#if PW_PSP
    if (a->running) {
        a->running = 0;
        sceKernelWaitThreadEnd(a->thread, NULL);
        sceKernelDeleteThread(a->thread);
    }
    if (a->hw_channel >= 0)
        sceAudioChRelease(a->hw_channel);
#endif
    lock(a);
    for (i = 0; i < AUDIO_VOICES; ++i)
        if (a->voices[i].active)
            end_voice(a, &a->voices[i]);
    unlock(a);
#if PW_PSP
    if (a->mp3_ready)
        sceMp3TermResource();
    if (a->sema > 0)
        sceKernelDeleteSema(a->sema);
#endif
    pak_close(&a->sounds);
    pak_close(&a->music);
    a->ready = 0;
}

/* ------------------------------------------------------------------------ */
/* clip cache                                                               */
/* ------------------------------------------------------------------------ */
static int evict_one(Audio *a)
{
    int i, best = -1;
    lock(a);
    for (i = 0; i < AUDIO_CLIPS; ++i) {
        AudioClip *c = &a->clips[i];
        if (c->file && !c->refs && (best < 0 || c->stamp < a->clips[best].stamp))
            best = i;
    }
    if (best >= 0) {
        mem_buddy_free(&a->heap, a->clips[best].data);
        memset(&a->clips[best], 0, sizeof(a->clips[best]));
        a->evictions++;
    }
    unlock(a);
    return best >= 0;
}

static int clip_get(Audio *a, uint32_t file)
{
    const DPakEntry *e;
    uint8_t *mem;
    int i, slot = -1;
    for (i = 0; i < AUDIO_CLIPS; ++i)
        if (a->clips[i].file == file) {
            a->clips[i].stamp = ++a->stamp;
            return i;
        }
    e = a->sounds.handle ? pak_find(&a->sounds, file) : NULL;
    if (!e || e->size < sizeof(DSndHeader))
        return -1;
    for (;;) {
        for (i = 0; i < AUDIO_CLIPS && slot < 0; ++i)
            if (!a->clips[i].file)
                slot = i;
        if (slot >= 0)
            break;
        if (!evict_one(a))
            return -1;
    }
    for (;;) {
        mem = (uint8_t *)mem_buddy_alloc(&a->heap, e->size);
        if (mem)
            break;
        if (!evict_one(a))
            return -1;
    }
    if (pak_read(&a->sounds, e, mem) != 0 || !dsnd_valid((const DSndHeader *)mem, e->size) ||
        ((const DSndHeader *)mem)->codec != DSND_CODEC_ADPCM) {
        mem_buddy_free(&a->heap, mem);
        return -1;
    }
    lock(a);
    a->clips[slot].file = file;
    a->clips[slot].data = mem;
    a->clips[slot].size = e->size;
    a->clips[slot].stamp = ++a->stamp;
    a->clips[slot].refs = 0;
    unlock(a);
    a->loads++;
    return slot;
}

/* ------------------------------------------------------------------------ */
/* streams (mixer thread)                                                   */
/* ------------------------------------------------------------------------ */
#if PW_PSP
static int mp3_feed(Audio *a, AudioStream *s)
{
    SceUChar8 *dst;
    SceInt32 towrite, srcpos;
    uint32_t n;
    if (sceMp3GetInfoToAddStreamData(s->mp3_handle, &dst, &towrite, &srcpos) < 0)
        return -1;
    n = (uint32_t)srcpos < s->hdr.data_size ? s->hdr.data_size - (uint32_t)srcpos : 0;
    if (n > (uint32_t)towrite)
        n = (uint32_t)towrite;
    if (n && pak_read_range(&a->music, s->entry, (uint32_t)sizeof(DSndHeader) + (uint32_t)srcpos, dst, n) != 0)
        return -1;
    return sceMp3NotifyAddStreamData(s->mp3_handle, (SceInt32)n) < 0 ? -1 : 0;
}
#endif

static void stream_close(AudioStream *s)
{
#if PW_PSP
    if (s->mp3_handle >= 0)
        sceMp3ReleaseMp3Handle(s->mp3_handle);
#endif
    s->mp3_handle = -1;
    s->active = 0;
    s->entry = NULL;
    s->chunk = -1;
}

/* reads the header of a pending stream; 0 when unusable */
static int stream_prepare(Audio *a, AudioStream *s)
{
    if (!s->entry || pak_read_range(&a->music, s->entry, 0, &s->hdr, sizeof(DSndHeader)) != 0 ||
        !dsnd_valid(&s->hdr, s->entry->size))
        return 0;
    s->chunk = -1;
    s->mp3_pos = 0;
    s->mp3_done = 0;
    if (s->hdr.codec == DSND_CODEC_ADPCM) {
        uint32_t group = (uint32_t)s->hdr.block_bytes * s->hdr.channels;
        s->chunk_bytes = (AUDIO_STREAM_CHUNK / group) * group;
        return s->chunk_bytes > 0;
    }
#if PW_PSP
    if (a->mp3_ready) {
        SceMp3InitArg arg;
        memset(&arg, 0, sizeof(arg));
        arg.mp3StreamStart = 0;
        arg.mp3StreamEnd = s->hdr.data_size;
        arg.mp3Buf = s->mp3_buf;
        arg.mp3BufSize = MP3_BUF_BYTES;
        arg.pcmBuf = (SceUChar8 *)s->mp3_pcm;
        arg.pcmBufSize = MP3_PCM_BYTES;
        s->mp3_handle = sceMp3ReserveMp3Handle(&arg);
        if (s->mp3_handle < 0 || mp3_feed(a, s) != 0 || sceMp3Init(s->mp3_handle) < 0) {
            stream_close(s);
            return 0;
        }
        sceMp3SetLoopNum(s->mp3_handle, 0);
    }
#endif
    return 1;
}

/* Fills v->pcm with the block holding `frame`; 0 when the source is done. */
static int fetch_block(Audio *a, AudioVoice *v, uint32_t frame)
{
    uint32_t ch;
    if (frame >= v->frames)
        return 0;
    if (v->clip >= 0 || (v->stream >= 0 && v->hdr->codec == DSND_CODEC_ADPCM)) {
        const DSndHeader *h = v->hdr;
        uint32_t bs = h->block_samples, b = frame / bs, group = (uint32_t)h->block_bytes * h->channels;
        const uint8_t *src;
        if (v->clip >= 0) {
            src = v->payload + b * group;
        } else {
            AudioStream *s = &a->streams[v->stream];
            uint32_t per = s->chunk_bytes / group, chunk = b / per;
            if ((int32_t)chunk != s->chunk) {
                uint32_t ofs = chunk * s->chunk_bytes, n = s->chunk_bytes;
                if (ofs >= h->data_size)
                    return 0;
                if (n > h->data_size - ofs)
                    n = h->data_size - ofs;
                if (pak_read_range(&a->music, s->entry, (uint32_t)sizeof(DSndHeader) + ofs, s->buf, n) != 0)
                    return 0;
                s->chunk = (int32_t)chunk;
            }
            src = s->buf + (b % per) * group;
        }
        for (ch = 0; ch < h->channels; ++ch)
            audio_ima_decode(src + ch * h->block_bytes, bs, v->pcm + ch, (int)h->channels);
        v->blk_start = (int32_t)(b * bs);
        v->blk_len = PW_MIN(bs, v->frames - b * bs);
        return 1;
    }
    /* MP3: sequential */
    {
        AudioStream *s = &a->streams[v->stream];
        uint32_t n;
        if (frame < s->mp3_done) {
            /* looped back to the start */
            s->mp3_done = 0;
#if PW_PSP
            if (s->mp3_handle >= 0)
                sceMp3ResetPlayPosition(s->mp3_handle);
#endif
        }
#if PW_PSP
        if (s->mp3_handle >= 0) {
            SceShort16 *pcm;
            int bytes, chans = sceMp3GetMp3ChannelNum(s->mp3_handle);
            if (sceMp3CheckStreamDataNeeded(s->mp3_handle) > 0 && mp3_feed(a, s) != 0)
                return 0;
            bytes = sceMp3Decode(s->mp3_handle, &pcm);
            if (bytes <= 0 || chans < 1)
                return 0;
            n = (uint32_t)bytes / (2u * (uint32_t)chans);
            n = PW_MIN(n, (uint32_t)AUDIO_DECODE_MAX);
            if (chans == (int)v->channels) {
                memcpy(v->pcm, pcm, n * v->channels * sizeof(int16_t));
            } else {
                uint32_t i;
                for (i = 0; i < n; ++i)
                    v->pcm[i * v->channels] = pcm[i * (uint32_t)chans];
            }
            v->blk_start = (int32_t)s->mp3_done;
            v->blk_len = n;
            s->mp3_done += n;
            return n > 0;
        }
#endif
        /* no decoder (host): silence of the right length */
        n = PW_MIN((uint32_t)AUDIO_DECODE_MAX, v->frames - s->mp3_done);
        if (frame >= s->mp3_done + n)
            s->mp3_done = frame - frame % AUDIO_DECODE_MAX;
        n = PW_MIN((uint32_t)AUDIO_DECODE_MAX, v->frames - s->mp3_done);
        memset(v->pcm, 0, n * v->channels * sizeof(int16_t));
        v->blk_start = (int32_t)s->mp3_done;
        v->blk_len = n;
        s->mp3_done += n;
        return n > 0;
    }
}

/* ------------------------------------------------------------------------ */
/* voices                                                                   */
/* ------------------------------------------------------------------------ */
static void end_voice(Audio *a, AudioVoice *v)
{
    if (v->clip >= 0 && a->clips[v->clip].refs)
        a->clips[v->clip].refs--;
    if (v->stream >= 0)
        stream_close(&a->streams[v->stream]);
    v->active = 0;
    v->clip = -1;
    v->stream = -1;
}

static void compute_gains(Audio *a, AudioVoice *v)
{
    float g = v->volume * a->volume[AUDIO_CAT_MASTER] * a->volume[v->cat] * v->fade_gain, pan = 0.0f, l, r;
    if (v->positional) {
        float dx = v->pos[0] - a->listener[0], dy = v->pos[1] - a->listener[1], dz = v->pos[2] - a->listener[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float att = d <= v->min_d ? 1.0f : v->min_d / d;
        if (d >= v->max_d)
            att = 0.0f;
        else if (d > 0.9f * v->max_d)
            att *= (v->max_d - d) / (0.1f * v->max_d);
        g *= att;
        if (d > 1.0f) {
            float rx = cosf(a->listener_yaw), ry = -sinf(a->listener_yaw);
            pan = PW_CLAMP((dx * rx + dy * ry) / d, -1.0f, 1.0f);
        }
    }
    /* equal power, normalised so a centred source plays at full gain */
    l = 1.41421356f * cosf((pan + 1.0f) * 0.78539816f);
    r = 1.41421356f * sinf((pan + 1.0f) * 0.78539816f);
    l = PW_MIN(l, 1.0f);
    r = PW_MIN(r, 1.0f);
    v->tgl = (int32_t)(PW_CLAMP(g * l, 0.0f, 2.0f) * 4096.0f);
    v->tgr = (int32_t)(PW_CLAMP(g * r, 0.0f, 2.0f) * 4096.0f);
}

static int voice_slot(Audio *a)
{
    int i, best = -1;
    int32_t best_gain = 0x7FFFFFFF;
    for (i = 0; i < AUDIO_VOICES; ++i)
        if (!a->voices[i].active)
            return i;
    /* full: steal the quietest one-shot effect */
    for (i = 0; i < AUDIO_VOICES; ++i) {
        AudioVoice *v = &a->voices[i];
        int32_t g = v->tgl + v->tgr;
        if (v->loop || v->stream >= 0)
            continue;
        if (g < best_gain) {
            best_gain = g;
            best = i;
        }
    }
    if (best >= 0) {
        end_voice(a, &a->voices[best]);
        a->steals++;
    }
    return best;
}

static uint32_t start_voice(Audio *a, int clip, int stream, uint32_t file, uint32_t sound, const AudioPlayOpts *o,
                            float volume, float min_d, float max_d)
{
    AudioVoice *v;
    int slot;
    uint32_t handle;
    lock(a);
    slot = voice_slot(a);
    if (slot < 0) {
        unlock(a);
        return 0;
    }
    v = &a->voices[slot];
    memset(v, 0, offsetof(AudioVoice, pcm));
    v->serial = (++a->serial) & 0xFFFFFFu;
    if (!v->serial)
        v->serial = ++a->serial & 0xFFFFFFu;
    v->clip = (int16_t)clip;
    v->stream = (int8_t)stream;
    v->cat = (int8_t)(o && o->cat ? o->cat : AUDIO_CAT_EFFECTS);
    v->sound = sound;
    v->owner = o ? o->owner : 0;
    v->volume = volume * (o && o->volume > 0.0f ? o->volume : 1.0f);
    v->pitch = o && o->pitch > 0.0f ? o->pitch : 1.0f;
    v->loop = o ? o->loop : 0;
    v->positional = o && o->pos;
    if (v->positional)
        memcpy(v->pos, o->pos, sizeof(v->pos));
    v->min_d = PW_MAX(1.0f, min_d);
    v->max_d = PW_MAX(v->min_d + 1.0f, max_d);
    v->fade_gain = 1.0f;
    v->blk_start = -1;
    if (clip >= 0) {
        const DSndHeader *h = (const DSndHeader *)a->clips[clip].data;
        v->hdr = h;
        v->payload = a->clips[clip].data + sizeof(DSndHeader);
        v->rate = h->rate;
        v->channels = h->channels;
        v->frames = h->samples;
        a->clips[clip].refs++;
    } else {
        AudioStream *s = &a->streams[stream];
        if (s->active) {
            int i;
            for (i = 0; i < AUDIO_VOICES; ++i)
                if (i != slot && a->voices[i].active && a->voices[i].stream == stream)
                    end_voice(a, &a->voices[i]);
        }
        s->entry = pak_find(&a->music, file);
        s->active = 1;
        v->hdr = NULL; /* the mixer reads the header (music.dpak belongs to its thread) */
    }
    v->active = 1;
    compute_gains(a, v);
    v->gl = v->tgl;
    v->gr = v->tgr;
    handle = ((uint32_t)v->serial << 8) | (uint32_t)slot;
    unlock(a);
    return handle;
}

uint32_t audio_play_file(Audio *a, uint32_t file, const AudioPlayOpts *o, float min_d, float max_d)
{
    int clip;
    if (!a->ready || !file)
        return 0;
    if (min_d <= 0.0f)
        min_d = a->def_min * a->min_mult;
    if (max_d <= 0.0f)
        max_d = a->def_max * a->max_mult;
    clip = clip_get(a, file);
    if (clip >= 0)
        return start_voice(a, clip, -1, file, 0, o, 1.0f, min_d, max_d);
    if (a->music.handle && pak_find(&a->music, file)) {
        int stream = o && o->cat == AUDIO_CAT_MUSIC ? AUDIO_STREAM_MUSIC : AUDIO_STREAM_SPEECH;
        return start_voice(a, -1, stream, file, 0, o, 1.0f, min_d, max_d);
    }
    a->misses++;
    return 0;
}

uint32_t audio_play_sound(Audio *a, uint32_t sound, const AudioPlayOpts *o)
{
    const DRpgSound *s;
    float mn, mx;
    int clip;
    if (!a->ready || !a->db)
        return 0;
    s = rpgdb_sound(a->db, sound);
    if (!s || !s->file) {
        a->misses++;
        return 0;
    }
    mn = s->min_range;
    mx = s->max_range;
    if (mn == 0.0f && mx == 0.0f) {
        mn = a->def_min;
        mx = a->def_max;
    }
    mn = PW_MAX(1.0f, mn * a->min_mult);
    mx = PW_MAX(mn, mx * a->max_mult);
    clip = clip_get(a, s->file);
    if (clip < 0) {
        a->misses++;
        return 0;
    }
    return start_voice(a, clip, -1, s->file, sound, o, (float)s->volume / 255.0f, mn, mx);
}

static AudioVoice *voice_of(Audio *a, uint32_t handle)
{
    uint32_t slot = handle & 0xFFu;
    AudioVoice *v;
    if (!handle || slot >= AUDIO_VOICES)
        return NULL;
    v = &a->voices[slot];
    return v->active && v->serial == (handle >> 8) ? v : NULL;
}

void audio_stop_handle(Audio *a, uint32_t handle)
{
    AudioVoice *v;
    lock(a);
    v = voice_of(a, handle);
    if (v)
        end_voice(a, v);
    unlock(a);
}

int audio_handle_playing(Audio *a, uint32_t handle)
{
    return voice_of(a, handle) != NULL;
}

void audio_stop(Audio *a, uint32_t owner, uint32_t sound)
{
    int i;
    lock(a);
    for (i = 0; i < AUDIO_VOICES; ++i) {
        AudioVoice *v = &a->voices[i];
        if (v->active && v->cat != AUDIO_CAT_MUSIC && (!owner || v->owner == owner) && (!sound || v->sound == sound))
            end_voice(a, v);
    }
    unlock(a);
}

int audio_playing(Audio *a, uint32_t owner, uint32_t sound)
{
    int i;
    for (i = 0; i < AUDIO_VOICES; ++i) {
        const AudioVoice *v = &a->voices[i];
        if (v->active && (!owner || v->owner == owner) && (!sound || v->sound == sound))
            return 1;
    }
    return 0;
}

uint32_t audio_say(Audio *a, uint32_t owner, uint32_t file, const float *pos)
{
    AudioPlayOpts o;
    memset(&o, 0, sizeof(o));
    o.owner = owner;
    o.pos = pos;
    o.cat = AUDIO_CAT_VOICE;
    return audio_play_file(a, file, &o, 300.0f, 4000.0f);
}

int audio_saying(Audio *a, uint32_t owner)
{
    int i;
    for (i = 0; i < AUDIO_VOICES; ++i) {
        const AudioVoice *v = &a->voices[i];
        if (v->active && v->stream == AUDIO_STREAM_SPEECH && (!owner || v->owner == owner))
            return 1;
    }
    return 0;
}

void audio_music_mode(Audio *a, int mode)
{
    a->music_mode = mode;
}

void audio_stream_music(Audio *a, uint32_t file)
{
    a->music_override = file;
}

void audio_set_listener(Audio *a, const float pos[3], float yaw)
{
    memcpy(a->listener, pos, sizeof(a->listener));
    a->listener_yaw = yaw;
}

void audio_set_volume(Audio *a, int cat, float v)
{
    if (cat >= 0 && cat < AUDIO_CAT_COUNT)
        a->volume[cat] = PW_CLAMP(v, 0.0f, 1.0f);
}

static uint32_t rnd(Audio *a)
{
    a->rng ^= a->rng << 13;
    a->rng ^= a->rng >> 17;
    a->rng ^= a->rng << 5;
    return a->rng;
}

static void update_music(Audio *a)
{
    AudioVoice *cur = voice_of(a, a->music_voice);
    AudioPlayOpts o;
    uint32_t pick = 0, n = 0, i;
    int mode = a->music_mode;
    memset(&o, 0, sizeof(o));
    o.cat = AUDIO_CAT_MUSIC;
    if (a->music_override) {
        if (cur)
            audio_stop_handle(a, a->music_voice);
        a->music_voice = audio_play_file(a, a->music_override, &o, 0.0f, 0.0f);
        a->music_playing_mode = -1;
        a->music_override = 0;
        return;
    }
    if (cur) {
        /* a mode change fades the current track out (StreamMusic tracks finish) */
        if (a->music_playing_mode >= 0 && a->music_playing_mode != mode && cur->fade >= 0.0f)
            cur->fade = -1.0f;
        return;
    }
    if (mode == AUDIO_MUSIC_OFF || !a->track_count)
        return;
    for (i = 0; i < a->track_count; ++i)
        n += a->tracks[i].category == mode;
    if (!n)
        mode = AUDIO_MUSIC_EXPLORE;
    for (i = 0, n = 0; i < a->track_count; ++i)
        n += a->tracks[i].category == mode;
    if (!n)
        return;
    n = rnd(a) % n;
    for (i = 0; i < a->track_count; ++i)
        if (a->tracks[i].category == mode && n-- == 0) {
            pick = a->tracks[i].key;
            break;
        }
    a->music_voice = audio_play_file(a, pick, &o, 0.0f, 0.0f);
    a->music_playing_mode = a->music_mode;
}

void audio_update(Audio *a, float dt)
{
    int i;
    if (!a->ready)
        return;
    lock(a);
    for (i = 0; i < AUDIO_VOICES; ++i) {
        AudioVoice *v = &a->voices[i];
        if (!v->active)
            continue;
        if (v->owner && v->positional && a->locate) {
            float p[3];
            if (a->locate(a->locate_user, v->owner, p))
                memcpy(v->pos, p, sizeof(p));
        }
        if (v->fade != 0.0f) {
            v->fade_gain += v->fade * dt;
            if (v->fade_gain <= 0.0f) {
                end_voice(a, v);
                continue;
            }
            if (v->fade_gain >= 1.0f) {
                v->fade_gain = 1.0f;
                v->fade = 0.0f;
            }
        }
        compute_gains(a, v);
    }
    unlock(a);
    update_music(a);
}

/* ------------------------------------------------------------------------ */
/* mixing                                                                   */
/* ------------------------------------------------------------------------ */
static void mix_voice(Audio *a, AudioVoice *v, uint32_t frames)
{
    int32_t *m = a->mix;
    int32_t gl = v->gl * 16, gr = v->gr * 16;  /* Q16 */
    int32_t dgl = (v->tgl - v->gl) * 16 / (int32_t)frames;
    int32_t dgr = (v->tgr - v->gr) * 16 / (int32_t)frames;
    uint32_t i;
    if (!v->hdr) {
        AudioStream *s = &a->streams[v->stream];
        if (!stream_prepare(a, s)) {
            end_voice(a, v);
            return;
        }
        v->hdr = &s->hdr;
        v->rate = s->hdr.rate;
        v->channels = s->hdr.channels;
        v->frames = s->hdr.samples;
    }
    if (!v->step)
        v->step = (uint32_t)((float)v->rate * v->pitch * 65536.0f / (float)AUDIO_RATE);
    for (i = 0; i < frames; ++i) {
        int32_t l, r;
        uint32_t k;
        if (v->pos_int >= v->frames) {
            if (!v->loop) {
                end_voice(a, v);
                return;
            }
            v->pos_int = 0;
            v->blk_start = -1;
        }
        if (v->blk_start < 0 || v->pos_int < (uint32_t)v->blk_start ||
            v->pos_int >= (uint32_t)v->blk_start + v->blk_len) {
            if (!fetch_block(a, v, v->pos_int)) {
                end_voice(a, v);
                return;
            }
        }
        k = v->pos_int - (uint32_t)v->blk_start;
        {
            int32_t f = (int32_t)(v->pos_frac >> 4); /* Q12 */
            uint32_t k1 = k + 1 < v->blk_len ? k + 1 : k;
            if (v->channels == 1) {
                int32_t s0 = v->pcm[k], s1 = v->pcm[k1];
                l = r = s0 + (((s1 - s0) * f) >> 12);
            } else {
                int32_t s0 = v->pcm[2 * k], s1 = v->pcm[2 * k1];
                int32_t t0 = v->pcm[2 * k + 1], t1 = v->pcm[2 * k1 + 1];
                l = s0 + (((s1 - s0) * f) >> 12);
                r = t0 + (((t1 - t0) * f) >> 12);
            }
        }
        m[2 * i] += (l * (gl / 16)) / 4096;
        m[2 * i + 1] += (r * (gr / 16)) / 4096;
        gl += dgl;
        gr += dgr;
        v->pos_frac += v->step;
        v->pos_int += v->pos_frac >> 16;
        v->pos_frac &= 0xFFFFu;
    }
    v->gl = v->tgl;
    v->gr = v->tgr;
}

void audio_mix(Audio *a, int16_t *out, uint32_t frames)
{
    uint32_t done = 0;
    if (!a->ready) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    lock(a);
    while (done < frames) {
        uint32_t n = PW_MIN(frames - done, (uint32_t)AUDIO_BLOCK), i;
        int k;
        memset(a->mix, 0, n * 2 * sizeof(int32_t));
        for (k = 0; k < AUDIO_VOICES; ++k)
            if (a->voices[k].active)
                mix_voice(a, &a->voices[k], n);
        for (i = 0; i < n * 2; ++i)
            out[done * 2 + i] = (int16_t)PW_CLAMP(a->mix[i], -32768, 32767);
        done += n;
    }
    a->frames_mixed += frames;
    unlock(a);
}
