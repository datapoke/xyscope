#ifndef XYSCOPE_RINGBUFFER_H
#define XYSCOPE_RINGBUFFER_H

#include <stdlib.h>
#include <string.h>

/* Memory barrier helpers for SPSC correctness on ARM (Apple Silicon).
 * On x86 these compile to plain loads/stores. */
#if defined(__cplusplus) && (__cplusplus >= 201103L)
#include <atomic>
#define rb_store_release(ptr, val) \
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE)
#define rb_load_acquire(ptr) \
    __atomic_load_n(ptr, __ATOMIC_ACQUIRE)
#elif defined(__GNUC__) || defined(__clang__)
#define rb_store_release(ptr, val) \
    do { __atomic_store_n(ptr, val, __ATOMIC_RELEASE); } while(0)
#define rb_load_acquire(ptr) \
    __atomic_load_n(ptr, __ATOMIC_ACQUIRE)
#else
/* MSVC / fallback — full barrier */
#define rb_store_release(ptr, val) \
    do { _ReadWriteBarrier(); *(ptr) = (val); _ReadWriteBarrier(); } while(0)
#define rb_load_acquire(ptr) \
    ((_ReadWriteBarrier()), *(ptr))
#endif

typedef struct {
    char   *buf;
    size_t  size;
    size_t  write_ptr;
    size_t  read_ptr;
} ringbuffer_t;

static inline ringbuffer_t *ringbuffer_create(size_t size) {
    ringbuffer_t *rb = (ringbuffer_t *)malloc(sizeof(ringbuffer_t));
    size_t power_of_two = 1;
    while (power_of_two < size) power_of_two <<= 1;
    rb->size = power_of_two;
    rb->buf = (char *)malloc(rb->size);
    rb->write_ptr = 0;
    rb->read_ptr = 0;
    return rb;
}

static inline void ringbuffer_free(ringbuffer_t *rb) {
    if (rb) {
        free(rb->buf);
        free(rb);
    }
}

static inline size_t ringbuffer_write_space(ringbuffer_t *rb) {
    size_t w = rb->write_ptr;
    size_t r = rb_load_acquire(&rb->read_ptr);
    if (w > r) {
        return ((r - w + rb->size) & (rb->size - 1)) - 1;
    } else if (w < r) {
        return (r - w) - 1;
    } else {
        return rb->size - 1;
    }
}

static inline size_t ringbuffer_read_space(ringbuffer_t *rb) {
    size_t w = rb_load_acquire(&rb->write_ptr);
    size_t r = rb->read_ptr;
    if (w > r) {
        return w - r;
    } else {
        return (w - r + rb->size) & (rb->size - 1);
    }
}

static inline size_t ringbuffer_write(ringbuffer_t *rb, const char *src, size_t cnt) {
    size_t free_cnt;
    size_t cnt2;
    size_t to_write;
    size_t n1, n2;

    free_cnt = ringbuffer_write_space(rb);
    if (free_cnt == 0) return 0;

    to_write = cnt > free_cnt ? free_cnt : cnt;
    cnt2 = rb->write_ptr + to_write;

    if (cnt2 > rb->size) {
        n1 = rb->size - rb->write_ptr;
        n2 = cnt2 & (rb->size - 1);
    } else {
        n1 = to_write;
        n2 = 0;
    }

    memcpy(rb->buf + rb->write_ptr, src, n1);
    if (n2) {
        size_t wrap = (rb->write_ptr + n1) & (rb->size - 1);
        memcpy(rb->buf + wrap, src + n1, n2);
    }

    /* Release barrier: ensure memcpy is visible before advancing write_ptr */
    rb_store_release(&rb->write_ptr,
        (rb->write_ptr + to_write) & (rb->size - 1));

    return to_write;
}

static inline size_t ringbuffer_read(ringbuffer_t *rb, char *dest, size_t cnt) {
    size_t free_cnt;
    size_t cnt2;
    size_t to_read;
    size_t n1, n2;

    free_cnt = ringbuffer_read_space(rb);
    if (free_cnt == 0) return 0;

    to_read = cnt > free_cnt ? free_cnt : cnt;
    cnt2 = rb->read_ptr + to_read;

    if (cnt2 > rb->size) {
        n1 = rb->size - rb->read_ptr;
        n2 = cnt2 & (rb->size - 1);
    } else {
        n1 = to_read;
        n2 = 0;
    }

    memcpy(dest, rb->buf + rb->read_ptr, n1);
    if (n2) {
        size_t wrap = (rb->read_ptr + n1) & (rb->size - 1);
        memcpy(dest + n1, rb->buf + wrap, n2);
    }

    /* Release barrier: ensure memcpy is done before advancing read_ptr */
    rb_store_release(&rb->read_ptr,
        (rb->read_ptr + to_read) & (rb->size - 1));

    return to_read;
}

static inline void ringbuffer_read_advance(ringbuffer_t *rb, size_t cnt) {
    rb_store_release(&rb->read_ptr,
        (rb->read_ptr + cnt) & (rb->size - 1));
}

#endif /* XYSCOPE_RINGBUFFER_H */
