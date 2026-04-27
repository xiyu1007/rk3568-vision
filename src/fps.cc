#include "fps.h"

#include <cstdio>

// 静态全局变量
static double last_tick = 0;
static int frame_count = 0;
static double fps = 0;
static bool initialized = false;

// 刷新 FPS 计算（第一次调用自动初始化）
void fps_update(void) {
    double current_tick = cv::getTickCount();
    
    if (!initialized) {
        last_tick = current_tick;
        frame_count = 0;
        fps = 0;
        initialized = true;
        return;
    }
    
    frame_count++;
    
    double elapsed = (current_tick - last_tick) / cv::getTickFrequency();
    if (elapsed >= 0.5) {
        fps = frame_count / elapsed;
        last_tick = current_tick;
        frame_count = 0;
    }
}

// 获取当前 FPS 值
double fps_get(void) {
    return fps;
}

// 显示 FPS（仅文字）
void fps_show(cv::Mat img, cv::Point position, cv::Scalar color, double font_scale, int thickness) {
    if (img.empty()) return;
    
    char fps_text[32];
    snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", fps);
    
    cv::putText(img, fps_text, position,
                cv::FONT_HERSHEY_SIMPLEX, font_scale,
                color, thickness, cv::LINE_AA);
}

// 显示 FPS（带背景框）- 复用 fps_show
void fps_show_with_box(cv::Mat img, cv::Point position, cv::Scalar color, 
                               double font_scale, int thickness) {
    if (img.empty()) return;
    
    char fps_text[32];
    snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", fps);
    
    // 计算文字尺寸
    int baseline = 0;
    cv::Size text_size = cv::getTextSize(fps_text, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);
    
    // 绘制背景框（自动适应文字大小）
    cv::rectangle(img, 
                  cv::Point(position.x - 5, position.y - text_size.height - 5),
                  cv::Point(position.x + text_size.width + 5, position.y + 5),
                  cv::Scalar(0, 0, 0), -1);
    
    // 复用 fps_show 绘制文字
    fps_show(img, position, color, font_scale, thickness);
}