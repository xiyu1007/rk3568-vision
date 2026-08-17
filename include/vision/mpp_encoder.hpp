// ============================================================================
// mpp_encoder.hpp — Rockchip MPP 硬件 H.264 编码器
// ============================================================================
//
// 用 Rockchip Media Process Platform（MPP）硬件编码器把 NV12 帧编码为 H.264。
// 与 H264Encoder（libx264 软编）接口一致，供 Pipeline 在「硬编优先、软编回退」策略下切换。
//
// MPP 输出 Annex-B 码流（00 00 00 01 起始码），本类内部转成 AVCC 格式
// （长度前缀），与 FFmpeg libx264 软编输出保持一致，从而让下游 Muxer/RTMP 无需改动。
//
// 注意：
//   - 依赖板端 librockchip-mpp（RK3568 系统自带，Makefile 链接 -lrockchip_mpp）。
//   - 输入必须是 NV12（MPP_FMT_YUV420SP），与 Frame 的 nv12_data 一致。
//   - 头文件用不透明指针声明 MPP 句柄，避免在 x86（无 MPP 头）环境编译失败；
//     真实类型在 mpp_encoder.cpp 里 include 板端 MPP 头文件。
// ============================================================================

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "vision/config.hpp"
#include "vision/types.hpp"

// MPP 句柄的前置声明（真实定义见板端 /usr/include/rockchip/*.h）。
// 注：MppCtx / MppBufferGroup 在 rk_type.h 里就是 typedef void*，这里用 void* 占位；
//     MppApi 是 struct MppApi_t（含函数指针表），用前置声明 + 指针。
struct MppApi_t;

namespace vision {

class MppEncoder {
public:
    MppEncoder() = default;
    ~MppEncoder();

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    // 打开硬件编码器。
    //   config：编码配置（bitrate/gop 等）
    //   width/height：帧宽高（NV12）
    //   fps：帧率
    bool Open(const EncodeConfig& config, uint32_t width, uint32_t height, uint32_t fps);

    // 编码一帧 NV12，产物经 on_packet 回调（AVPacket，AVCC 格式）。
    bool Encode(const FramePtr& frame,
                const std::function<void(const PacketPtr&)>& on_packet);

    // 冲刷编码器残留帧。
    void Flush(const std::function<void(const PacketPtr&)>& on_packet);

    // 关闭并释放 MPP 资源。
    void Close();

    // 获取 SPS/PPS（AVCC 格式，供封装器写流头）。
    const uint8_t* GetExtradata() const;
    int GetExtradataSize() const;

private:
    // 从 MPP 取 SPS/PPS 并转成 AVCC 格式的 extradata。
    bool PrepareExtradata();

    // 把一段 Annex-B 码流转成 AVPacket（AVCC 格式），经 on_packet 回调。
    void EmitAnnexBPacket(const uint8_t* data, size_t size, int64_t pts,
                          const std::function<void(const PacketPtr&)>& on_packet);

    void* ctx_ = nullptr;                              // MppCtx（typedef void*）
    struct MppApi_t* mpi_ = nullptr;                   // MppApi*
    void* buf_group_ = nullptr;                        // MppBufferGroup（typedef void*）

    std::vector<uint8_t> extradata_;   // SPS/PPS（AVCC 格式）
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 0;
    int64_t pts_ = 0;
    bool opened_ = false;
};

} // namespace vision
