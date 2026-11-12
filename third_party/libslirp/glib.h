/* Minimal self-contained GLib 2.0 compatibility shim for libslirp 4.8.0 */
#ifndef WINPIXEL_GLIB_SHIM_H
#define WINPIXEL_GLIB_SHIM_H

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#define G_OS_UNIX 1
#endif

#define G_LITTLE_ENDIAN 1234
#define G_BIG_ENDIAN    4321
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define G_BYTE_ORDER G_BIG_ENDIAN
#define GUINT16_TO_BE(x)   ((uint16_t)(x))
#define GUINT16_FROM_BE(x) ((uint16_t)(x))
#define GUINT32_TO_BE(x)   ((uint32_t)(x))
#define GUINT32_FROM_BE(x) ((uint32_t)(x))
#define GUINT64_TO_BE(x)   ((uint64_t)(x))
#define GUINT64_FROM_BE(x) ((uint64_t)(x))
#else
#define G_BYTE_ORDER G_LITTLE_ENDIAN
#define GUINT16_TO_BE(x)   __builtin_bswap16((uint16_t)(x))
#define GUINT16_FROM_BE(x) __builtin_bswap16((uint16_t)(x))
#define GUINT32_TO_BE(x)   __builtin_bswap32((uint32_t)(x))
#define GUINT32_FROM_BE(x) __builtin_bswap32((uint32_t)(x))
#define GUINT64_TO_BE(x)   __builtin_bswap64((uint64_t)(x))
#define GUINT64_FROM_BE(x) __builtin_bswap64((uint64_t)(x))
#endif
#define GINT16_TO_BE(x)    ((int16_t)GUINT16_TO_BE(x))
#define GINT16_FROM_BE(x)  ((int16_t)GUINT16_FROM_BE(x))
#define GINT32_TO_BE(x)    ((int32_t)GUINT32_TO_BE(x))
#define GINT32_FROM_BE(x)  ((int32_t)GUINT32_FROM_BE(x))
#define GINT64_TO_BE(x)    ((int64_t)GUINT64_TO_BE(x))
#define GINT64_FROM_BE(x)  ((int64_t)GUINT64_FROM_BE(x))

typedef int            gboolean;
typedef char           gchar;
typedef int            gint;
typedef unsigned int   guint;
typedef int16_t        gint16;
typedef uint16_t       guint16;
typedef int32_t        gint32;
typedef uint32_t       guint32;
typedef int64_t        gint64;
typedef uint64_t       guint64;
typedef size_t         gsize;
typedef ssize_t        gssize;
typedef void *         gpointer;
typedef const void *   gconstpointer;
typedef char **        GStrv;

#ifndef typeof
#define typeof __typeof__
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define G_N_ELEMENTS(arr) (sizeof(arr) / sizeof((arr)[0]))
#define G_SIZEOF_MEMBER(type, member) sizeof(((type *)0)->member)
#define G_STATIC_ASSERT(expr) _Static_assert(expr, #expr)
#define G_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define G_LIKELY(expr)   __builtin_expect(!!(expr), 1)
#define G_GNUC_PRINTF(fmt_idx, arg_idx) __attribute__((__format__(__printf__, fmt_idx, arg_idx)))

/* Memory allocation */
static inline void *g_malloc(size_t n) {
    if (n == 0) return NULL;
    void *p = malloc(n);
    if (!p) abort();
    return p;
}
static inline void *g_malloc0(size_t n) {
    if (n == 0) return NULL;
    void *p = calloc(1, n);
    if (!p) abort();
    return p;
}
static inline void *g_realloc(void *p, size_t n) {
    if (n == 0) { free(p); return NULL; }
    void *r = realloc(p, n);
    if (!r) abort();
    return r;
}
static inline void g_free(void *p) { free(p); }

#define g_new(type, count)  ((type *)g_malloc(sizeof(type) * (size_t)(count)))
#define g_new0(type, count) ((type *)g_malloc0(sizeof(type) * (size_t)(count)))

/* String helpers */
static inline char *g_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}
static inline size_t g_strlcpy(char *dst, const char *src, size_t siz) {
    size_t srclen = strlen(src);
    if (siz > 0) {
        size_t n = srclen >= siz ? siz - 1 : srclen;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return srclen;
}
static inline gboolean g_str_has_prefix(const char *str, const char *prefix) {
    if (!str || !prefix) return FALSE;
    return strncmp(str, prefix, strlen(prefix)) == 0;
}
static inline char *g_strstr_len(const char *haystack, gssize haystack_len, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    size_t hlen = haystack_len < 0 ? strlen(haystack) : (size_t)haystack_len;
    if (hlen < nlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(haystack + i, needle, nlen) == 0)
            return (char *)(haystack + i);
    }
    return NULL;
}
static inline int g_ascii_strcasecmp(const char *s1, const char *s2) {
    return strcasecmp(s1, s2);
}

#define g_snprintf  snprintf
#define g_vsnprintf vsnprintf
#define g_getenv    getenv
#define g_strerror  strerror

/* GString */
typedef struct {
    char *str;
    gsize len;
    gsize allocated_len;
} GString;

static inline GString *g_string_new(const char *init) {
    GString *s = g_new0(GString, 1);
    size_t ilen = init ? strlen(init) : 0;
    s->allocated_len = ilen + 64;
    s->str = (char *)g_malloc(s->allocated_len);
    if (init) memcpy(s->str, init, ilen);
    s->str[ilen] = '\0';
    s->len = ilen;
    return s;
}
static inline void g_string_append_printf(GString *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static inline void g_string_append_printf(GString *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed > 0) {
        if (s->len + (size_t)needed + 1 > s->allocated_len) {
            s->allocated_len = (s->len + (size_t)needed + 64) * 2;
            s->str = (char *)g_realloc(s->str, s->allocated_len);
        }
        vsnprintf(s->str + s->len, s->allocated_len - s->len, fmt, ap2);
        s->len += (size_t)needed;
    }
    va_end(ap2);
}
static inline char *g_string_free(GString *s, gboolean free_segment) {
    if (!s) return NULL;
    char *ret = s->str;
    if (free_segment) {
        g_free(ret);
        ret = NULL;
    }
    g_free(s);
    return ret;
}

/* GRand */
typedef struct { uint64_t state; } GRand;
static inline GRand *g_rand_new(void) {
    GRand *r = g_new0(GRand, 1);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    r->state = ((uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32) ^ (uintptr_t)r) | 1ULL;
    return r;
}
static inline void g_rand_free(GRand *r) { g_free(r); }
static inline gint32 g_rand_int_range(GRand *r, gint32 begin, gint32 end) {
    if (end <= begin) return begin;
    uint64_t x = r->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->state = x;
    uint32_t span = (uint32_t)(end - begin);
    return begin + (gint32)((uint32_t)(x >> 16) % span);
}

/* Logging, Assertions & Debug */
typedef struct {
    const char *key;
    guint value;
} GDebugKey;

static inline guint g_parse_debug_string(const char *string, const GDebugKey *keys, guint nkeys) {
    if (!string || !keys) return 0;
    guint result = 0;
    for (guint i = 0; i < nkeys; i++) {
        if (strstr(string, keys[i].key) || !strcasecmp(string, "all"))
            result |= keys[i].value;
    }
    return result;
}

#define g_assert(expr) assert(expr)
#define g_assert_not_reached() do { abort(); } while (0)
#define g_return_if_fail(expr) do { if (!(expr)) return; } while (0)
#define g_return_val_if_fail(expr, val) do { if (!(expr)) return (val); } while (0)
#define g_warn_if_fail(expr) do { if (!(expr)) fprintf(stderr, "slirp warn: %s failed at %s:%d\n", #expr, __FILE__, __LINE__); } while (0)
#define g_warn_if_reached() do { fprintf(stderr, "slirp warn: unreachable reached at %s:%d\n", __FILE__, __LINE__); } while (0)
#define g_warning(...) do { fprintf(stderr, "slirp warning: " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define g_critical(...) do { fprintf(stderr, "slirp critical: " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define g_error(...) do { fprintf(stderr, "slirp fatal: " __VA_ARGS__); fprintf(stderr, "\n"); abort(); } while (0)
#define g_debug(...) do { } while (0)

/* GError & Process spawn stubs (only referenced by unused slirp_add_exec) */
typedef struct {
    int domain;
    int code;
    char *message;
} GError;

typedef int GSpawnFlags;
typedef int GPid;
typedef void (*GSpawnChildSetupFunc)(gpointer user_data);
#define G_SPAWN_SEARCH_PATH 4
#define GLIB_CHECK_VERSION(a, b, c) 1

static inline void g_error_free(GError *err) {
    if (err) { g_free(err->message); g_free(err); }
}
static inline void g_strfreev(char **str_array) {
    if (!str_array) return;
    for (size_t i = 0; str_array[i]; i++) g_free(str_array[i]);
    g_free(str_array);
}
static inline guint g_strv_length(char **str_array) {
    guint n = 0;
    if (str_array) while (str_array[n]) n++;
    return n;
}
static inline gboolean g_shell_parse_argv(const char *command_line, gint *argcp, char ***argvp, GError **error) {
    (void)command_line; (void)argcp; (void)argvp; (void)error;
    return FALSE;
}
static inline gboolean g_spawn_async_with_fds(const char *working_directory, char **argv, char **envp,
                                              GSpawnFlags flags, void (*child_setup)(void *), void *user_data,
                                              int *child_pid, gint stdin_fd, gint stdout_fd, gint stderr_fd,
                                              GError **error) {
    (void)working_directory; (void)argv; (void)envp; (void)flags; (void)child_setup; (void)user_data;
    (void)child_pid; (void)stdin_fd; (void)stdout_fd; (void)stderr_fd; (void)error;
    return FALSE;
}

#endif /* WINPIXEL_GLIB_SHIM_H */
