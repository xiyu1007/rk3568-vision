#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ==========================================================================
 * SPSC 无锁环形队列（Single Producer, Single Consumer Lock-Free Ring Buffer）
 * ==========================================================================
 *
 * **设计目标**：在采集/推理/编码/显示线程之间高效传递帧数据指针
 *   只传递指针（8字节），不拷贝帧数据本体，实现真正的"零数据拷贝"通信
 *
 * **为什么用无锁队列？**
 *   - 传统的 mutex + condition_variable 每次操作都要进出内核态（~1μs开销）
 *   - 在高吞吐（30fps × 多帧缓存）场景下，锁竞争会严重降低性能
 *   - 无锁队列只在用户态通过原子指令完成，延迟 <50ns
 *
 * **为什么 SPSC（单生产者单消费者）？**
 *   - 本项目的每个队列都有明确的生产者和消费者：
 *     cap_q:  采集线程(生产) → 推理线程(消费)
 *     inf_q:  推理线程(生产) → 编码线程(消费)
 *     disp_q: 推理线程(生产) → 显示线程(消费)
 *   - SPSC 比 MPSC/MPMC 简单得多，不需要复杂的 CAS 循环
 *   - 没有 ABA 问题（不涉及节点复用）
 *
 * **Cache Line 对齐（关键性能优化）**
 *   CPU 缓存以 64 字节（Cache Line）为单位加载数据
 *   head 和 tail 分别在两个独立的 Cache Line 上，避免：
 *     - False Sharing（伪共享）：生产者写 tail 时，不会使消费者缓存的 head 失效
 *     - 同样，消费者写 head 时，不会使生产者缓存的 tail 失效
 *   RINGBUF_ALIGN=64 保证了 head 和 tail 各自占据完整的 Cache Line
 *   _pad1 和 _pad2 数组填充到 64 字节边界
 *
 * **容量设计：cap-1 可用槽位**
 *   队列实际容量 = 传入容量 + 1
 *   利用一个"哨兵"槽位区分队空和队满：
 *     队空：head == tail
 *     队满：(tail + 1) % cap == head
 *   这样 push 永远不会覆盖未消费的数据
 *
 * **内存模型（Memory Order）**
 *   push 端：
 *     - RELAXED 读 tail（只有生产者自己写 tail）
 *     - ACQUIRE 读 head（看到消费者对 head 的写入及其之前的操作）
 *     - RELEASE 写 tail（保证数据写入在 tail 更新之前对其他线程可见）
 *   pop 端：
 *     - RELAXED 读 head（只有消费者自己写 head）
 *     - ACQUIRE 读 tail（看到生产者对 tail 的写入及其之前的数据写入）
 *     - RELEASE 写 head（保证数据读取在 head 更新之前完成）
 *
 * **用法示例：**
 *   ringbuf_t rb;
 *   void* items[QUEUE_CAP + 1];           // +1 用于区分满/空
 *   ringbuf_init(&rb, items, QUEUE_CAP);  // 实际可存 QUEUE_CAP 个元素
 *   // 生产者：
 *   ringbuf_push(&rb, ptr);   // 返回 false = 队列满，丢弃
 *   // 消费者：
 *   void* item = ringbuf_pop(&rb); // 返回 NULL = 队列空
 */

#define RINGBUF_ALIGN 64   /* CPU Cache Line 大小（ARM Cortex-A55） */

typedef struct {
    void*  *buf;                /* 调用者提供的 void* 数组（外部静态分配，零动态内存） */
    size_t  cap;                /* buf 总容量（实际可用 = cap-1，参见上方说明） */

    /* ── 生产者端（写指针） ──────────────────────────────────────── */
    size_t  _head __attribute__((aligned(RINGBUF_ALIGN))); /* 强制 64 字节对齐 */
    size_t  _pad1[14];         /* 填充至 64 字节（head 占 8B + 14×8B = 120B... 实际上
                                 * 需要精确计算：head(8B) + 14×8B = 120B，加上之前的
                                 * buf(8B) + cap(8B) = 16B，需要 pad 到 64B 边界 */

    /* ── 消费者端（读指针） ──────────────────────────────────────── */
    size_t  _tail __attribute__((aligned(RINGBUF_ALIGN))); /* 强制 64 字节对齐 */
    size_t  _pad2[14];         /* 填充至 64 字节 */

    /*
     * 命名注意：head = 生产者写入位置（push 时移动）
     *          tail = 消费者读取位置（pop 时移动）
     * 这与某些实现的命名相反，但在本项目中约定如上
     */
} ringbuf_t;

/*
 * 初始化环形队列
 * @storage：预先分配的 void* 数组（大小 = capacity + 1）
 * @capacity：期望的可存储元素数量（实际内部会 +1）
 */
static inline void ringbuf_init(ringbuf_t* rb, void** storage, size_t capacity) {
    rb->buf   = storage;
    rb->cap   = capacity + 1;   /* +1 槽位用于区分队空和队满状态 */
    rb->_head = 0;               /* head 初始为 0（生产者从 0 开始写入） */
    rb->_tail = 0;               /* tail 初始为 0（消费者从 0 开始读取） */
}

/*
 * 生产者：向队列推入一个元素（void* 指针）
 * 返回 true 表示成功，false 表示队列已满（调用者负责处理丢帧逻辑）
 * 
 * 执行流程：
 *   1. RELAXED 读取 tail（只有自己写 tail，不需要同步）
 *   2. 计算 next = (tail + 1) % cap
 *   3. ACQUIRE 读取 head（需要看到消费者对 head 的最新写入）
 *   4. 如果 next == head → 队列满，丢弃
 *   5. 写入数据到 buf[tail]
 *   6. RELEASE 写入 tail = next（让消费者看到新数据）
 */
static inline bool ringbuf_push(ringbuf_t* rb, void* item) {
    size_t tail = __atomic_load_n(&rb->_tail, __ATOMIC_RELAXED);
    size_t next = (tail + 1) % rb->cap;
    if (next == __atomic_load_n(&rb->_head, __ATOMIC_ACQUIRE))
        return false;           /* 队列满：next 追上了 head */
    rb->buf[tail] = item;
    __atomic_store_n(&rb->_tail, next, __ATOMIC_RELEASE);
    return true;
}

/*
 * 消费者：从队列取出一个元素（void* 指针）
 * 返回取出的指针，NULL 表示队列为空
 *
 * 执行流程：
 *   1. RELAXED 读取 head（只有自己写 head）
 *   2. ACQUIRE 读取 tail（需要看到生产者对 tail 的最新写入）
 *   3. 如果 head == tail → 队列空，返回 NULL
 *   4. 读取数据 buf[head]
 *   5. RELEASE 写入 head = (head + 1) % cap（让生产者看到空间释放）
 */
static inline void* ringbuf_pop(ringbuf_t* rb) {
    size_t head = __atomic_load_n(&rb->_head, __ATOMIC_RELAXED);
    if (head == __atomic_load_n(&rb->_tail, __ATOMIC_ACQUIRE))
        return NULL;            /* 队列空：head 追上了 tail */
    void* item = rb->buf[head];
    __atomic_store_n(&rb->_head, (head + 1) % rb->cap, __ATOMIC_RELEASE);
    return item;
}

/* 判断队列是否为空（head == tail） */
static inline bool ringbuf_empty(const ringbuf_t* rb) {
    return __atomic_load_n(&rb->_head, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&rb->_tail, __ATOMIC_ACQUIRE);
}

/* 返回队列中当前的元素数量 */
static inline size_t ringbuf_size(const ringbuf_t* rb) {
    size_t head = __atomic_load_n(&rb->_head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&rb->_tail, __ATOMIC_ACQUIRE);
    return (tail >= head) ? (tail - head) : (rb->cap - head + tail);
}

/* 清空队列（仅在 shutdown 时使用，不保证数据安全） */
static inline void ringbuf_clear(ringbuf_t* rb) {
    __atomic_store_n(&rb->_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&rb->_tail, 0, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */
