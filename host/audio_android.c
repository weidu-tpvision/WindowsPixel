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

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "winvm-audio", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "winvm-audio", __VA_ARGS__)

static pthread_mutex_t g_audio_mu = PTHREAD_MUTEX_INITIALIZER;

static AAudioStream *g_out_stream = NULL;
static int g_out_running = 0;
static unsigned g_out_ch = 0, g_out_rate = 0;

static AAudioStream *g_in_stream = NULL;
static int g_in_running = 0;
static unsigned g_in_ch = 0, g_in_rate = 0;

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

static int hda_start_out(void *opaque, unsigned channels, unsigned rate_hz) {
    HvHda *hda = (HvHda *)opaque;
    if (channels < 1 || channels > 2) channels = 2;
    if (rate_hz < 8000 || rate_hz > 192000) rate_hz = 48000;

    pthread_mutex_lock(&g_audio_mu);
    if (g_out_stream && (channels != g_out_ch || rate_hz != g_out_rate)) {
        AAudioStream_requestStop(g_out_stream);
        AAudioStream_close(g_out_stream);
        g_out_stream = NULL;
        g_out_running = 0;
    }

    if (!g_out_stream) {
        AAudioStreamBuilder *b = NULL;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) {
            pthread_mutex_unlock(&g_audio_mu);
            return -1;
        }
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b, (int32_t)channels);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)rate_hz);
        AAudioStreamBuilder_setDataCallback(b, out_data_cb, hda);

        aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &g_out_stream);
        AAudioStreamBuilder_delete(b);
        if (rc != AAUDIO_OK || !g_out_stream) {
            LOGW("AAudio output openStream failed: %d", rc);
            g_out_stream = NULL;
            pthread_mutex_unlock(&g_audio_mu);
            return -1;
        }
        g_out_ch = channels;
        g_out_rate = rate_hz;
    }

    if (!g_out_running) {
        aaudio_result_t rc = AAudioStream_requestStart(g_out_stream);
        if (rc == AAUDIO_OK) {
            g_out_running = 1;
            LOGI("AAudio output started (%u ch @ %u Hz)", channels, rate_hz);
        } else {
            LOGW("AAudio output requestStart failed: %d", rc);
        }
    }
    int ok = g_out_running ? 0 : -1;
    pthread_mutex_unlock(&g_audio_mu);
    return ok;
}

static void hda_stop_out(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&g_audio_mu);
    if (g_out_stream && g_out_running) {
        AAudioStream_requestStop(g_out_stream);
        g_out_running = 0;
    }
    pthread_mutex_unlock(&g_audio_mu);
}

static int hda_start_in(void *opaque, unsigned channels, unsigned rate_hz) {
    HvHda *hda = (HvHda *)opaque;
    if (channels < 1 || channels > 2) channels = 2;
    if (rate_hz < 8000 || rate_hz > 192000) rate_hz = 48000;

    pthread_mutex_lock(&g_audio_mu);
    if (g_in_stream && (channels != g_in_ch || rate_hz != g_in_rate)) {
        AAudioStream_requestStop(g_in_stream);
        AAudioStream_close(g_in_stream);
        g_in_stream = NULL;
        g_in_running = 0;
    }

    if (!g_in_stream) {
        AAudioStreamBuilder *b = NULL;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) {
            pthread_mutex_unlock(&g_audio_mu);
            return -1;
        }
        AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b, (int32_t)channels);
        AAudioStreamBuilder_setSampleRate(b, (int32_t)rate_hz);
        AAudioStreamBuilder_setDataCallback(b, in_data_cb, hda);

        aaudio_result_t rc = AAudioStreamBuilder_openStream(b, &g_in_stream);
        AAudioStreamBuilder_delete(b);
        if (rc != AAUDIO_OK || !g_in_stream) {
            LOGW("AAudio input openStream failed: %d (RECORD_AUDIO permission pending?)", rc);
            g_in_stream = NULL;
            pthread_mutex_unlock(&g_audio_mu);
            return -1;
        }
        g_in_ch = channels;
        g_in_rate = rate_hz;
    }

    if (!g_in_running) {
        aaudio_result_t rc = AAudioStream_requestStart(g_in_stream);
        if (rc == AAUDIO_OK) {
            g_in_running = 1;
            LOGI("AAudio input started (%u ch @ %u Hz)", channels, rate_hz);
        } else {
            LOGW("AAudio input requestStart failed: %d", rc);
        }
    }
    int ok = g_in_running ? 0 : -1;
    pthread_mutex_unlock(&g_audio_mu);
    return ok;
}

static void hda_stop_in(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&g_audio_mu);
    if (g_in_stream && g_in_running) {
        AAudioStream_requestStop(g_in_stream);
        g_in_running = 0;
    }
    pthread_mutex_unlock(&g_audio_mu);
}

void hv_audio_attach_hda(HvHda *hda) {
    if (!hda) return;
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
    if (g_out_stream) {
        if (g_out_running) AAudioStream_requestStop(g_out_stream);
        AAudioStream_close(g_out_stream);
        g_out_stream = NULL;
        g_out_running = 0;
    }
    if (g_in_stream) {
        if (g_in_running) AAudioStream_requestStop(g_in_stream);
        AAudioStream_close(g_in_stream);
        g_in_stream = NULL;
        g_in_running = 0;
    }
    pthread_mutex_unlock(&g_audio_mu);
}

void hv_audio_attach(HvVirtioSnd *d) {
    (void)d;
}

#endif /* __ANDROID__ */
