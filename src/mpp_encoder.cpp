// ============================================================================
// mpp_encoder.cpp — Rockchip MPP 硬件 H.264 编码器实现
// ============================================================================

#include "vision/mpp_encoder.hpp"

#include <cstring>

#if defined(__aarch64__)
#include "rockchip/rk_mpi.h"
#include "rockchip/mpp_frame.h"
#include "rockchip/mpp_packet.h"
#include "rockchip/mpp_buffer.h"
#include "rockchip/mpp_err.h"
#include "rockchip/rk_venc_cfg.h"
#endif

#include "vision/logger.hpp"

namespace vision {

#if defined(__aarch64__)

namespace {

// 查找下一个 Annex-B 起始码（00 00 01 或 00 00 00 01），返回起始码长度（3 或 4）。
// 找不到返回 0。data/size 为待扫描区域。
int FindStartCode(const uint8_t* data, size_t size, size_t* code_len) {
    for (size_t i = 0; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *code_len = 3;
            return static_cast<int>(i);
        }
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            *code_len = 4;
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 把一段 Annex-B 数据（可能含多个 NAL）按 NAL 切分，回调每个 NAL。
// 回调参数：nal 数据指针、长度（不含起始码）。
void ParseAnnexB(const uint8_t* data, size_t size,
                 const std::function<void(const uint8_t*, size_t)>& on_nal) {
    size_t pos = 0;
    // 跳过开头可能的起始码。
    size_t code_len = 0;
    int start = FindStartCode(data, size, &code_len);
    if (start > 0) {
        // 起始码前有数据（理论上不该出现），跳过。
        pos = 0;
    } else if (start == 0) {
        pos = static_cast<size_t>(code_len);
    } else {
        return;
    }

    while (pos < size) {
        size_t next_len = 0;
        int next = FindStartCode(data + pos, size - pos, &next_len);
        if (next < 0) {
            // 最后一个 NAL。
            on_nal(data + pos, size - pos);
            break;
        }
        if (next > 0) {
            on_nal(data + pos, static_cast<size_t>(next));
        }
        pos += static_cast<size_t>(next) + next_len;
    }
}

// 从 MPP 的 SPS/PPS（Annex-B）构造 AVCC 格式的 extradata（AVCDecoderConfigurationRecord）。
// sps_pps 为 Annex-B 数据（含 SPS 和 PPS 的起始码），输出到 out。
bool BuildAvccExtradata(const uint8_t* sps_pps, size_t size, std::vector<uint8_t>& out) {
    std::vector<const uint8_t*> sps_list;
    std::vector<size_t> sps_len;
    std::vector<const uint8_t*> pps_list;
    std::vector<size_t> pps_len;

    ParseAnnexB(sps_pps, size, [&](const uint8_t* nal, size_t len) {
        if (len == 0) {
            return;
        }
        const int type = nal[0] & 0x1F;
        if (type == 7) {          // SPS
            sps_list.push_back(nal);
            sps_len.push_back(len);
        } else if (type == 8) {   // PPS
            pps_list.push_back(nal);
            pps_len.push_back(len);
        }
    });

    if (sps_list.empty() || pps_list.empty() || sps_len[0] < 4) {
        return false;
    }

    out.clear();
    out.push_back(0x01);                       // configurationVersion
    out.push_back(sps_list[0][1]);             // AVCProfileIndication
    out.push_back(sps_list[0][2]);             // profile_compatibility
    out.push_back(sps_list[0][3]);             // AVCLevelIndication
    out.push_back(0xFF);                       // lengthSizeMinusOne(3) | 111100
    out.push_back(0xE0 | static_cast<uint8_t>(sps_list.size()));  // numOfSequenceParameterSets
    for (size_t i = 0; i < sps_list.size(); ++i) {
        out.push_back(static_cast<uint8_t>(sps_len[i] >> 8));
        out.push_back(static_cast<uint8_t>(sps_len[i] & 0xFF));
        out.insert(out.end(), sps_list[i], sps_list[i] + sps_len[i]);
    }
    out.push_back(static_cast<uint8_t>(pps_list.size()));  // numOfPictureParameterSets
    for (size_t i = 0; i < pps_list.size(); ++i) {
        out.push_back(static_cast<uint8_t>(pps_len[i] >> 8));
        out.push_back(static_cast<uint8_t>(pps_len[i] & 0xFF));
        out.insert(out.end(), pps_list[i], pps_list[i] + pps_len[i]);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 析构
// ---------------------------------------------------------------------------
MppEncoder::~MppEncoder() {
    Close();
}

// ---------------------------------------------------------------------------
// Open：创建 MPP 上下文并配置 H264 编码
// ---------------------------------------------------------------------------
bool MppEncoder::Open(const EncodeConfig& config, uint32_t width, uint32_t height,
                      uint32_t fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;

    // 1. 创建 MPP 上下文。
    if (mpp_create(&ctx_, &mpi_) != MPP_OK || ctx_ == nullptr || mpi_ == nullptr) {
        Logger::instance().error("mpp: mpp_create failed");
        Close();
        return false;
    }

    // 2. 初始化为 H.264 编码器。
    if (mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        Logger::instance().error("mpp: mpp_init(ENC, AVC) failed");
        Close();
        return false;
    }

    // 3. 配置编码参数。
    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);
    mpp_enc_cfg_set_s32(cfg, "prep:width", static_cast<RK_S32>(width));
    mpp_enc_cfg_set_s32(cfg, "prep:height", static_cast<RK_S32>(height));
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", static_cast<RK_S32>(width));
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", static_cast<RK_S32>(height));
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);  // NV12
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", static_cast<RK_S32>(config.bitrate));
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", static_cast<RK_S32>(config.bitrate * 17 / 16));
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", static_cast<RK_S32>(config.bitrate * 15 / 16));
    mpp_enc_cfg_set_s32(cfg, "rc:gop", static_cast<RK_S32>(config.gop_size));
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", static_cast<RK_S32>(fps));
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    // profile：100=high / 77=main / 66=baseline。用 high 匹配软编配置。
    mpp_enc_cfg_set_s32(cfg, "codec:profile", 100);
    if (mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        Logger::instance().error("mpp: MPP_ENC_SET_CFG failed");
        mpp_enc_cfg_deinit(cfg);
        Close();
        return false;
    }
    mpp_enc_cfg_deinit(cfg);

    // 4. 头信息（SPS/PPS）单独获取，不塞进每个 IDR 帧。
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_DEFAULT;
    mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &header_mode);

    // 5. 输入 buffer 组（DRM 内存）。
    if (mpp_buffer_group_get_internal(&buf_group_, MPP_BUFFER_TYPE_DRM) != MPP_OK ||
        buf_group_ == nullptr) {
        Logger::instance().error("mpp: buffer group create failed");
        Close();
        return false;
    }

    // 6. 取 SPS/PPS。
    if (!PrepareExtradata()) {
        Logger::instance().warn("mpp: get extradata failed, will retry on first IDR");
    }

    opened_ = true;
    Logger::instance().info("mpp: opened h264 encoder %ux%u@%u bitrate=%u gop=%u",
                            width, height, fps, config.bitrate, config.gop_size);
    return true;
}

// ---------------------------------------------------------------------------
// PrepareExtradata：取 SPS/PPS 转 AVCC
// ---------------------------------------------------------------------------
bool MppEncoder::PrepareExtradata() {
    MppPacket extra = nullptr;
    if (mpi_->control(ctx_, MPP_ENC_GET_EXTRA_INFO, &extra) != MPP_OK || extra == nullptr) {
        return false;
    }
    void* pos = mpp_packet_get_pos(extra);
    size_t len = mpp_packet_get_length(extra);
    bool ok = pos != nullptr && len > 0 &&
              BuildAvccExtradata(static_cast<const uint8_t*>(pos), len, extradata_);
    mpp_packet_deinit(&extra);
    return ok;
}

// ---------------------------------------------------------------------------
// Encode：编码一帧 NV12
// ---------------------------------------------------------------------------
bool MppEncoder::Encode(const FramePtr& frame,
                        const std::function<void(const PacketPtr&)>& on_packet) {
    if (!opened_ || !frame) {
        return false;
    }

    // 1. 从 buffer 组取一块内存，拷贝 NV12。
    const size_t frame_size = static_cast<size_t>(width_) * height_ * 3 / 2;
    MppBuffer buffer = nullptr;
    if (mpp_buffer_get(buf_group_, &buffer, frame_size) != MPP_OK || buffer == nullptr) {
        Logger::instance().warn("mpp: buffer_get failed");
        return false;
    }
    void* ptr = mpp_buffer_get_ptr(buffer);
    std::memcpy(ptr, frame->nv12_data, frame_size);

    // 2. 构造 MppFrame 并提交。
    MppFrame mframe = nullptr;
    mpp_frame_init(&mframe);
    mpp_frame_set_width(mframe, static_cast<RK_S32>(width_));
    mpp_frame_set_height(mframe, static_cast<RK_S32>(height_));
    mpp_frame_set_hor_stride(mframe, static_cast<RK_S32>(width_));
    mpp_frame_set_ver_stride(mframe, static_cast<RK_S32>(height_));
    mpp_frame_set_fmt(mframe, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(mframe, buffer);
    mpp_frame_set_pts(mframe, pts_++);
    mpp_buffer_put(buffer);

    mpi_->encode_put_frame(ctx_, mframe);
    mpp_frame_deinit(&mframe);

    // 3. 取编码结果。
    MppPacket packet = nullptr;
    while (mpi_->encode_get_packet(ctx_, &packet) == MPP_OK && packet != nullptr) {
        void* data = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);
        int64_t pts = mpp_packet_get_pts(packet);
        if (data != nullptr && len > 0) {
            // 首帧含 SPS/PPS 时顺带补齐 extradata（GET_EXTRA_INFO 失败时的兜底）。
            if (extradata_.empty()) {
                std::vector<uint8_t> tmp;
                if (BuildAvccExtradata(static_cast<const uint8_t*>(data), len, tmp)) {
                    extradata_ = std::move(tmp);
                }
            }
            EmitAnnexBPacket(static_cast<const uint8_t*>(data), len, pts, on_packet);
        }
        mpp_packet_deinit(&packet);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Flush：发送 EOS 并冲刷残留帧
// ---------------------------------------------------------------------------
void MppEncoder::Flush(const std::function<void(const PacketPtr&)>& on_packet) {
    if (!opened_) {
        return;
    }
    MppFrame mframe = nullptr;
    mpp_frame_init(&mframe);
    mpp_frame_set_eos(mframe, 1);
    mpi_->encode_put_frame(ctx_, mframe);
    mpp_frame_deinit(&mframe);

    MppPacket packet = nullptr;
    while (mpi_->encode_get_packet(ctx_, &packet) == MPP_OK && packet != nullptr) {
        void* data = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);
        int64_t pts = mpp_packet_get_pts(packet);
        if (data != nullptr && len > 0) {
            EmitAnnexBPacket(static_cast<const uint8_t*>(data), len, pts, on_packet);
        }
        mpp_packet_deinit(&packet);
    }
}

// ---------------------------------------------------------------------------
// EmitAnnexBPacket：Annex-B 转 AVCC 并包装成 AVPacket 回调
// ---------------------------------------------------------------------------
void MppEncoder::EmitAnnexBPacket(const uint8_t* data, size_t size, int64_t pts,
                                  const std::function<void(const PacketPtr&)>& on_packet) {
    PacketPtr av_pkt = CreatePacket();
    av_pkt->pts = pts;
    av_pkt->dts = pts;

    // Annex-B 转 AVCC：起始码替换为 4 字节长度前缀。
    std::vector<uint8_t> avcc;
    avcc.reserve(size);
    ParseAnnexB(data, size, [&](const uint8_t* nal, size_t len) {
        if (len == 0) {
            return;
        }
        avcc.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        avcc.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        avcc.push_back(static_cast<uint8_t>(len & 0xFF));
        avcc.insert(avcc.end(), nal, nal + len);
    });

    if (avcc.empty()) {
        return;
    }
    av_pkt->data = static_cast<uint8_t*>(av_malloc(avcc.size()));
    std::memcpy(av_pkt->data, avcc.data(), avcc.size());
    av_pkt->size = static_cast<int>(avcc.size());
    on_packet(av_pkt);
}

// ---------------------------------------------------------------------------
// Close：释放 MPP 资源
// ---------------------------------------------------------------------------
void MppEncoder::Close() {
    if (ctx_ != nullptr && mpi_ != nullptr) {
        mpi_->reset(ctx_);
    }
    if (buf_group_ != nullptr) {
        mpp_buffer_group_put(buf_group_);
        buf_group_ = nullptr;
    }
    if (ctx_ != nullptr) {
        mpp_destroy(ctx_);
        ctx_ = nullptr;
    }
    mpi_ = nullptr;
    opened_ = false;
    extradata_.clear();
}

const uint8_t* MppEncoder::GetExtradata() const {
    return extradata_.empty() ? nullptr : extradata_.data();
}

int MppEncoder::GetExtradataSize() const {
    return static_cast<int>(extradata_.size());
}

#else  // !__aarch64__：x86 编译检查用空实现

MppEncoder::~MppEncoder() = default;

bool MppEncoder::Open(const EncodeConfig&, uint32_t, uint32_t, uint32_t) {
    Logger::instance().warn("mpp: hardware encoder unavailable on non-aarch64 build");
    return false;
}

bool MppEncoder::Encode(const FramePtr&, const std::function<void(const PacketPtr&)>&) {
    return false;
}

void MppEncoder::Flush(const std::function<void(const PacketPtr&)>&) {}

void MppEncoder::Close() {}

const uint8_t* MppEncoder::GetExtradata() const { return nullptr; }

int MppEncoder::GetExtradataSize() const { return 0; }

#endif  // __aarch64__

} // namespace vision
