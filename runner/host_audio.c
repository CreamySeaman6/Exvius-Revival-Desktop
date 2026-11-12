// Sound output. The game's audio engine hands over finished PCM buffers per player; each player gets a
// converter to the device format, and the device callback mixes whatever the players have ready.
#include <string.h>

#include <SDL.h>

#include "runner.h"
#include "ui.h"

#define MAXP 32
typedef struct { int used, playing; SDL_AudioStream *stream; float volume; int rate, channels, bits; unsigned long written; int had, dry, primed, last_avail, still; } Player;
static Player players[MAXP];
static SDL_AudioDeviceID dev;
static SDL_AudioSpec have;
static FILE *dump;
static SDL_atomic_t master, muted;                          // master volume in percent; muted silences without forgetting it

static SDL_atomic_t gaps, low_water;                       // for the runner's periodic report
static int reserve_bytes;                                   // how much sound a player collects before it starts
int audio_gaps(void) { return SDL_AtomicSet(&gaps, 0); }
int audio_low_ms(void) {                                    // the least sound any playing stream had in hand, since the last call
    int b = SDL_AtomicSet(&low_water, 1 << 30), per_sec = have.freq * have.channels * 2;
    return b == 1 << 30 || !per_sec ? -1 : (int)((int64_t)b * 1000 / per_sec);
}
static void mix(void *ud, Uint8 *out, int len) {
    (void)ud;
    static Uint8 tmp[32768];
    memset(out, 0, (size_t)len);
    for (int i = 0; i < MAXP; i++) {
        Player *p = &players[i];
        if (!p->used || !p->playing) continue;
        int avail = SDL_AudioStreamAvailable(p->stream), done = 0;
        if (!p->primed) {
            // The game hands sound over in real time, a little at a time, so a stream that starts playing the moment
            // its first piece arrives has nothing in hand and every small delay is heard. Let a reserve build up
            // first. A short sound never reaches the reserve: it starts once no more of it is arriving.
            p->still = avail > 0 && avail == p->last_avail ? p->still + 1 : 0;
            p->last_avail = avail;
            if (avail >= reserve_bytes || p->still >= 2) p->primed = 1;
            else continue;
        }
        if (avail < SDL_AtomicGet(&low_water)) SDL_AtomicSet(&low_water, avail);
        while (done < len) {
            int want = len - done > (int)sizeof tmp ? (int)sizeof tmp : len - done;
            int got = SDL_AudioStreamGet(p->stream, tmp, want);
            if (got <= 0) break;
            SDL_MixAudioFormat(out + done, tmp, have.format, (Uint32)got, (int)(p->volume * SDL_MIX_MAXVOLUME));
            done += got;
        }
        // A sound that stops for a moment and then carries on is what a listener hears as a hiccup: count those.
        if (done == len) { if (p->had && p->dry > 0 && p->dry < 50) SDL_AtomicAdd(&gaps, 1); p->had = 1; p->dry = 0; }
        else if (p->had) p->dry++;
        if (done < len) { p->primed = 0; p->last_avail = 0; p->still = 0; }    // ran dry: collect a reserve again
    }
    // Master volume. The game's own output is at full scale, which a phone tames with its media volume; here it would
    // play at whatever the PC is set to, so it is turned down by default.
    int gain = SDL_AtomicGet(&muted) ? 0 : SDL_AtomicGet(&master);    // 0..100
    if (gain != 100) {
        Sint16 *s = (Sint16 *)out;
        for (int i = 0, n = len / 2; i < n; i++) s[i] = (Sint16)(s[i] * gain / 100);
    }
    if (dump) fwrite(out, 1, (size_t)len, dump);
}
// Volume in percent: FFBE_VOLUME if set (tests), else the saved setting, else 25. "Muted" silences without forgetting it.
void audio_load_settings(void) {
    int v = cfg_int("volume", -1);
    if (v < 0) {                                            // from before the settings file: the value F9/F10 used to save
        v = 25;
        char p[1100]; SDL_snprintf(p, sizeof p, "%s/volume.txt", g_root);
        FILE *f = os_fopen(p, "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = 25; fclose(f); }
    }
    const char *e = getenv("FFBE_VOLUME");
    if (e && *e) v = atoi(e);
    SDL_AtomicSet(&master, v < 0 ? 0 : v > 100 ? 100 : v);
    SDL_AtomicSet(&muted, cfg_int("muted", 0) != 0);
}
int audio_master(void) { return SDL_AtomicGet(&master); }
void audio_set_master(int v) { SDL_AtomicSet(&master, v < 0 ? 0 : v > 100 ? 100 : v); }
int audio_muted(void) { return SDL_AtomicGet(&muted); }
void audio_set_muted(int on) { SDL_AtomicSet(&muted, on != 0); }
static void ensure_device(void) {
    if (dev) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { rlog("[audio] no sound: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { rlog("[audio] no sound device: %s\n", SDL_GetError()); return; }
    if (getenv("FFBE_AUDIO_DUMP")) dump = fopen(getenv("FFBE_AUDIO_DUMP"), "wb");
    int ms = getenv("FFBE_AUDIO_RESERVE") ? atoi(getenv("FFBE_AUDIO_RESERVE")) : 60;
    reserve_bytes = (int)((int64_t)have.freq * have.channels * 2 * (ms < 0 ? 0 : ms > 500 ? 500 : ms) / 1000);
    SDL_AtomicSet(&low_water, 1 << 30);
    rlog("[audio] device %d Hz, %d channels (%s), volume %d%%\n", have.freq, have.channels, SDL_GetCurrentAudioDriver(), SDL_AtomicGet(&master));
    SDL_PauseAudioDevice(dev, 0);
}
int audio_open(int rate, int channels, int bits) {
    ensure_device();
    if (!dev) return -1;
    for (int i = 0; i < MAXP; i++) if (!players[i].used) {
        Player *p = &players[i];
        SDL_AudioFormat fmt = bits == 8 ? AUDIO_U8 : bits == 32 ? AUDIO_F32SYS : AUDIO_S16SYS;
        SDL_AudioStream *s = SDL_NewAudioStream(fmt, (Uint8)channels, rate, have.format, have.channels, have.freq);
        if (!s) return -1;
        SDL_LockAudioDevice(dev);
        memset(p, 0, sizeof *p);
        p->stream = s; p->volume = 1.0f; p->rate = rate; p->channels = channels; p->bits = bits; p->playing = 0; p->used = 1;
        SDL_UnlockAudioDevice(dev);
        rlog("[audio] player %d: %d Hz, %d channels, %d bits\n", i, rate, channels, bits);
        return i;
    }
    return -1;
}
int audio_write(int id, const void *data, size_t bytes) {
    if (id < 0 || id >= MAXP || !players[id].used) return -1;
    SDL_LockAudioDevice(dev);
    SDL_AudioStreamPut(players[id].stream, data, (int)bytes);
    players[id].written += bytes;
    SDL_UnlockAudioDevice(dev);
    return 0;
}
int audio_queued_ms(int id) {
    if (id < 0 || id >= MAXP || !players[id].used) return 0;
    SDL_LockAudioDevice(dev);
    int avail = SDL_AudioStreamAvailable(players[id].stream);
    SDL_UnlockAudioDevice(dev);
    int bytes_per_sec = have.freq * have.channels * (SDL_AUDIO_BITSIZE(have.format) / 8);
    return bytes_per_sec ? (int)((int64_t)avail * 1000 / bytes_per_sec) : 0;
}
void audio_close(int id) {
    if (id < 0 || id >= MAXP || !players[id].used) return;
    SDL_LockAudioDevice(dev);
    players[id].used = 0;
    SDL_FreeAudioStream(players[id].stream);
    SDL_UnlockAudioDevice(dev);
}
void audio_ctl(int id, int what, int value) {
    if (id < 0 || id >= MAXP || !players[id].used) return;
    SDL_LockAudioDevice(dev);
    if (what == 0) players[id].playing = value;
    if (what == 1) { float db = value / 100.0f; players[id].volume = value <= -9600 ? 0.0f : SDL_powf(10.0f, db / 20.0f); if (players[id].volume > 1) players[id].volume = 1; }
    if (what == 2) SDL_AudioStreamClear(players[id].stream);
    SDL_UnlockAudioDevice(dev);
}
