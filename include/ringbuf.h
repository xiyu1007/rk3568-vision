#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SPSC (Single Producer, Single Consumer) lock-free ring buffer.
 * Zero dynamic allocation — caller provides storage.
 *
 * Usage:
 *   ringbuf_t rb;
 *   uint8_t* items[RINGBUF_CAPACITY];
 *   ringbuf_init(&rb, items, RINGBUF_CAPACITY);
 *   // Producer:
 *   ringbuf_push(&rb, ptr);   // returns false if full
 *   // Consumer:
 *   void* item = ringbuf_pop(&rb); // returns NULL if empty
 */

#define RINGBUF_ALIGN 64

typedef struct {
    void*  *buf;        /* caller-provided array of void*   */
    size_t  cap;         /* buf capacity (actual slots = cap-1) */
    size_t  _head __attribute__((aligned(RINGBUF_ALIGN)));
    size_t  _pad1[14];   /* pad to 64 bytes (head cache line)   */
    size_t  _tail __attribute__((aligned(RINGBUF_ALIGN)));
    size_t  _pad2[14];   /* pad to 64 bytes (tail cache line)   */
} ringbuf_t;

static inline void ringbuf_init(ringbuf_t* rb, void** storage, size_t capacity) {
    rb->buf  = storage;
    rb->cap  = capacity + 1;  /* +1 to distinguish full vs empty */
    rb->_head = 0;
    rb->_tail = 0;
}

static inline bool ringbuf_push(ringbuf_t* rb, void* item) {
    size_t tail = __atomic_load_n(&rb->_tail, __ATOMIC_RELAXED);
    size_t next = (tail + 1) % rb->cap;
    if (next == __atomic_load_n(&rb->_head, __ATOMIC_ACQUIRE))
        return false;  /* full */
    rb->buf[tail] = item;
    __atomic_store_n(&rb->_tail, next, __ATOMIC_RELEASE);
    return true;
}

static inline void* ringbuf_pop(ringbuf_t* rb) {
    size_t head = __atomic_load_n(&rb->_head, __ATOMIC_RELAXED);
    if (head == __atomic_load_n(&rb->_tail, __ATOMIC_ACQUIRE))
        return NULL;  /* empty */
    void* item = rb->buf[head];
    __atomic_store_n(&rb->_head, (head + 1) % rb->cap, __ATOMIC_RELEASE);
    return item;
}

static inline bool ringbuf_empty(const ringbuf_t* rb) {
    return __atomic_load_n(&rb->_head, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&rb->_tail, __ATOMIC_ACQUIRE);
}

static inline size_t ringbuf_size(const ringbuf_t* rb) {
    size_t head = __atomic_load_n(&rb->_head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&rb->_tail, __ATOMIC_ACQUIRE);
    return (tail >= head) ? (tail - head) : (rb->cap - head + tail);
}

static inline void ringbuf_clear(ringbuf_t* rb) {
    __atomic_store_n(&rb->_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rb->_tail, 0, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */
