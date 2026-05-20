/*
 * ==========================================================================
 * bridge.cpp — C / C++ 桥接层（Bridge Pattern）
 * ==========================================================================
 *
 * **为什么需要 bridge？**
 *   项目核心（pipeline.c）使用纯 C 编写（轻量、无异常、易调试）
 *   检测器（detector.cpp）和显示（display.cpp）使用 C++ 和 OpenCV
 *   bridge 提供了一组 extern "C" 函数，将 C++ 对象包装为 C 可调用的 void* 接口
 *
 * **设计模式：Opaque Pointer（不透明指针）**
 *   C 代码通过 void* 持有 C++ 对象指针，不关心内部实现
 *   C++ 代码在 bridge 函数内部通过 static_cast 恢复类型
 *   这是 C/C++ 混合编程的标准模式
 *
 * **OpenCV 集成**：
 *   本项目使用 OpenCV（编译时已优化为 NEON SIMD）进行：
 *     - NV12→BGR 颜色空间转换（cvtColor, SIMD 加速）
 *     - 图像缩放（resize, INTER_LINEAR, 预处理步骤）
 *     - 本地显示和检测框绘制（rectangle, putText, imshow）
 */

#include "bridge.h"
#include "logger.h"

extern "C" {
#include "bridge.h"              /* 确保 C 链接一致性 */
}

#include <opencv2/core.hpp>       /* cv::Mat, cv::Scalar, cv::Point, cv::Rect */
#include <opencv2/imgproc.hpp>    /* cv::cvtColor, cv::resize, cv::rectangle, cv::putText */
#include <opencv2/highgui.hpp>    /* cv::namedWindow, cv::imshow, cv::waitKey, cv::destroyWindow */

/* ==========================================================================
 *  Detector Bridge（检测器桥接）
 * ========================================================================== */

#include "detector.hpp"

/*
 * 创建检测器实例
 * C 代码调用：void* det = bridge_detector_create();
 * 内部：new rk3568_vision::Detector() 创建 C++ 对象
 */
void* bridge_detector_create(void) {
    return new rk3568_vision::Detector();
}

/*
 * 销毁检测器实例
 * C 代码调用：bridge_detector_destroy(det);
 * 内部：delete C++ 对象
 */
void bridge_detector_destroy(void* d) {
    delete static_cast<rk3568_vision::Detector*>(d);
}

/*
 * 初始化检测器（加载模型 + 加载标签）
 * 返回 1 成功，0 失败
 */
int bridge_detector_init(void* d, const char* model, const char* labels,
                          float conf, float nms, uint32_t npu_core) {
    auto* det = static_cast<rk3568_vision::Detector*>(d);
    return det->init(model, labels, conf, nms, npu_core) ? 1 : 0;
}

/*
 * 执行目标检测
 *
 * 输入：bgr（BGR 格式裸数据, w×h 像素）
 * 输出：result（detect_result_t，检测到的目标列表）
 *
 * 内部流程：
 *   1. 从裸指针构造 cv::Mat（零拷贝包装，不复制数据）
 *   2. 调用 det->detect(img)（完整的预处理+推理+后处理管道）
 *   3. 将 C++ 的 DetectResult 转换为 C 的 detect_result_t
 *
 * 注意：构造 cv::Mat 时不拷贝数据（const_cast<uint8_t*>(bgr)），
 * 这要求 bgr 在 detect() 返回前保持有效（通常是 frame_t.bgr_data）
 */
void bridge_detector_detect(void* d, const uint8_t* bgr, int w, int h,
                             detect_result_t* result) {
    auto* det = static_cast<rk3568_vision::Detector*>(d);
    /* 零拷贝包装为 cv::Mat（不分配新内存，直接使用 bgr 指针） */
    cv::Mat img(h, w, CV_8UC3, const_cast<uint8_t*>(bgr));
    auto r = det->detect(img);

    /* 复制检测结果到 C 结构体 */
    result->count = r.count;

    /* 每 30 次检测输出一次日志（减少日志量） */
    static int log_cnt = 0;
    if (r.count > 0 && ++log_cnt % 30 == 0) {
        char names[256] = {0};
        int off = 0;
        for (uint32_t i = 0; i < r.count && i < 3; i++) {
            off += snprintf(names + off, sizeof(names) - off, "%s(%.0f%%) ",
                           r.boxes[i].label, r.boxes[i].conf * 100.0f);
        }
        LOG_INFO("detected: %s", names);
    }

    /* 逐框复制：x, y, w, h, class_id, conf, label */
    for (uint32_t i = 0; i < r.count && i < DETECT_MAX_BOXES; i++) {
        result->boxes[i].x        = r.boxes[i].x;
        result->boxes[i].y        = r.boxes[i].y;
        result->boxes[i].w        = r.boxes[i].w;
        result->boxes[i].h        = r.boxes[i].h;
        result->boxes[i].class_id = r.boxes[i].class_id;
        result->boxes[i].conf     = r.boxes[i].conf;
        strncpy(result->boxes[i].label, r.boxes[i].label, FRAME_LABEL_MAX - 1);
    }
}

/* 获取模型输入宽度/高度 */
int bridge_detector_input_w(void* d) {
    return (int)static_cast<rk3568_vision::Detector*>(d)->input_width();
}

int bridge_detector_input_h(void* d) {
    return (int)static_cast<rk3568_vision::Detector*>(d)->input_height();
}


/* ==========================================================================
 *  Display Bridge（显示桥接）
 * ========================================================================== */

/*
 * 创建显示窗口
 *
 * 使用 cv::namedWindow + WINDOW_NORMAL 模式（可调整大小）
 * 默认窗口大小 960×540（16:9）
 *
 * 为什么用 cv::String* 而非 cv::Mat？
 *   显示窗口只需窗口名，用 cv::String 简单存储
 *   cv::String 是 std::string 的 OpenCV 别名
 */
void* bridge_display_create(const char* name) {
    cv::namedWindow(name, cv::WINDOW_NORMAL);  /* WINDOW_NORMAL：可调整大小 */
    cv::resizeWindow(name, 960, 540);           /* 默认窗口尺寸（16:9）     */
    return new cv::String(name);                /* 存储窗口名以便后续使用    */
}

/* 销毁显示窗口 */
void bridge_display_destroy(void* d) {
    if (!d) return;
    cv::destroyWindow(static_cast<cv::String*>(d)->c_str());
    delete static_cast<cv::String*>(d);
}

/*
 * 显示一帧图像（含 FPS 叠加和检测框绘制）
 *
 * 绘制内容：
 *   1. 克隆原始 BGR 图像（不修改原始数据）
 *   2. 如果 show_fps && fps>0：在左上角绘制绿色 FPS 文字
 *   3. 遍历检测结果，绘制：
 *      - 绿色矩形框（cv::rectangle）
 *      - 类别标签 + 置信度（cv::putText，框中上方）
 *   4. cv::imshow 显示图像
 *   5. cv::waitKey(1) 处理 OpenCV 窗口事件并等待 1ms
 *
 * 注意：cv::waitKey(1) 是必需的，否则 OpenCV 窗口不会刷新
 *   1ms 是 OpenCV GUI 事件循环的最短处理时间
 *
 * @d：显示窗口指针（cv::String*）
 * @bgr：BGR 图像数据（裸指针，w×h×3 字节）
 * @w, h：图像宽高
 * @show_fps：是否显示 FPS
 * @fps：当前 FPS 值
 * @detections：检测结果（包含检测框列表）
 */
void bridge_display_show(void* d, const uint8_t* bgr, int w, int h,
                          int show_fps, double fps,
                          const detect_result_t* detections) {
    if (!d) return;

    /* 零拷贝包装 BGR 数据为 cv::Mat，然后 clone 一份用于绘制 */
    cv::Mat img(h, w, CV_8UC3, const_cast<uint8_t*>(bgr));
    cv::Mat disp = img.clone();   /* clone 独立副本，避免修改原始帧数据 */

    /* FPS 叠加：绿色文字，左上角 (15, 35) 位置 */
    if (show_fps && fps > 0) {
        char text[32];
        snprintf(text, sizeof(text), "FPS: %.1f", fps);
        cv::putText(disp, text, cv::Point(15, 35),
                    cv::FONT_HERSHEY_SIMPLEX, 0.8,       /* 字体大小 0.8 */
                    cv::Scalar(0, 255, 0), 2, cv::LINE_AA);  /* 绿色, 粗细2, 抗锯齿 */
    }

    /* 检测框绘制 */
    if (detections) {
        for (uint32_t i = 0; i < detections->count; i++) {
            auto& b = detections->boxes[i];

            /* 绿色矩形框（框宽 2 像素） */
            cv::rectangle(disp, cv::Rect(b.x, b.y, b.w, b.h),
                         cv::Scalar(0, 255, 0), 2);

            /* 标签文字（如 "person 85.3%"），在框上方 5 像素处 */
            char label[128];
            snprintf(label, sizeof(label), "%s %.1f%%",
                     b.label, b.conf * 100.0f);
            cv::putText(disp, label, cv::Point(b.x, b.y - 5),
                       cv::FONT_HERSHEY_SIMPLEX, 0.5,       /* 字体大小 0.5 */
                       cv::Scalar(0, 255, 0), 1);            /* 绿色, 粗细1 */
        }
    }

    /* 显示图像 */
    cv::imshow(static_cast<cv::String*>(d)->c_str(), disp);

    /*
     * cv::waitKey(1) — 关键帧调
     * 必须调用此函数 OpenCV 窗口才会刷新
     * 参数 1 = 等待 1ms 键盘输入（如果无按键则返回 -1）
     * 如果 waitKey 时间太长 → 显示帧率被限制
     * 如果 waitKey 时间太短 → CPU 空转浪费
     * 1ms 是最佳平衡点
     */
    cv::waitKey(1);
}

/*
 * NV12 格式转 BGR 格式（使用 OpenCV cvtColor）
 *
 * OpenCV 内部使用 SIMD（NEON）指令加速颜色空间转换
 * 在 ARM Cortex-A55 上，640×480 NV12→BGR 转换约 1-2ms
 *
 * NV12 cv::Mat 构造技巧：
 *   CV_8UC1：单通道 8-bit，尺寸为 (h + h/2) × w
 *   因为 NV12 的 Y 平面和 UV 平面在内存中连续排列
 *   stride 参数指定每行的字节数（可能 > width 因为有 V4L2 对齐要求）
 *
 * @nv12：NV12 数据指针
 * @stride：Y 平面每行字节数
 * @w, h：图像宽高
 * @bgr_out：输出 BGR 数据（调用者已分配 w×h×3 字节）
 */
void bridge_nv12_to_bgr(const uint8_t* nv12, int stride, int w, int h, uint8_t* bgr_out) {
    /* 构造 NV12 cv::Mat：高 h+h/2（Y+UV）, 宽 w（等于 stride 的实际像素宽） */
    cv::Mat nv12_mat(h + h/2, w, CV_8UC1, const_cast<uint8_t*>(nv12), stride);
    /* 构造输出 BGR cv::Mat */
    cv::Mat bgr_mat(h, w, CV_8UC3, bgr_out);
    /* OpenCV NEON 加速 NV12→BGR 转换 */
    cv::cvtColor(nv12_mat, bgr_mat, cv::COLOR_YUV2BGR_NV12);
}
