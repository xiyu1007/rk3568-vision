#ifndef __FPS_H__
#define __FPS_H__

#include <opencv2/opencv.hpp>

void fps_update(void);
double fps_get(void);

// 基础函数
void fps_show(cv::Mat img, cv::Point position, cv::Scalar color, double font_scale, int thickness);

// 重载版本 - 提供默认参数
inline void fps_show(cv::Mat img, cv::Point position = cv::Point(15, 35), cv::Scalar color = cv::Scalar(0, 255, 0)) {
    fps_show(img, position, color, 0.8, 2);
}

// 带背景框的基础函数
void fps_show_with_box(cv::Mat img, cv::Point position, cv::Scalar color, double font_scale, int thickness);

// 重载版本 - 提供默认参数
inline void fps_show_with_box(cv::Mat img, cv::Point position = cv::Point(15, 35), cv::Scalar color = cv::Scalar(0, 255, 0)) {
    fps_show_with_box(img, position, color, 0.8, 2);
}

#endif /* __FPS_H__ */