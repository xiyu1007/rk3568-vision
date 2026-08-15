// ============================================================================
// ring_buffer.hpp — 有界环形缓冲区（生产者-消费者通信核心）
// ============================================================================
//
// 职责：在流水线相邻两级之间传递帧指针，实现模块解耦与线程同步。
//
// 设计要点：
//   1. 【环形结构】用固定大小的 vector + 头尾指针实现，不动态扩容，
//      避免高负载下的内存分配抖动。
//   2. 【条件变量】阻塞等待用 std::condition_variable，而非忙轮询
//      （消费者在队列空时休眠，生产者入队时唤醒，CPU 占用极低）。
//   3. 【丢最旧】生产者 push 时若队列已满，丢弃队首最旧元素再写入，
//      保证流水线始终消费【最新】的帧，避免延迟累积。
//   4. 【优雅关闭】close() 会唤醒所有阻塞中的消费者，使 pop 立即返回
//      false，配合流水线的 running_ 标志实现线程优雅退出。
//
// 线程安全模型：
//   单生产者单消费者（SPSC）语义足够本项目使用：
//     cap_q    采集 → 稳帧
//     inf_q    稳帧 → 推理
//     enc_q    推理 → 编码
//     push_q   编码 → 推流
//     record_q 编码 → 录制
//   为简单与健壮，内部仍用 mutex 保护（25fps 场景锁开销可忽略）。
// ============================================================================

#pragma once

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>
#include <utility>

namespace vision {

template <typename T>
class RingBuffer {
public:
    // 构造：capacity 为可容纳元素个数（不含哨兵，内部直接按此分配）。
    explicit RingBuffer(size_t capacity)
        : capacity_(capacity), buffer_(capacity) {}

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ----------------------------------------------------------------------
    // push — 生产者入队（丢最旧策略，永不阻塞）
    // ----------------------------------------------------------------------
    // 队列满时丢弃队首最旧元素，再写入新元素，并累计 dropped_ 计数。
    // 这样做的原因：在实时视频流水线中，旧帧的价值远低于新帧，
    // 宁可丢旧帧，也不让延迟越积越长。
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == capacity_) {
            // 队列已满：丢弃最旧元素（head 前移）
            head_ = (head_ + 1) % capacity_;
            --size_;
            ++dropped_;
        }
        buffer_[tail_] = std::move(item);   // 写入新元素
        tail_ = (tail_ + 1) % capacity_;     // 尾指针前移
        ++size_;
        not_empty_.notify_one();             // 唤醒一个等待的消费者
    }

    // ----------------------------------------------------------------------
    // pop — 消费者阻塞取出（队列空时休眠，直到有数据或 close）
    // ----------------------------------------------------------------------
    // 返回 false 表示队列已被 close（流水线关闭），调用方应据此退出线程。
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        // wait：当 size_>0 或 closed_ 成立时返回，否则释放锁休眠
        not_empty_.wait(lock, [this] { return size_ > 0 || closed_; });
        if (size_ == 0) {
            return false;                   // 队列空且已关闭
        }
        out = std::move(buffer_[head_]);    // 取出队首
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }

    // ----------------------------------------------------------------------
    // popFor — 带超时的消费者取出（用于稳帧器按节拍等待）
    // ----------------------------------------------------------------------
    // 返回值：
    //   true  —— 取到了元素
    //   false —— 超时（timeout 内没有数据）或队列已关闭
    bool popFor(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, timeout,
                                 [this] { return size_ > 0 || closed_; })) {
            return false;                   // 超时，无数据
        }
        if (size_ == 0) {
            return false;                   // 队列空且已关闭
        }
        out = std::move(buffer_[head_]);
        head_ = (head_ + 1) % capacity_;
        --size_;
        return true;
    }

    // ----------------------------------------------------------------------
    // close — 关闭队列，唤醒所有阻塞的消费者
    // ----------------------------------------------------------------------
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
    }

    // 当前元素个数（调试/监控用）。
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

    // 累计被丢弃（丢最旧）的元素个数（性能统计用）。
    uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    size_t         capacity_;               // 容量（可容纳元素数）
    std::vector<T> buffer_;                 // 环形存储区
    size_t         head_ = 0;               // 队首（读位置）
    size_t         tail_ = 0;               // 队尾（写位置）
    size_t         size_  = 0;              // 当前元素个数
    bool           closed_ = false;         // 关闭标志
    uint64_t       dropped_ = 0;            // 累计丢帧数

    mutable std::mutex      mutex_;         // 保护上述字段
    std::condition_variable not_empty_;     // 非空条件变量（消费者等待）
};

} // namespace vision
