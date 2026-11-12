/*
 * LD_PRELOAD shim that logs every heap call as a raw text trace to
 * $ALLOCTRACE_OUT.<pid>. tools/convert/trace_convert.cpp cleans the result.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *(*real_malloc)(size_t);
static void (*real_free)(void *);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);
static void *(*real_memalign)(size_t, size_t);
static void *(*real_valloc)(size_t);
static void *(*real_pvalloc)(size_t);

/* dlsym() may allocate before the real functions are resolved; those requests
 * are served from here and never logged. */
static char boot_buf[64 * 1024] __attribute__((aligned(16)));
static size_t boot_used;
static int initialising;

static void *boot_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (boot_used + n > sizeof boot_buf) return NULL;
    void *p = boot_buf + boot_used;
    boot_used += n;
    return p; /* static storage is already zeroed (calloc-safe) */
}
static int is_boot(void *p) {
    return (char *)p >= boot_buf && (char *)p < boot_buf + sizeof boot_buf;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int in_hook;
static int out_fd = -1;
static int out_failed;
static int finalised;
/* stdio allocates, so hooks format records here and flush with write(2). */
static char obuf[1 << 20];
static size_t olen;

static void flush_buf(void) {
    size_t off = 0;
    while (off < olen && out_fd >= 0) {
        ssize_t w = write(out_fd, obuf + off, olen - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    olen = 0;
}

static char *put_dec(char *p, uint64_t v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_hex(char *p, uint64_t v) {
    static const char hx[] = "0123456789abcdef";
    char t[20]; int n = 0;
    do { t[n++] = hx[v & 15]; v >>= 4; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void open_out(void) {
    const char *base = getenv("ALLOCTRACE_OUT");
    if (!base || !*base) { out_failed = 1; return; }
    char path[4096];
    size_t bl = strlen(base);
    if (bl > sizeof path - 32) { out_failed = 1; return; }
    memcpy(path, base, bl);
    char *e = path + bl;
    *e++ = '.';
    e = put_dec(e, (uint64_t)getpid());
    *e = 0;
    out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out_fd < 0) out_failed = 1;
}

/* caller holds `lock` */
static void emit(char op, uint64_t a, uint64_t sz, uint64_t b, int nargs) {
    if (out_failed) return;
    if (out_fd < 0) { open_out(); if (out_failed) return; }
    if (olen + 64 > sizeof obuf) flush_buf();
    char *p = obuf + olen;
    *p++ = op; *p++ = ' ';
    p = put_hex(p, a);
    if (nargs >= 2) { *p++ = ' '; p = put_dec(p, sz); }
    if (nargs >= 3) { *p++ = ' '; p = put_hex(p, b); }
    *p++ = '\n';
    olen = (size_t)(p - obuf);
    if (finalised) flush_buf();
}

static void atfork_prepare(void) { pthread_mutex_lock(&lock); }
static void atfork_parent(void) { pthread_mutex_unlock(&lock); }
static void atfork_child(void) {
    /* the parent will write these records; the child reopens its own file */
    olen = 0;
    if (out_fd >= 0) close(out_fd);
    out_fd = -1;
    out_failed = 0;
    pthread_mutex_unlock(&lock);
}

static void init(void) {
    if (real_malloc || initialising) return;
    initialising = 1;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");
    pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
    initialising = 0;
}

__attribute__((constructor)) static void ctor(void) { init(); }

__attribute__((destructor)) static void dtor(void) {
    pthread_mutex_lock(&lock);
    flush_buf();
    finalised = 1; /* frees from later destructors are written immediately */
    pthread_mutex_unlock(&lock);
}

/* Only the outermost call logs. The lock spans the real call and the log
 * write, so trace order matches real heap order across threads. */
#define ENTER()                                                   \
    int traced_ = !in_hook;                                       \
    if (traced_) { in_hook = 1; pthread_mutex_lock(&lock); }
#define LEAVE()                                                   \
    if (traced_) { pthread_mutex_unlock(&lock); in_hook = 0; }

void *malloc(size_t n) {
    if (!real_malloc) { init(); if (!real_malloc) return boot_alloc(n); }
    ENTER();
    void *p = real_malloc(n);
    if (traced_ && p) emit('a', (uintptr_t)p, n, 0, 2);
    LEAVE();
    return p;
}

void *calloc(size_t n, size_t e) {
    if (!real_calloc) { init(); if (!real_calloc) return boot_alloc(n * e); }
    ENTER();
    void *p = real_calloc(n, e);
    if (traced_ && p) emit('a', (uintptr_t)p, (uint64_t)n * e, 0, 2);
    LEAVE();
    return p;
}

void free(void *p) {
    if (!p || is_boot(p)) return;
    if (!real_free) init();
    ENTER();
    if (traced_) emit('f', (uintptr_t)p, 0, 0, 1);
    real_free(p);
    LEAVE();
}

void *realloc(void *p, size_t n) {
    if (p && is_boot(p)) {           /* move a bootstrap block to the real heap */
        void *q = malloc(n);
        size_t avail = (size_t)(boot_buf + sizeof boot_buf - (char *)p);
        if (q) memcpy(q, p, n < avail ? n : avail);
        return q;
    }
    if (!real_realloc) { init(); if (!real_realloc) return boot_alloc(n); }
    ENTER();
    void *q = real_realloc(p, n);
    if (traced_) {
        if (!p) { if (q) emit('a', (uintptr_t)q, n, 0, 2); }
        else if (n == 0) emit('f', (uintptr_t)p, 0, 0, 1);
        else if (q) emit('r', (uintptr_t)p, n, (uintptr_t)q, 3);
        /* q == NULL with n > 0: realloc failed and the old block is unchanged */
    }
    LEAVE();
    return q;
}

int posix_memalign(void **out, size_t al, size_t n) {
    if (!real_posix_memalign) init();
    ENTER();
    int r = real_posix_memalign(out, al, n);
    if (traced_ && r == 0 && *out) emit('a', (uintptr_t)*out, n, 0, 2);
    LEAVE();
    return r;
}

void *aligned_alloc(size_t al, size_t n) {
    if (!real_aligned_alloc) init();
    ENTER();
    void *p = real_aligned_alloc(al, n);
    if (traced_ && p) emit('a', (uintptr_t)p, n, 0, 2);
    LEAVE();
    return p;
}

void *memalign(size_t al, size_t n) {
    if (!real_memalign) init();
    ENTER();
    void *p = real_memalign(al, n);
    if (traced_ && p) emit('a', (uintptr_t)p, n, 0, 2);
    LEAVE();
    return p;
}

void *valloc(size_t n) {
    if (!real_valloc) init();
    ENTER();
    void *p = real_valloc(n);
    if (traced_ && p) emit('a', (uintptr_t)p, n, 0, 2);
    LEAVE();
    return p;
}

void *pvalloc(size_t n) {
    if (!real_pvalloc) init();
    ENTER();
    void *p = real_pvalloc(n);
    if (traced_ && p) emit('a', (uintptr_t)p, n, 0, 2);
    LEAVE();
    return p;
}
