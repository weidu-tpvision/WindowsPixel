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
static pthread_cond_t g_audio_cv = PTHREAD_COND_INITIALIZER;
/* Set by hv_audio_shutdown, cleared by hv_audio_attach_hda: once the VM is going down no stream may
   be (re)opened, because the callbacks' userData is the HvHda that hv_winvm_destroy frees. */
static int g_audio_closed = 0;
static int g_worker_running = 0;
static pthread_t g_worker_tid;

/* One direction's stream. Opening/closing AAudio streams can take tens of ms (and AAudio forbids
   reopening inside its error callback), so vCPU MMIO handlers and disconnect callbacks only post
   desired state here and wake g_worker_tid to reconcile streams off the vCPU thread. */
typedef struct AudioDir {
    aaudio_direction_t dir;
    const char *name;
    AAudioStream_dataCallback cb;
    AAudioStream *stream;
    int running;
    unsigned ch, rate;
    HvHda *hda;
    /* Desired state (guarded by g_audio_mu) */
    int want_run;
    int reopen_req;
    unsigned want_ch, want_rate;
    HvHda *want_hda;
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

static AudioDir g_out = {AAUDIO_DIRECTION_OUTPUT, "output", out_data_cb, NULL, 0, 0, 0, NULL, 0, 0, 0, 0, NULL};
static AudioDir g_in = {AAUDIO_DIRECTION_INPUT, "input", in_data_cb, NULL, 0, 0, 0, NULL, 0, 0, 0, 0, NULL};

static void error_cb(AAudioStream *stream, void *userData, aaudio_result_t error) {
    AudioDir *d = (AudioDir *)userData;
    LOGW("AAudio %s stream error %d", d->name, error);
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    pthread_mutex_lock(&g_audio_mu);
    if (d->stream == stream && !g_audio_closed) {
        d->reopen_req = 1;
        pthread_cond_signal(&g_audio_cv);
    }
    pthread_mutex_unlock(&g_audio_mu);
}

static void close_stream_unlocked(AAudioStream *s, int was_running) {
    if (!s) return;
    if (was_running) AAudioStream_requestStop(s);
    AAudioStream_close(s);
}

static int dir_needs_work_locked(const AudioDir *d) {
    if (d->reopen_req && d->stream) return 1;
    if (d->want_run) {
        if (!d->stream || !d->running) return 1;
        if (d->want_ch != d->ch || d->want_rate != d->rate || d->want_hda != d->hda) return 1;
    } else if (d->running && d->stream) {
        return 1;
    }
    return 0;
}

/* Reconciles one direction; unlocks g_audio_mu across blocking AAudio calls so vCPU threads never stall. */
static void reconcile_dir_locked(AudioDir *d) {
    if (g_audio_closed) return;

    int need_reopen = d->reopen_req ||
                      (d->stream && d->want_run &&
                       (d->want_ch != d->ch || d->want_rate != d->rate || d->want_hda != d->hda));
    d->reopen_req = 0;

    if (need_reopen && d->stream) {
        AAudioStream *old = d->stream;
        int was_run = d->running;
        d->stream = NULL;
        d->running = 0;
        pthread_mutex_unlock(&g_audio_mu);
        close_stream_unlocked(old, was_run);
        pthread_mutex_lock(&g_audio_mu);
        if (g_audio_closed) return;
    }

    if (!d->want_run) {
        if (d->stream && d->running) {
            AAudioStream *s = d->stream;
            d->running = 0;
            pthread_mutex_unlock(&g_audio_mu);
            AAudioStream_requestStop(s);
            pthread_mutex_lock(&g_audio_mu);
        }
        return;
    }

    if (!d->stream) {
        unsigned ch = d->want_ch, rate = d->want_rate;
        HvHda *hda = d->want_hda;
        pthread_mutex_unlock(&g_audio_mu);

        AAudioStream *opened = NULL;
        AAudioStreamBuilder *b = NULL;
        aaudio_result_t rc = AAudio_createStreamBuilder(&b);
        if (rc == AAUDIO_OK && b) {
            AAudioStreamBuilder_setDirection(b, d->dir);
            AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setChannelCount(b, (int32_t)ch);
            AAudioStreamBuilder_setSampleRate(b, (int32_t)rate);
            AAudioStreamBuilder_setDataCallback(b, d->cb, hda);
            AAudioStreamBuilder_setErrorCallback(b, error_cb, d);
            rc = AAudioStreamBuilder_openStream(b, &opened);
            AAudioStreamBuilder_delete(b);
        }

        pthread_mutex_lock(&g_audio_mu);
        if (rc != AAUDIO_OK || !opened) {
            LOGW("AAudio %s openStream failed: %d%s", d->name, rc,
                 d->dir == AAUDIO_DIRECTION_INPUT ? " (RECORD_AUDIO permission pending?)" : "");
            /* Avoid spinning if open fails (e.g. mic permission denied); next guest start retries. */
            d->want_run = 0;
            return;
        }
        if (g_audio_closed || d->stream) {
            pthread_mutex_unlock(&g_audio_mu);
            close_stream_unlocked(opened, 0);
            pthread_mutex_lock(&g_audio_mu);
            return;
        }
        d->stream = opened;
        d->ch = ch;
        d->rate = rate;
        d->hda = hda;
    }

    if (d->want_run && d->stream && !d->running) {
        AAudioStream *s = d->stream;
        unsigned ch = d->ch, rate = d->rate;
        pthread_mutex_unlock(&g_audio_mu);
        aaudio_result_t rc = AAudioStream_requestStart(s);
        pthread_mutex_lock(&g_audio_mu);
        if (g_audio_closed || d->stream != s) return;
        if (rc == AAUDIO_OK) {
            d->running = 1;
            LOGI("AAudio %s started (%u ch @ %u Hz)", d->name, ch, rate);
        } else {
            LOGW("AAudio %s requestStart failed: %d", d->name, rc);
            d->want_run = 0;
        }
    }
}

static void *audio_worker_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_audio_mu);
    while (!g_audio_closed) {
        if (!dir_needs_work_locked(&g_out) && !dir_needs_work_locked(&g_in)) {
            pthread_cond_wait(&g_audio_cv, &g_audio_mu);
            continue;
        }
        if (dir_needs_work_locked(&g_out)) reconcile_dir_locked(&g_out);
        if (!g_audio_closed && dir_needs_work_locked(&g_in)) reconcile_dir_locked(&g_in);
    }
    pthread_mutex_unlock(&g_audio_mu);
    return NULL;
}

static void ensure_worker_locked(void) {
    if (!g_worker_running && !g_audio_closed) {
        if (pthread_create(&g_worker_tid, NULL, audio_worker_thread, NULL) == 0)
            g_worker_running = 1;
    }
}

static void sanitize(unsigned *channels, unsigned *rate_hz) {
    if (*channels < 1 || *channels > 2) *channels = 2;
    if (*rate_hz < 8000 || *rate_hz > 192000) *rate_hz = 48000;
}

static int request_start_dir(AudioDir *d, HvHda *hda, unsigned channels, unsigned rate_hz) {
    sanitize(&channels, &rate_hz);
    pthread_mutex_lock(&g_audio_mu);
    if (g_audio_closed) {
        pthread_mutex_unlock(&g_audio_mu);
        return -1;
    }
    d->want_hda = hda;
    d->want_ch = channels;
    d->want_rate = rate_hz;
    d->want_run = 1;
    ensure_worker_locked();
    pthread_cond_signal(&g_audio_cv);
    pthread_mutex_unlock(&g_audio_mu);
    return 0;
}

static int hda_start_out(void *opaque, unsigned channels, unsigned rate_hz) {
    return request_start_dir(&g_out, (HvHda *)opaque, channels, rate_hz);
}

static int hda_start_in(void *opaque, unsigned channels, unsigned rate_hz) {
    return request_start_dir(&g_in, (HvHda *)opaque, channels, rate_hz);
}

static void stop_dir(AudioDir *d) {
    pthread_mutex_lock(&g_audio_mu);
    d->want_run = 0;
    pthread_cond_signal(&g_audio_cv);
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
    g_out.want_run = g_out.reopen_req = 0;
    g_in.want_run = g_in.reopen_req = 0;
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
    g_out.want_run = g_out.reopen_req = 0;
    g_in.want_run = g_in.reopen_req = 0;
    int join_worker = g_worker_running;
    pthread_t tid = g_worker_tid;
    g_worker_running = 0;
    pthread_cond_broadcast(&g_audio_cv);
    pthread_mutex_unlock(&g_audio_mu);

    if (join_worker) pthread_join(tid, NULL);

    pthread_mutex_lock(&g_audio_mu);
    AAudioStream *so = g_out.stream, *si = g_in.stream;
    int ro = g_out.running, ri = g_in.running;
    g_out.stream = NULL; g_out.running = 0; g_out.hda = NULL;
    g_in.stream = NULL;  g_in.running = 0;  g_in.hda = NULL;
    pthread_mutex_unlock(&g_audio_mu);

    close_stream_unlocked(so, ro);
    close_stream_unlocked(si, ri);
}

void hv_audio_attach(HvVirtioSnd *d) {
    (void)d;
}

#endif /* __ANDROID__ */
