#pragma once

#include <cstddef>
#include <atomic>
#include <vector>
#include <memory>

namespace rk3568_vision {

// ============================================================================
// 环形队列 (RingBuffer) — 单生产者单消费者 (SPSC)
//
// 为什么用 SPSC 而非 MPSC:
// - Pipeline 中每个阶段只有一个生产者和一个消费者
// - SPSC 可以实现真正的 wait-free (无需互斥锁)
// - 使用 memory_order 控制缓存一致性，避免不必要的 fence
//
// Cache line 对齐 (64字节):
// - 防止 false sharing: 生产者和消费者的指针在不同 cache line
// - RK3568 Cortex-A55 L1 cache line = 64 bytes
// ============================================================================
template <typename T>
class alignas(64) RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity + 1)  // +1: 区分空/满
        , buffer_(capacity_)
    {}

    ~RingBuffer() = default;

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // 生产者: push（非阻塞，失败返回 false）
    bool push(T&& item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next = (tail + 1) % capacity_;

        // 队列满（需要留一个空位区分空/满）
        if (next == head_.load(std::memory_order_acquire))
            return false;

        buffer_[tail] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // 消费者: pop（非阻塞，失败返回 false）
    bool pop(T& item) {
        size_t head = head_.load(std::memory_order_relaxed);

        // 队列空
        if (head == tail_.load(std::memory_order_acquire))
            return false;

        item = std::move(buffer_[head]);
        head_.store((head + 1) % capacity_, std::memory_order_release);
        return true;
    }

    // 非破坏性查看队首
    bool peek(T& item) {
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return false;
        item = buffer_[head];
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    size_t size() const {
        size_t head = head_.load(std::memory_order_acquire);
        size_t tail = tail_.load(std::memory_order_acquire);
        return (tail >= head) ? (tail - head) : (capacity_ - head + tail);
    }

    size_t capacity() const { return capacity_ - 1; }

    void clear() {
        head_.store(0, std::memory_order_release);
        tail_.store(0, std::memory_order_release);
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;

    // cache line 隔离：head 和 tail 在不同 cache line
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace rk3568_vision
