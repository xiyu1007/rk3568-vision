/*
 * ==========================================================================
 * display.cpp — OpenCV 本地显示模块
 * ==========================================================================
 *
 * **功能**：使用 OpenCV 的 highgui 模块在本地显示器上显示视频画面
 *
 * **设计说明**：
 *   这个类提供了基本的 OpenCV 窗口管理功能
 *   但在 bridge.cpp 中，bridge_display_show 函数直接实现了完整的显示逻辑
 *   （包括 FPS 叠加和检测框绘制），而不使用本类的 show_frame 方法
 *
 *   所以本类在项目中更接近"窗口名管理器"，实际的帧显示逻辑在 bridge.cpp 中
 *
 * **窗口配置**：
 *   - cv::WINDOW_NORMAL：可调整大小的窗口（适合不同分辨率）
 *   - 默认窗口尺寸 960×540（16:9 比例）
 */

#include "display.hpp"

extern "C" {
#include "logger.h"
}

#include <opencv2/highgui.hpp>    /* cv::namedWindow, cv::imshow, cv::waitKey, cv::destroyWindow */
#include <opencv2/imgproc.hpp>    /* cv::resizeWindow */

namespace rk3568_vision {

/*
 * 构造显示窗口
 *
 * @name：窗口标题（如 "RK3568 Vision"）
 *
 * WINDOW_NORMAL 模式允许用户自由调整窗口大小
 * 初始尺寸 960×540，适合大多数显示器
 */
Display::Display(const std::string& name) : window_name_(name) {
    cv::namedWindow(window_name_, cv::WINDOW_NORMAL);  /* 可调整大小 */
    cv::resizeWindow(window_name_, 960, 540);           /* 默认尺寸（16:9） */
    open_ = true;
    LOG_INFO("display created: %s", name.c_str());
}

/* 析构：自动关闭窗口 */
Display::~Display() { close(); }

/*
 * 显示一帧图像
 *
 * 注意：当前项目中 bridge.cpp 的 bridge_display_show 直接实现了显示逻辑
 * 本方法保留作为备选/简单场景使用
 *
 * cv::waitKey(1)：处理 OpenCV 窗口事件并等 1ms
 *   必须调用此函数窗口才会刷新
 */
void Display::show_frame(const cv::Mat& img) {
    if (!open_ || img.empty()) return;
    cv::imshow(window_name_, img);
    cv::waitKey(1);
}

/* 关闭并销毁窗口 */
void Display::close() {
    if (!open_) return;
    open_ = false;
    cv::destroyWindow(window_name_);
}

} // namespace rk3568_vision
