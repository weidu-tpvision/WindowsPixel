/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * winvm_jni.c — JNI bridge between the Android Kotlin/Compose UI and libwinvm.
 */
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "fb_display.h"
#include "share.h"
#include "winvm.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "winvm-jni", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "winvm-jni", __VA_ARGS__)

static ANativeWindow *g_surface_win = NULL;
static atomic_int g_vm_running = 0;

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
    if (atomic_load(&g_vm_running)) {
        return (*env)->NewStringUTF(env, "VM is already running.");
    }

    const char *code_fd = j_code_fd ? (*env)->GetStringUTFChars(env, j_code_fd, NULL) : NULL;
    const char *vars_fd = j_vars_fd ? (*env)->GetStringUTFChars(env, j_vars_fd, NULL) : NULL;
    const char *disk0 = j_disk0 ? (*env)->GetStringUTFChars(env, j_disk0, NULL) : NULL;
    const char *disk1 = j_disk1 ? (*env)->GetStringUTFChars(env, j_disk1, NULL) : NULL;
    const char *share_dir = j_share_dir ? (*env)->GetStringUTFChars(env, j_share_dir, NULL) : NULL;
    const char *share_sock = j_share_sock ? (*env)->GetStringUTFChars(env, j_share_sock, NULL) : NULL;

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
        LOGE("hv_winvm_create failed: %s", err);
        return (*env)->NewStringUTF(env, err[0] ? err : "hv_winvm_create failed");
    }

    HvWinVirt *b = hv_winvm_board();
    if (b && b->fb && g_surface_win) {
        hv_fb_android_set_window(b->fb, g_surface_win);
    }

    atomic_store(&g_vm_running, 1);
    hv_winvm_start(1, on_vm_exit);
    LOGI("VM started (%d vCPUs, %llu MB RAM)", cfg.ncpus, (unsigned long long)cfg.ram_mb);
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_winpixel_app_WinVmBridge_nativeStopVm(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    if (atomic_load(&g_vm_running)) {
        hv_winvm_stop();
        atomic_store(&g_vm_running, 0);
    }
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
    if (g_surface_win) {
        ANativeWindow_release(g_surface_win);
        g_surface_win = NULL;
    }
    if (surface) {
        g_surface_win = ANativeWindow_fromSurface(env, surface);
    }
    HvWinVirt *b = hv_winvm_board();
    hv_fb_android_set_window(b ? b->fb : NULL, g_surface_win);
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
