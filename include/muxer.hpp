// ============================================================================
// muxer.hpp — FFmpeg 封装输出器（RTMP 推流 / MP4 录制共用）
// ============================================================================
//
// 职责：把 H.264 编码包封装成指定容器格式并写出。
//
// 两种用法：
//   - format="flv", url="rtmp://..." → RTMP 推流（远程展示）
//   - format="mp4", url="output/xxx.mp4" → 本地录制（保存 MP4 文件）
//
// 封装流程（FFmpeg libavformat 标准流程）：
//   1. avformat_alloc_output_context2 创建输出上下文
//   2. avformat_new_stream 新建视频流并设置 H.264 参数
//   3. avio_open 打开目标（RTMP 建连 / 打开本地文件）
//   4. avformat_write_header 写容器头
//   5. [循环] av_interleaved_write_frame 写每个编码包
//   6. av_write_trailer + avio_close 收尾
// ============================================================================

#pragma once

#include <cstdint>
#include <string>

#include "encoded_packet.hpp"

struct AVFormatContext;
struct AVStream;

namespace vision {

class Muxer {
public:
    Muxer() = default;
    ~Muxer();

    Muxer(const Muxer&) = delete;
    Muxer& operator=(const Muxer&) = delete;

    // 打开封装器。
    //   extradata/extradata_size：编码器的 SPS/PPS（H.264 解码器初始化必需）
    bool open(const std::string& format, const std::string& url,
              uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate,
              const uint8_t* extradata, int extradata_size);

    // 写入一个编码包。返回 false 表示写入失败（如网络断开/磁盘满）。
    bool push(const PacketPtr& pkt);

    // 关闭并释放资源。
    void close();

    bool isOpen() const { return ctx_ != nullptr; }

private:
    AVFormatContext* ctx_ = nullptr;   // FFmpeg 输出上下文
    AVStream*        vs_  = nullptr;   // 视频流
    uint32_t         fps_ = 25;        // 帧率（用于时间基换算）
};

} // namespace vision
