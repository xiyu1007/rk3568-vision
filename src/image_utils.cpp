// ============================================================================
// image_utils.cpp — 图像处理实现
// ============================================================================

#include "image_utils.hpp"

#include <algorithm>
#include <cstring>

namespace vision {

namespace {

// 把浮点值钳制到 [0, 255]（RGB 分量合法范围）。
inline uint8_t clamp8(float v) {
    if (v < 0.0f)   return 0;
    if (v > 255.0f) return 255;
    return static_cast<uint8_t>(v);
}

} // namespace

// ---------------------------------------------------------------------------
// NV12 → RGB + letterbox
// ---------------------------------------------------------------------------
void nv12ToRgbLetterbox(const uint8_t* nv12, int w, int h,
                        uint8_t* rgb, int dst_w, int dst_h,
                        LetterboxInfo& info) {
    // 1. 计算等比缩放比例与居中填充。
    float scale = std::min(static_cast<float>(dst_w) / w,
                           static_cast<float>(dst_h) / h);
    int scaled_w = static_cast<int>(w * scale);
    int scaled_h = static_cast<int>(h * scale);
    info.scale = scale;
    info.pad_x = (dst_w - scaled_w) / 2;
    info.pad_y = (dst_h - scaled_h) / 2;

    // 2. 整幅输出先用灰边填充（114 是 YOLOv5 官方 letterbox 填充色）。
    std::memset(rgb, 114, static_cast<size_t>(dst_w) * dst_h * 3);

    // 3. 分离 Y / UV 平面指针。
    const uint8_t* y_plane  = nv12;
    const uint8_t* uv_plane = nv12 + static_cast<size_t>(w) * h;

    // 4. 逐输出像素做最近邻采样 + YUV→RGB。
    for (int dy = 0; dy < scaled_h; ++dy) {
        int sy = static_cast<int>(dy / scale);          // 源行（最近邻）
        sy = std::min(sy, h - 1);
        const uint8_t* y_row  = y_plane  + static_cast<size_t>(sy) * w;
        const uint8_t* uv_row = uv_plane + static_cast<size_t>(sy / 2) * w;

        // 输出行首地址（跳过左侧 pad_x 个灰边像素）。
        uint8_t* dst_row = rgb + (static_cast<size_t>(dy + info.pad_y) * dst_w
                                  + info.pad_x) * 3;
        for (int dx = 0; dx < scaled_w; ++dx) {
            int sx = static_cast<int>(dx / scale);      // 源列（最近邻）
            sx = std::min(sx, w - 1);

            // 亮度 Y。
            int yv = y_row[sx];

            // 色度 U/V：NV12 的 UV 交错平面，每 2×2 像素共享一组 (U,V)。
            // 字节偏移 = (sy/2)*w + (sx&~1)，其中偶数位是 U，奇数位是 V。
            size_t uv_off = static_cast<size_t>(sy / 2) * w + (sx & ~1);
            int uv = uv_row[uv_off];       // U
            int vv = uv_row[uv_off + 1];   // V

            // BT.601 有限范围（16~235 / 16~240）YUV → RGB。
            float yf = (yv - 16) * 1.164f;
            float uf = uv - 128.0f;
            float vf = vv - 128.0f;
            dst_row[dx * 3 + 0] = clamp8(yf + 1.596f * vf);           // R
            dst_row[dx * 3 + 1] = clamp8(yf - 0.391f * uf - 0.813f * vf); // G
            dst_row[dx * 3 + 2] = clamp8(yf + 2.018f * uf);           // B
        }
    }
}

// ---------------------------------------------------------------------------
// 在 NV12 上绘制检测框
// ---------------------------------------------------------------------------
void drawBoxesNv12(uint8_t* nv12, int w, int h, const DetectResult& det) {
    uint8_t* y_plane = nv12;

    for (uint32_t i = 0; i < det.count; ++i) {
        const DetectBox& b = det.boxes[i];

        // 钳制到图像边界，避免越界访问。
        int x0 = std::max(0, std::min(b.x, w - 1));
        int y0 = std::max(0, std::min(b.y, h - 1));
        int x1 = std::max(0, std::min(b.x + b.w - 1, w - 1));
        int y1 = std::max(0, std::min(b.y + b.h - 1, h - 1));
        if (x1 < x0 || y1 < y0) continue;   // 非法框跳过

        // 画上下两条水平边（亮度置 255 = 白色）。
        for (int x = x0; x <= x1; ++x) {
            y_plane[static_cast<size_t>(y0) * w + x] = 255;
            y_plane[static_cast<size_t>(y1) * w + x] = 255;
        }
        // 画左右两条垂直边。
        for (int y = y0; y <= y1; ++y) {
            y_plane[static_cast<size_t>(y) * w + x0] = 255;
            y_plane[static_cast<size_t>(y) * w + x1] = 255;
        }
    }
}

} // namespace vision
