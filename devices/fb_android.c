/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fb_android.c — Android ANativeWindow + EGL + OpenGL ES 3.0 120 Hz blitter for ramfb.
 *
 * ramfb exposes little-endian BGRX8888 (DRM XR24: byte0=B, byte1=G, byte2=R, byte3=X).
 * We upload directly to a GL_RGBA8 texture and use hardware GL_TEXTURE_SWIZZLE_R/B/A
 * so the GPU converts BGRX -> RGBA at full refresh rate with zero CPU pixel swizzling.
 */
#include "fb_display.h"

#ifdef __ANDROID__

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "winvm-fb", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "winvm-fb", __VA_ARGS__)

static pthread_mutex_t g_fb_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fb_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_fb_ack_cv = PTHREAD_COND_INITIALIZER;
static HvFbDisplay *g_fb_disp = NULL;
static ANativeWindow *g_win = NULL;
static int g_win_changed = 0;
static int g_surface_bound = 0;
static int g_thread_started = 0;
static int g_stop_thread = 0;
static pthread_t g_render_thread;
static atomic_uint_fast64_t g_frames = 0;

static const char *VERT_SRC =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUv;\n"
    "out vec2 vUv;\n"
    "void main() {\n"
    "    vUv = aUv;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vUv;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(uTex, vUv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOGE("shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint build_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

typedef struct EglState {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig cfg;
    EGLSurface surf;
    GLuint prog;
    GLuint tex;
    GLuint vao;
    GLuint vbo;
    uint32_t tex_w;
    uint32_t tex_h;
    uint8_t *scratch;
    size_t scratch_cap;
} EglState;

static void egl_destroy_surface(EglState *e) {
    if (e->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (e->surf != EGL_NO_SURFACE) {
            eglDestroySurface(e->dpy, e->surf);
            e->surf = EGL_NO_SURFACE;
        }
    }
}

static void egl_destroy_all(EglState *e) {
    if (e->dpy != EGL_NO_DISPLAY) {
        if (e->surf != EGL_NO_SURFACE && e->ctx != EGL_NO_CONTEXT) {
            eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx);
        }
        if (e->tex) glDeleteTextures(1, &e->tex);
        if (e->vbo) glDeleteBuffers(1, &e->vbo);
        if (e->vao) glDeleteVertexArrays(1, &e->vao);
        if (e->prog) glDeleteProgram(e->prog);
        eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (e->surf != EGL_NO_SURFACE) eglDestroySurface(e->dpy, e->surf);
        if (e->ctx != EGL_NO_CONTEXT) eglDestroyContext(e->dpy, e->ctx);
        eglTerminate(e->dpy);
    }
    free(e->scratch);
    memset(e, 0, sizeof *e);
    e->dpy = EGL_NO_DISPLAY;
    e->ctx = EGL_NO_CONTEXT;
    e->surf = EGL_NO_SURFACE;
}

static int egl_init_context(EglState *e) {
    e->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (e->dpy == EGL_NO_DISPLAY || !eglInitialize(e->dpy, NULL, NULL)) {
        LOGE("eglInitialize failed");
        return -1;
    }
    static const EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(e->dpy, cfg_attrs, &e->cfg, 1, &ncfg) || ncfg < 1) {
        LOGE("eglChooseConfig failed");
        return -1;
    }
    static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    e->ctx = eglCreateContext(e->dpy, e->cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (e->ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return -1;
    }
    return 0;
}

static int egl_bind_window(EglState *e, ANativeWindow *win) {
    egl_destroy_surface(e);
    if (!win) return -1;
    e->surf = eglCreateWindowSurface(e->dpy, e->cfg, win, NULL);
    if (e->surf == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return -1;
    }
    if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
        LOGE("eglMakeCurrent failed");
        egl_destroy_surface(e);
        return -1;
    }
    eglSwapInterval(e->dpy, 1);

    if (!e->prog) {
        e->prog = build_program();
        if (!e->prog) return -1;
        glGenTextures(1, &e->tex);
        glBindTexture(GL_TEXTURE_2D, e->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* Zero-copy BGRX8888 -> RGBA8 hardware channel swizzle */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);

        static const float quad[] = {
            /* pos(x,y), uv(u,v) */
            -1.0f, -1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 1.0f,
            -1.0f,  1.0f, 0.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 0.0f,
        };
        glGenVertexArrays(1, &e->vao);
        glGenBuffers(1, &e->vbo);
        glBindVertexArray(e->vao);
        glBindBuffer(GL_ARRAY_BUFFER, e->vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
        e->tex_w = 0;
        e->tex_h = 0;
    }
    return 0;
}

static void *fb_render_thread(void *arg) {
    (void)arg;
    EglState e;
    memset(&e, 0, sizeof e);
    e.dpy = EGL_NO_DISPLAY;
    e.ctx = EGL_NO_CONTEXT;
    e.surf = EGL_NO_SURFACE;

    if (egl_init_context(&e) != 0) {
        egl_destroy_all(&e);
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&g_fb_mu);
        while (!g_stop_thread && !g_win) {
            if (g_surface_bound) {
                pthread_mutex_unlock(&g_fb_mu);
                egl_destroy_surface(&e);
                pthread_mutex_lock(&g_fb_mu);
                g_surface_bound = 0;
                pthread_cond_broadcast(&g_fb_ack_cv);
            } else {
                pthread_cond_broadcast(&g_fb_ack_cv);
                pthread_cond_wait(&g_fb_cv, &g_fb_mu);
            }
        }
        if (g_stop_thread) {
            g_surface_bound = 0;
            pthread_cond_broadcast(&g_fb_ack_cv);
            pthread_mutex_unlock(&g_fb_mu);
            break;
        }
        HvFbDisplay *fb = g_fb_disp;
        ANativeWindow *win = g_win;
        int changed = g_win_changed;
        g_win_changed = 0;
        if (win) ANativeWindow_acquire(win);
        pthread_mutex_unlock(&g_fb_mu);

        if (changed || e.surf == EGL_NO_SURFACE) {
            if (egl_bind_window(&e, win) != 0) {
                if (win) ANativeWindow_release(win);
                usleep(16000);
                continue;
            }
            pthread_mutex_lock(&g_fb_mu);
            g_surface_bound = 1;
            pthread_mutex_unlock(&g_fb_mu);
        }

        EGLint surf_w = 0, surf_h = 0;
        eglQuerySurface(e.dpy, e.surf, EGL_WIDTH, &surf_w);
        eglQuerySurface(e.dpy, e.surf, EGL_HEIGHT, &surf_h);
        if (win) ANativeWindow_release(win);

        const uint8_t *pixels = NULL;
        uint32_t fw = 0, fh = 0, stride = 0;
        if (fb && hv_fb_display_get_frame(fb, &pixels, &fw, &fh, &stride) == 0 && pixels && fw > 0 && fh > 0) {
            size_t need = (size_t)fw * fh * 4u;
            if (stride != fw * 4u) {
                if (need > e.scratch_cap) {
                    uint8_t *nb = realloc(e.scratch, need);
                    if (nb) { e.scratch = nb; e.scratch_cap = need; }
                }
                if (e.scratch_cap >= need) {
                    for (uint32_t y = 0; y < fh; y++)
                        memcpy(e.scratch + (size_t)y * fw * 4u, pixels + (size_t)y * stride, (size_t)fw * 4u);
                    pixels = e.scratch;
                }
            }

            glBindTexture(GL_TEXTURE_2D, e.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (fw != e.tex_w || fh != e.tex_h) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)fw, (GLsizei)fh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                e.tex_w = fw;
                e.tex_h = fh;
                LOGI("guest ramfb texture resized to %ux%u (surface %dx%d)", fw, fh, surf_w, surf_h);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)fw, (GLsizei)fh,
                                GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            }

            glViewport(0, 0, surf_w, surf_h);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (surf_w > 0 && surf_h > 0) {
                float guest_aspect = (float)fw / (float)fh;
                float view_aspect = (float)surf_w / (float)surf_h;
                int vx = 0, vy = 0, vw = surf_w, vh = surf_h;
                if (view_aspect > guest_aspect) {
                    vw = (int)((float)surf_h * guest_aspect);
                    vx = (surf_w - vw) / 2;
                } else {
                    vh = (int)((float)surf_w / guest_aspect);
                    vy = (surf_h - vh) / 2;
                }
                glViewport(vx, vy, vw, vh);
            }

            glUseProgram(e.prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, e.tex);
            glBindVertexArray(e.vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            eglSwapBuffers(e.dpy, e.surf);
            atomic_fetch_add(&g_frames, 1);
        } else {
            glViewport(0, 0, surf_w, surf_h);
            glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(e.dpy, e.surf);
            usleep(16000);
        }
    }

    egl_destroy_all(&e);
    return NULL;
}

void hv_fb_android_set_window(HvFbDisplay *fb, struct ANativeWindow *win) {
    pthread_mutex_lock(&g_fb_mu);
    if (fb) g_fb_disp = fb;
    if (g_win) ANativeWindow_release(g_win);
    g_win = win;
    if (g_win) ANativeWindow_acquire(g_win);
    g_win_changed = 1;
    if (!g_thread_started && g_win) {
        g_stop_thread = 0;
        if (pthread_create(&g_render_thread, NULL, fb_render_thread, NULL) == 0)
            g_thread_started = 1;
    }
    pthread_cond_broadcast(&g_fb_cv);
    /* SurfaceHolder.surfaceDestroyed requires rendering to stop before returning. */
    if (!win && g_thread_started) {
        while (g_surface_bound && !g_stop_thread) {
            pthread_cond_wait(&g_fb_ack_cv, &g_fb_mu);
        }
    }
    pthread_mutex_unlock(&g_fb_mu);
}

uint64_t hv_fb_android_frames(void) {
    return (uint64_t)atomic_load(&g_frames);
}

void hv_fb_android_shutdown(void) {
    pthread_mutex_lock(&g_fb_mu);
    int started = g_thread_started;
    g_stop_thread = 1;
    if (g_win) {
        ANativeWindow_release(g_win);
        g_win = NULL;
    }
    pthread_cond_broadcast(&g_fb_cv);
    pthread_mutex_unlock(&g_fb_mu);
    if (started) {
        pthread_join(g_render_thread, NULL);
        pthread_mutex_lock(&g_fb_mu);
        g_thread_started = 0;
        g_stop_thread = 0;
        pthread_mutex_unlock(&g_fb_mu);
    }
}

#endif /* __ANDROID__ */
