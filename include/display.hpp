/**
 * ==========================================================================
 * display.hpp — OpenCV 本地显示模块头文件
 * ==========================================================================
 *
 * **Display 类**：简单的 OpenCV 窗口管理器
 *   管理一个 namedWindow，提供 show_frame / close 方法
 *   实际绘制逻辑（FPS/检测框）在 bridge.cpp 的 bridge_display_show 中
 */

#pragma once

#include <opencv2/core.hpp>
#include <string>

namespace rk3568_vision {

class Display {
public:
    /* 创建显示窗口，默认标题 "RK3568 Vision" */
    explicit Display(const std::string& name = "RK3568 Vision");
    ~Display();

    /* 显示一帧图像（简单 imshow，无额外绘制） */
    void show_frame(const cv::Mat& img);

    /* 关闭窗口 */
    void close();

    /* 检查窗口是否打开 */
    bool is_open() const { return open_; }

private:
    std::string window_name_;   /* 窗口标题 */
    bool open_ = false;         /* 窗口打开状态 */
};

} // namespace rk3568_vision
