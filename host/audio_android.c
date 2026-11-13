/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * audio_android.c — Full-duplex Android AAudio backend for Intel ICH6 HD Audio (devices/hda.c).
 *   - Playback (Speaker Out): AAudio low-latency PCM_I16 stream pulling from HvHda TX ring.
 *   - Capture (Microphone In): AAudio low-latency PCM_I16 input stream pushing to HvHda RX ring.
 */
#include "audio_mac.h"

#ifdef __ANDROID__

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "winvm-audio", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "winvm-audio", __VA_ARGS__)

static pthread_mutex_t g_audio_mu = PTHREAD_MUTEX_INITIALIZER;
/* Set by hv_audio_shutdown, cleared by hv_audio_attach_hda: once the VM is going down no stream may
   be (re)opened, because the callbacks' userData is the HvHda that hv_winvm_destroy frees. */
static int g_audio_closed = 0;

/* One direction's stream. AAudio streams die on route changes (headphones, Bluetooth): the error
   callback then reopens it with the same parameters, from a fresh thread as AAudio requires. */
typedef struct AudioDir {
    aaudio_direction_t dir;
    const char *name;
    AAudioStream_dataCallback cb;
    AAudioStream *stream;
    int running;
    unsigned ch, rate;
    HvHda *hda;
} AudioDir;

static aaudio_data_callback_result_t out_data_cb(AAudioStream *stream, void *userData,
                                                 void *audioData, int32_t numFrames) {
    HvHda *hda = (HvHda *)userData;
    int32_t ch = AAudioStream_getChannelCount(stream);
    if (ch < 1) ch = 2;
    size_t bytes = (size_t)numFrames * (size_t)ch * sizeof(int16_t);
    if (hda && audioData && bytes > 0)
        hv_hda_pull_playback(hda, audioData, bytes);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t in_data_cb(AAudioStream *stream, void *userData,
                                                void *audioData, int32_t numFrames) {
    HvHda *hda = (HvHda *)userData;
    int32_t ch = AAudioStream_getChannelCount(stream);
    if (ch < 1) ch = 2;
    size_t bytes = (size_t)numFrames * (size_t)ch * sizeof(int16_t);
    if (hda && audioData && bytes > 0)
        hv_hda_push_capture(hda, audioData, bytes);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static AudioDir g_out = {AAUDIO_DIRECTION_OUTPUT, "output", out_data_cb, NULL, 0, 0, 0, NULL};
static AudioDir g_in = {AAUDIO_DIRECTION_INPUT, "input", in_data_cb, NULL, 0, 0, 0, NULL};

static void error_cb(AAudioStream *stream, void *userData, aaudio_result_t error);

static void close_locked(AudioDir *d) {
    if (!d->stream) return;
    if (d->running) AAudioStream_requestStop(d->stream);
    AAudioStream_close(d->stream);
    d->stream = NULL;
    d->running = 0;
}

/* g_audio_mu held. Opens (if needed) and starts d's stream; 0 if it is running. */
static int start_locked(AudioDir *d, HvHda *hda, unsigned channels, unsigned rate_hz) {
    if (g_audio_closed) return -1;
    if (d->stream && (channels != d->ch || rate_hz != d->rate || hda != d->hda)) close_locked(d);
    if (!d->stream) {
        AAudioStreamBuilder *b = NULL;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return -1;
        AAudioStreamBuilder_setDirection(b, d->dir);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b, (int32_t)channels);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)rate_hz);
        AAudioStreamBuilder_setDataCallback(b, d->cb, hda);
        AAudioStreamBuilder_setErrorCallback(b, error_cb, d);
        aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &d->stream);
        AAudioStreamBuilder_delete(b);
        if (rc != AAUDIO_OK || !d->stream) {
            LOGW("AAudio %s openStream failed: %d%s", d->name, rc,
                 d->dir == AAUDIO_DIRECTION_INPUT ? " (RECORD_AUDIO permission pending?)" : "");
            d->stream = NULL;
            return -1;
        }
        d->ch = channels;
        d->rate = rate_hz;
        d->hda = hda;
    }
    if (!d->running) {
        aaudio_result_t rc = AAudioStream_requestStart(d->stream);
        if (rc == AAUDIO_OK) {
            d->running = 1;
            LOGI("AAudio %s started (%u ch @ %u Hz)", d->name, channels, rate_hz);
        } else {
            LOGW("AAudio %s requestStart failed: %d", d->name, rc);
        }
    }
    return d->running ? 0 : -1;
}

typedef struct Reopen {
    AudioDir *d;
    AAudioStream *dead;
} Reopen;

static void *reopen_thread(void *arg) {
    Reopen r = *(Reopen *)arg;
    free(arg);
    pthread_mutex_lock(&g_audio_mu);
    /* Only if the dead stream is still the current one, and the guest still wants it running. */
    if (r.d->stream == r.dead && !g_audio_closed) {
        int was_running = r.d->running;
        close_locked(r.d);
        if (was_running && start_locked(r.d, r.d->hda, r.d->ch, r.d->rate) == 0)
            LOGI("AAudio %s reopened after disconnect", r.d->name);
    }
    pthread_mutex_unlock(&g_audio_mu);
    return NULL;
}

static void error_cb(AAudioStream *stream, void *userData, aaudio_result_t error) {
    LOGW("AAudio %s stream error %d", ((AudioDir *)userData)->name, error);
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    Reopen *r = malloc(sizeof *r);
    if (!r) return;
    r->d = (AudioDir *)userData;
    r->dead = stream;
    pthread_t t;
    if (pthread_create(&t, NULL, reopen_thread, r) == 0) pthread_detach(t);
    else free(r);
}

static void sanitize(unsigned *channels, unsigned *rate_hz) {
    if (*channels < 1 || *channels > 2) *channels = 2;
    if (*rate_hz < 8000 || *rate_hz > 192000) *rate_hz = 48000;
}

static int hda_start_out(void *opaque, unsigned channels, unsigned rate_hz) {
    sanitize(&channels, &rate_hz);
    pthread_mutex_lock(&g_audio_mu);
    int ok = start_locked(&g_out, (HvHda *)opaque, channels, rate_hz);
    pthread_mutex_unlock(&g_audio_mu);
    return ok;
}

static int hda_start_in(void *opaque, unsigned channels, unsigned rate_hz) {
    sanitize(&channels, &rate_hz);
    pthread_mutex_lock(&g_audio_mu);
    int ok = start_locked(&g_in, (HvHda *)opaque, channels, rate_hz);
    pthread_mutex_unlock(&g_audio_mu);
    return ok;
}

static void stop_dir(AudioDir *d) {
    pthread_mutex_lock(&g_audio_mu);
    if (d->stream && d->running) {
        AAudioStream_requestStop(d->stream);
        d->running = 0;
    }
    pthread_mutex_unlock(&g_audio_mu);
}

static void hda_stop_out(void *opaque) {
    (void)opaque;
    stop_dir(&g_out);
}

static void hda_stop_in(void *opaque) {
    (void)opaque;
    stop_dir(&g_in);
}

void hv_audio_attach_hda(HvHda *hda) {
    if (!hda) return;
    pthread_mutex_lock(&g_audio_mu);
    g_audio_closed = 0;
    pthread_mutex_unlock(&g_audio_mu);
    hda->backend = (HvHdaBackend){
        .start_out = hda_start_out,
        .stop_out = hda_stop_out,
        .start_in = hda_start_in,
        .stop_in = hda_stop_in,
        .opaque = hda,
    };
}

void hv_audio_shutdown(void) {
    pthread_mutex_lock(&g_audio_mu);
    g_audio_closed = 1;
    close_locked(&g_out);
    close_locked(&g_in);
    pthread_mutex_unlock(&g_audio_mu);
}

void hv_audio_attach(HvVirtioSnd *d) {
    (void)d;
}

#endif /* __ANDROID__ */
