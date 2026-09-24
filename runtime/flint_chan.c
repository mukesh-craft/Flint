// flint_chan.c — v0.22: bounded MPMC channels for i64 messages.
//
// The safe handoff primitive behind Flint's concurrency story: share by
// communicating instead of sharing memory. Blocking send/recv with close
// semantics; every failure mode is loud (err flag or panic), never silent.
//
// Contract: an endpoint must be joined/finished before flint_chan_free
// (like pthreads: no use-after-free). Messages are i64 values; pass heap
// objects by handle (map/aegis lease ids are i64 already).
// PORT: pthread mutex/cond — POSIX + MinGW fine; MSVC needs native
// primitives (same documented gap as flint_thread_*).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

extern void flint_panic(const char* msg);
extern void flint_set_err(int64_t err);

typedef struct {
    int64_t* buf;
    int64_t cap;    // >= 1 (clamped at creation)
    int64_t head;   // dequeue index
    int64_t tail;   // enqueue index
    int64_t count;  // buffered messages
    int closed;     // no more sends; buffered messages still drain
    pthread_mutex_t mtx;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} FlintChan;

void* flint_chan_new(int64_t cap) {
    if (cap < 1) cap = 1;
    // Overflow guard on cap * 8.
    if ((uint64_t)cap > (uint64_t)SIZE_MAX / sizeof(int64_t)) {
        flint_set_err(1);
        return NULL;
    }
    FlintChan* ch = (FlintChan*)malloc(sizeof(FlintChan));
    if (!ch) { flint_set_err(1); return NULL; }
    ch->buf = (int64_t*)malloc((size_t)cap * sizeof(int64_t));
    if (!ch->buf) { free(ch); flint_set_err(1); return NULL; }
    ch->cap = cap;
    ch->head = 0;
    ch->tail = 0;
    ch->count = 0;
    ch->closed = 0;
    pthread_mutex_init(&ch->mtx, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    return ch;
}

void flint_chan_free(void* p) {
    FlintChan* ch = (FlintChan*)p;
    if (!ch) return;
    pthread_mutex_destroy(&ch->mtx);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    free(ch->buf);
    free(ch);
}

int64_t flint_chan_send(void* p, int64_t v) {
    FlintChan* ch = (FlintChan*)p;
    if (!ch) { flint_set_err(1); return -1; }
    pthread_mutex_lock(&ch->mtx);
    while (ch->count >= ch->cap && !ch->closed)
        pthread_cond_wait(&ch->not_full, &ch->mtx);
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mtx);
        flint_set_err(1);
        return -1;
    }
    ch->buf[ch->tail] = v;
    ch->tail = (ch->tail + 1) % ch->cap;
    ch->count++;
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mtx);
    return 0;
}

int64_t flint_chan_recv(void* p) {
    FlintChan* ch = (FlintChan*)p;
    if (!ch) flint_panic("chan_recv: null channel");
    pthread_mutex_lock(&ch->mtx);
    while (ch->count <= 0 && !ch->closed)
        pthread_cond_wait(&ch->not_empty, &ch->mtx);
    if (ch->count <= 0) {
        // Closed and drained: loud panic, never a silent bogus value.
        pthread_mutex_unlock(&ch->mtx);
        flint_panic("chan_recv: channel closed and empty");
    }
    int64_t v = ch->buf[ch->head];
    ch->head = (ch->head + 1) % ch->cap;
    ch->count--;
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mtx);
    return v;
}

int64_t flint_chan_close(void* p) {
    FlintChan* ch = (FlintChan*)p;
    if (!ch) { flint_set_err(1); return -1; }
    pthread_mutex_lock(&ch->mtx);
    ch->closed = 1;
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mtx);
    return 0;
}

int64_t flint_chan_len(void* p) {
    FlintChan* ch = (FlintChan*)p;
    if (!ch) { flint_set_err(1); return -1; }
    pthread_mutex_lock(&ch->mtx);
    int64_t n = ch->count;
    pthread_mutex_unlock(&ch->mtx);
    return n;
}
