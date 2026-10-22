
#include "agthread.h"

#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif


struct ag_mutex {
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t m;
#endif
};

ag_mutex *ag_mutex_new(void)
{
    ag_mutex *m = (ag_mutex *)calloc(1, sizeof(ag_mutex));
    if (!m) return NULL;
#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    if (pthread_mutex_init(&m->m, NULL) != 0) { free(m); return NULL; }
#endif
    return m;
}

void ag_mutex_free(ag_mutex *m)
{
    if (!m) return;
#ifdef _WIN32
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->m);
#endif
    free(m);
}

void ag_mutex_lock(ag_mutex *m)
{
    if (!m) return;
#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->m);
#endif
}

void ag_mutex_unlock(ag_mutex *m)
{
    if (!m) return;
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->m);
#endif
}


struct ag_thread {
#ifdef _WIN32
    HANDLE h;
#else
    pthread_t t;
#endif
    ag_thread_fn fn;
    void *ud;
};

#ifdef _WIN32
static DWORD WINAPI ag_thread_trampoline(LPVOID p)
{
    ag_thread *t = (ag_thread *)p;
    t->fn(t->ud);
    return 0;
}
#else
static void *ag_thread_trampoline(void *p)
{
    ag_thread *t = (ag_thread *)p;
    t->fn(t->ud);
    return NULL;
}
#endif

ag_thread *ag_thread_start(ag_thread_fn fn, void *ud)
{
    if (!fn) return NULL;
    ag_thread *t = (ag_thread *)calloc(1, sizeof(ag_thread));
    if (!t) return NULL;
    t->fn = fn;
    t->ud = ud;
#ifdef _WIN32
    t->h = CreateThread(NULL, 0, ag_thread_trampoline, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
#else
    if (pthread_create(&t->t, NULL, ag_thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
#endif
    return t;
}

void ag_thread_join(ag_thread *t)
{
    if (!t) return;
#ifdef _WIN32
    if (t->h) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); }
#else
    pthread_join(t->t, NULL);
#endif
    free(t);
}


int ag_cpu_count(void)
{
    const char *env = getenv("AG_THREADS");
    if (env && *env) {
        int n = atoi(env);
        if (n >= 1) return n > 16 ? 16 : n;
    }
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

typedef struct {
    ag_work_fn fn;
    void      *ud;
    int        count;
    int        chunk;
    int        next;
    ag_mutex  *lock;
} ag_work;

static void ag_work_run(ag_work *w, int first, int last)
{
    for (int i = first; i < last; ++i)
        w->fn(w->ud, i, w->count);
}

#ifdef _WIN32
static DWORD WINAPI ag_work_thread(LPVOID p)
{
    ag_work *w = (ag_work *)p;
    for (;;) {
        int idx = -1;
        ag_mutex_lock(w->lock);
        if (w->next < w->count) idx = w->next++;
        ag_mutex_unlock(w->lock);
        if (idx < 0) break;
        int first = idx * w->chunk;
        int last = first + w->chunk;
        if (last > w->count) last = w->count;
        ag_work_run(w, first, last);
    }
    return 0;
}
#else
static void *ag_work_thread(void *p)
{
    ag_work *w = (ag_work *)p;
    for (;;) {
        int idx = -1;
        ag_mutex_lock(w->lock);
        if (w->next < w->count) idx = w->next++;
        ag_mutex_unlock(w->lock);
        if (idx < 0) break;
        int first = idx * w->chunk;
        int last = first + w->chunk;
        if (last > w->count) last = w->count;
        ag_work_run(w, first, last);
    }
    return NULL;
}
#endif

int ag_parallel_for(int count, ag_work_fn fn, void *ud)
{
    if (count <= 0 || !fn) return 0;

    int nthreads = ag_cpu_count();
    if (count < nthreads * 4) nthreads = 1;
    if (nthreads < 1) nthreads = 1;

    if (nthreads == 1) {
        for (int i = 0; i < count; ++i) fn(ud, i, count);
        return 1;
    }

    ag_work w;
    w.fn = fn;
    w.ud = ud;
    w.count = count;
    w.chunk = (count + nthreads - 1) / nthreads;
    w.next = 0;
    w.lock = ag_mutex_new();
    if (!w.lock) {
        for (int i = 0; i < count; ++i) fn(ud, i, count);
        return 1;
    }

#ifdef _WIN32
    HANDLE th[16];
    int started = 0;
    for (int i = 0; i < nthreads - 1 && i < 16; ++i) {
        th[started] = CreateThread(NULL, 0, ag_work_thread, &w, 0, NULL);
        if (th[started]) started++;
    }
    for (int i = 0; i < started; ++i) {
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
    }
#else
    pthread_t th[16];
    int started = 0;
    for (int i = 0; i < nthreads - 1 && i < 16; ++i) {
        if (pthread_create(&th[started], NULL, ag_work_thread, &w) == 0) started++;
    }
    for (int i = 0; i < started; ++i) pthread_join(th[i], NULL);
#endif

    for (;;) {
        int idx = -1;
        ag_mutex_lock(w.lock);
        if (w.next < w.count) idx = w.next++;
        ag_mutex_unlock(w.lock);
        if (idx < 0) break;
        int first = idx * w.chunk;
        int last = first + w.chunk;
        if (last > w.count) last = w.count;
        ag_work_run(&w, first, last);
    }

    ag_mutex_free(w.lock);
    return nthreads;
}
