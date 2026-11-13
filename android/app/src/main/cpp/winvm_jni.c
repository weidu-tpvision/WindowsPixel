/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * winvm_jni.c — JNI bridge between the Android Kotlin/Compose UI and libwinvm.
 */
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "fb_display.h"
#include "share.h"
#include "winvm.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "winvm-jni", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "winvm-jni", __VA_ARGS__)

/* g_op_mu serializes whole start/stop operations (held for seconds: never taken on the UI thread).
   g_life_mu is short: it guards g_surface_win and the moment the VM's display may be handed to the
   renderer, so a surface callback can't pass a board->fb that a concurrent stop is about to free. */
static pthread_mutex_t g_op_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_life_mu = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_surface_win = NULL;
static atomic_int g_vm_running = 0;
static int g_display_live = 0; /* under g_life_mu: board.fb is valid to show */

static const char *get_utf(JNIEnv *env, jstring s) {
    return s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL; /* NULL on OOM too (exception pending) */
}

static void on_vm_exit(int reason) {
    LOGI("VM exited with reason=%d (%s)", reason, hv_winvm_reason_name(reason));
    atomic_store(&g_vm_running, 0);
}

JNIEXPORT jstring JNICALL
Java_com_winpixel_app_WinVmBridge_nativeStartVm(
    JNIEnv *env, jclass clazz,
    jstring j_code_fd, jstring j_vars_fd,
    jstring j_disk0, jstring j_disk1,
    jstring j_share_dir, jstring j_share_sock,
    jint cpus, jint ram_mb, jboolean sound, jboolean no_net) {
    (void)clazz;
    pthread_mutex_lock(&g_op_mu);
    if (atomic_load(&g_vm_running)) {
        pthread_mutex_unlock(&g_op_mu);
        return (*env)->NewStringUTF(env, "VM is already running.");
    }
    /* Clean up any previous VM instance that exited on its own before recreating. */
    pthread_mutex_lock(&g_life_mu);
    g_display_live = 0;
    pthread_mutex_unlock(&g_life_mu);
    hv_winvm_destroy();

    const char *code_fd = get_utf(env, j_code_fd);
    const char *vars_fd = get_utf(env, j_vars_fd);
    const char *disk0 = get_utf(env, j_disk0);
    const char *disk1 = get_utf(env, j_disk1);
    const char *share_dir = get_utf(env, j_share_dir);
    const char *share_sock = get_utf(env, j_share_sock);

    HvWinVmConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.code_fd = code_fd;
    cfg.vars_fd = vars_fd;
    cfg.disk0 = disk0;
    cfg.disk1 = (disk1 && disk1[0]) ? disk1 : NULL;
    cfg.share_dir = (share_dir && share_dir[0]) ? share_dir : NULL;
    cfg.share_sock = (share_sock && share_sock[0]) ? share_sock : NULL;
    cfg.ncpus = cpus > 0 ? (int)cpus : 4;
    cfg.ram_mb = ram_mb > 0 ? (uint64_t)ram_mb : 4096;
    cfg.jit_bytes = 256u << 20;
    cfg.no_net = no_net ? 1 : 0;
    cfg.enable_sound = sound ? 1 : 0;

    char err[512] = {0};
    int rc = hv_winvm_create(&cfg, err, sizeof err);

    if (code_fd) (*env)->ReleaseStringUTFChars(env, j_code_fd, code_fd);
    if (vars_fd) (*env)->ReleaseStringUTFChars(env, j_vars_fd, vars_fd);
    if (disk0) (*env)->ReleaseStringUTFChars(env, j_disk0, disk0);
    if (disk1) (*env)->ReleaseStringUTFChars(env, j_disk1, disk1);
    if (share_dir) (*env)->ReleaseStringUTFChars(env, j_share_dir, share_dir);
    if (share_sock) (*env)->ReleaseStringUTFChars(env, j_share_sock, share_sock);

    if (rc != 0) {
        pthread_mutex_unlock(&g_op_mu);
        LOGE("hv_winvm_create failed: %s", err);
        return (*env)->NewStringUTF(env, err[0] ? err : "hv_winvm_create failed");
    }

    pthread_mutex_lock(&g_life_mu);
    HvWinVirt *b = hv_winvm_board();
    g_display_live = 1;
    if (b && b->fb && g_surface_win) hv_fb_android_set_window(b->fb, g_surface_win);
    atomic_store(&g_vm_running, 1);
    pthread_mutex_unlock(&g_life_mu);
    hv_winvm_start(1, on_vm_exit);
    pthread_mutex_unlock(&g_op_mu);
    LOGI("VM started (%d vCPUs, %llu MB RAM)", cfg.ncpus, (unsigned long long)cfg.ram_mb);
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeStopVm(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_op_mu); /* a second Stop waits, then finds nothing to destroy */
    pthread_mutex_lock(&g_life_mu);
    g_display_live = 0;
    atomic_store(&g_vm_running, 0);
    pthread_mutex_unlock(&g_life_mu);
    hv_winvm_destroy();
    pthread_mutex_unlock(&g_op_mu);
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativePowerButton(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    hv_winvm_power_button();
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeSetSurface(JNIEnv *env, jclass clazz, jobject surface) {
    (void)clazz;
    pthread_mutex_lock(&g_life_mu);
    if (g_surface_win) {
        ANativeWindow_release(g_surface_win);
        g_surface_win = NULL;
    }
    if (surface) {
        g_surface_win = ANativeWindow_fromSurface(env, surface);
    }
    HvWinVirt *b = g_display_live ? hv_winvm_board() : NULL;
    hv_fb_android_set_window(b ? b->fb : NULL, g_surface_win);
    pthread_mutex_unlock(&g_life_mu);
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeTabletEvent(
    JNIEnv *env, jclass clazz, jdouble x_norm, jdouble y_norm, jint buttons, jint wheel) {
    (void)env;
    (void)clazz;
    hv_winvm_tablet(x_norm, y_norm, (uint8_t)(buttons & 0xff), (int8_t)wheel);
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeKeyHid(JNIEnv *env, jclass clazz, jint usage, jboolean down) {
    (void)env;
    (void)clazz;
    hv_winvm_key_hid((uint8_t)(usage & 0xff), down ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeKeyLinux(JNIEnv *env, jclass clazz, jint code, jboolean down) {
    (void)env;
    (void)clazz;
    hv_winvm_key_linux((uint16_t)(code & 0xffff), down ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeSendCtrlAltDel(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    hv_winvm_send_ctrl_alt_del();
}

JNIEXPORT jboolean JNICALL
Java_com_winpixel_app_WinVmBridge_nativeIsRunning(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return atomic_load(&g_vm_running) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_com_winpixel_app_WinVmBridge_nativeDisplaySize(JNIEnv *env, jclass clazz) {
    (void)clazz;
    uint32_t w = 800, h = 600;
    hv_winvm_display_size(&w, &h);
    jintArray arr = (*env)->NewIntArray(env, 2);
    if (arr) {
        jint vals[2] = {(jint)w, (jint)h};
        (*env)->SetIntArrayRegion(env, arr, 0, 2, vals);
    }
    return arr;
}

JNIEXPORT jstring JNICALL
Java_com_winpixel_app_WinVmBridge_nativePollOpenUrl(JNIEnv *env, jclass clazz) {
    (void)clazz;
    char *url = hv_share_take_open_url();
    if (!url) return NULL;
    jstring res = (*env)->NewStringUTF(env, url);
    free(url);
    return res;
}
