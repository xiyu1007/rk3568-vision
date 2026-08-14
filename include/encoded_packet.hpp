// ============================================================================
// encoded_packet.hpp — H.264 编码数据包类型
// ============================================================================
//
// 定义编码线程产出的数据包类型 PacketPtr（std::shared_ptr<AVPacket>）。
//
// 为什么用 shared_ptr？
//   编码线程产出的一个 H.264 包要同时发给【推流线程】和【录制线程】两个
//   消费者。shared_ptr 自动管理引用计数，两个消费者都释放后才真正 free，
//   无需手动引用计数，也避免了数据竞争。
// ============================================================================

#pragma once

#include <memory>

#include <libavcodec/avcodec.h>   // AVPacket

namespace vision {

// AVPacket 的自定义删除器：用 av_packet_free 释放（含内部数据缓冲）。
struct PacketDeleter {
    void operator()(AVPacket* p) const {
        if (p) av_packet_free(&p);
    }
};

// 编码数据包共享指针。
using PacketPtr = std::shared_ptr<AVPacket>;

// 分配一个新的空 AVPacket（用 shared_ptr 管理，释放自动完成）。
inline PacketPtr makePacket() {
    return PacketPtr(av_packet_alloc(), PacketDeleter{});
}

} // namespace vision
