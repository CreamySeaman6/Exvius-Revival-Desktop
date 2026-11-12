// OpenSL ES, as far as the game's audio engine uses it: an engine, an output mix, and PCM players fed through
// a buffer queue. Each player forwards its buffers to the runner's sound output and gets its "buffer finished"
// callback when the runner has played that much.
#define _GNU_SOURCE
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shared.h"
#include "hyper.h"

typedef struct Itf { void **vtbl; struct Player *pl; int kind; } Itf;
typedef struct Player {
    Itf obj, play, bq, vol, other;
    int host, rate, channels, bits, state, alive;
    void (*cb)(void *itf, void *ctx); void *ctx;
    pthread_t thread; pthread_mutex_t lock;
    int fifo_ms[64], head, count, total_ms; unsigned played;
    int volume;
} Player;

static void *obj_vtbl[12], *engine_vtbl[20], *play_vtbl[14], *bq_vtbl[6], *vol_vtbl[12], *any_vtbl[32];
void *SL_IID_tab[8];
static struct { char b[16]; } iid_store[8];
enum { IID_ASBQ, IID_BQ, IID_ENGINE, IID_META, IID_PLAY, IID_PREFETCH, IID_SEEK, IID_VOLUME };
static Itf engine_obj, engine_itf, mix_obj, generic_itf;
static int n_players;

static uint32_t ok(void) { return 0; }
static uint32_t unsupported(void) { return 12; }

#define CUSHION_MS 120
static void *pump(void *arg) {
    Player *p = arg;
    struct timespec nap = {0, 2000000};
    hc(HC_THREAD_URGENT, 1, 0, 0, 0);
    while (p->alive) {
        int fire = 0;
        pthread_mutex_lock(&p->lock);
        if (p->count > 0 && p->state == 3) {
            int queued = (int)hc(HC_AUDIO_QUEUED, p->host, 0, 0, 0);
            // Keep CUSHION_MS more sound waiting on the host than the game thinks is queued, so short pauses in the
            // game (loading, a busy frame) do not run the output dry.
            if (queued <= p->total_ms - p->fifo_ms[p->head] + CUSHION_MS) {
                p->total_ms -= p->fifo_ms[p->head];
                p->head = (p->head + 1) % 64; p->count--; p->played++;
                fire = 1;
            }
        }
        pthread_mutex_unlock(&p->lock);
        if (fire && p->cb) p->cb(&p->bq, p->ctx);
        else nanosleep(&nap, NULL);
    }
    return NULL;
}

// ---- object ----
static uint32_t obj_Realize(Itf *self, uint32_t async) {
    (void)async;
    Player *p = self->pl;
    if (p && p->host < 0) {
        p->host = (int)hc(HC_AUDIO_OPEN, p->rate, p->channels, p->bits, 0);
        p->alive = 1;
        pthread_create(&p->thread, NULL, pump, p);
    }
    return 0;
}
static uint32_t obj_GetState(Itf *self, uint32_t *state) { (void)self; *state = 2; return 0; }
static uint32_t obj_GetInterface(Itf *self, void *iid, void **out) {
    Player *p = self->pl;
    if (self == &engine_obj && iid == SL_IID_tab[IID_ENGINE]) { *out = &engine_itf; return 0; }
    if (p) {
        if (iid == SL_IID_tab[IID_PLAY]) { *out = &p->play; return 0; }
        if (iid == SL_IID_tab[IID_ASBQ] || iid == SL_IID_tab[IID_BQ]) { *out = &p->bq; return 0; }
        if (iid == SL_IID_tab[IID_VOLUME]) { *out = &p->vol; return 0; }
    }
    if (self == &mix_obj) { *out = &generic_itf; return 0; }
    *out = NULL;
    return 12;
}
static void obj_Destroy(Itf *self) {
    Player *p = self->pl;
    if (!p) return;
    p->alive = 0;
    if (p->host >= 0) { pthread_join(p->thread, NULL); hc(HC_AUDIO_CLOSE, p->host, 0, 0, 0); }
    free(p);
}

// ---- engine ----
typedef struct { uint32_t type, a, b; } Locator;
typedef struct { uint32_t type, channels, rate_mhz, bits, container, mask, endian, repr; } Format;
typedef struct { void *locator, *format; } DataSource;

static uint32_t eng_CreateAudioPlayer(Itf *self, void **out, DataSource *src, void *sink, uint32_t n, void **ids, uint32_t *req) {
    (void)self; (void)sink; (void)n; (void)ids; (void)req;
    Locator *loc = src->locator; Format *fmt = src->format;
    if ((loc->type != 0x800007BD && loc->type != 6) || !fmt || (fmt->type != 2 && fmt->type != 4)) {
        logf_("[audio] player with locator type 0x%x is not supported (only PCM buffer queues are)\n", loc->type);
        *out = NULL;
        return 11;
    }
    Player *p = calloc(1, sizeof *p);
    p->obj.vtbl = obj_vtbl; p->play.vtbl = play_vtbl; p->bq.vtbl = bq_vtbl; p->vol.vtbl = vol_vtbl;
    p->obj.pl = p->play.pl = p->bq.pl = p->vol.pl = p;
    p->rate = (int)(fmt->rate_mhz / 1000); p->channels = (int)fmt->channels; p->bits = (int)fmt->bits;
    if (fmt->type == 4 && fmt->repr == 3) p->bits = 32;
    p->host = -1; p->state = 1;
    pthread_mutex_init(&p->lock, NULL);
    n_players++;
    if (g_verbose) logf_("[audio] new PCM player #%d: %d Hz, %d ch, %d bit, %u buffers\n", n_players, p->rate, p->channels, p->bits, loc->a);
    *out = &p->obj;
    return 0;
}
static uint32_t eng_CreateOutputMix(Itf *self, void **out) { (void)self; *out = &mix_obj; return 0; }

// ---- play ----
static uint32_t play_SetPlayState(Itf *self, uint32_t state) {
    Player *p = self->pl;
    pthread_mutex_lock(&p->lock);
    p->state = (int)state;
    pthread_mutex_unlock(&p->lock);
    hc(HC_AUDIO_CTL, p->host, 0, state == 3, 0);
    return 0;
}
static uint32_t play_GetPlayState(Itf *self, uint32_t *state) { *state = (uint32_t)self->pl->state; return 0; }
static uint32_t play_GetU32(Itf *self, uint32_t *out) { (void)self; *out = 0; return 0; }

// ---- buffer queue ----
static uint32_t bq_Enqueue(Itf *self, const void *buf, uint32_t size) {
    Player *p = self->pl;
    int bytes_per_ms = p->rate * p->channels * (p->bits / 8) / 1000;
    int ms = bytes_per_ms ? (int)(size / (uint32_t)bytes_per_ms) : 0;
    pthread_mutex_lock(&p->lock);
    if (p->count >= 64) { pthread_mutex_unlock(&p->lock); return 7; }
    p->fifo_ms[(p->head + p->count) % 64] = ms; p->count++; p->total_ms += ms;
    pthread_mutex_unlock(&p->lock);
    hc(HC_AUDIO_WRITE, p->host, (long)buf, size, 0);
    return 0;
}
static uint32_t bq_Clear(Itf *self) {
    Player *p = self->pl;
    pthread_mutex_lock(&p->lock);
    p->count = 0; p->total_ms = 0;
    pthread_mutex_unlock(&p->lock);
    hc(HC_AUDIO_CTL, p->host, 2, 0, 0);
    return 0;
}
static uint32_t bq_GetState(Itf *self, uint32_t *state) { state[0] = (uint32_t)self->pl->count; state[1] = self->pl->played; return 0; }
static uint32_t bq_RegisterCallback(Itf *self, void *cb, void *ctx) { self->pl->cb = cb; self->pl->ctx = ctx; return 0; }

// ---- volume ----
static uint32_t vol_Set(Itf *self, int16_t mb) { self->pl->volume = mb; hc(HC_AUDIO_CTL, self->pl->host, 1, mb, 0); return 0; }
static uint32_t vol_Get(Itf *self, int16_t *mb) { *mb = (int16_t)self->pl->volume; return 0; }
static uint32_t vol_GetMax(Itf *self, int16_t *mb) { (void)self; *mb = 0; return 0; }

uint32_t real_slCreateEngine(void **out) {
    for (int i = 0; i < 12; i++) obj_vtbl[i] = ok;
    obj_vtbl[0] = obj_Realize; obj_vtbl[2] = obj_GetState; obj_vtbl[3] = obj_GetInterface; obj_vtbl[6] = obj_Destroy;
    for (int i = 0; i < 20; i++) engine_vtbl[i] = unsupported;
    engine_vtbl[2] = eng_CreateAudioPlayer; engine_vtbl[7] = eng_CreateOutputMix;
    for (int i = 0; i < 14; i++) play_vtbl[i] = ok;
    play_vtbl[0] = play_SetPlayState; play_vtbl[1] = play_GetPlayState; play_vtbl[2] = play_GetU32; play_vtbl[3] = play_GetU32;
    bq_vtbl[0] = bq_Enqueue; bq_vtbl[1] = bq_Clear; bq_vtbl[2] = bq_GetState; bq_vtbl[3] = bq_RegisterCallback;
    for (int i = 0; i < 12; i++) vol_vtbl[i] = ok;
    vol_vtbl[0] = vol_Set; vol_vtbl[1] = vol_Get; vol_vtbl[2] = vol_GetMax;
    for (int i = 0; i < 32; i++) any_vtbl[i] = ok;
    engine_obj.vtbl = obj_vtbl; mix_obj.vtbl = obj_vtbl; engine_itf.vtbl = engine_vtbl; generic_itf.vtbl = any_vtbl;
    logf_("[audio] sound engine created\n");
    *out = &engine_obj;
    return 0;
}
void sl_init_iids(void) { for (int i = 0; i < 8; i++) SL_IID_tab[i] = &iid_store[i]; }
