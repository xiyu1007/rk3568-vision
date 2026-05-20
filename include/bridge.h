/*
 * ==========================================================================
 * bridge.h — C/C++ 桥接接口头文件
 * ==========================================================================
 *
 * **Bridge Pattern**（桥接模式）
 *   C 语言编写的 pipeline.c 通过 bridge 函数调用 C++ 实现的 Detector 和 Display
 *   bridge 函数内部将 void* 转换为 C++ 类指针，调用对应的 C++ 方法
 *
 * **为什么需要 Bridge？**
 *   - pipeline.c 使用纯 C（轻量、可移植、无异常）
 *   - Detector 和 Display 使用 C++ + OpenCV（RAII、STL、智能指针）
 *   - bridge 提供 extern "C" 接口，打破语言边界
 *
 * **接口约定**：
 *   - 所有 bridge 函数使用 void* 传递 C++ 对象
 *   - 创建函数返回 new 出来的对象指针（C 侧看到 void*）
 *   - 销毁函数调用 delete 释放对象
 *   - extern "C" 确保 C 链接（无名称修饰）
 */

#ifndef BRIDGE_H
#define BRIDGE_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Detector Bridge（检测器桥接） ─────────────────────────────────────── */

/* 创建检测器实例 */
void* bridge_detector_create(void);

/* 销毁检测器实例 */
void  bridge_detector_destroy(void* d);

/*
 * 初始化检测器（加载模型 + 标签）
 * 返回 1 成功，0 失败
 */
int   bridge_detector_init(void* d, const char* model, const char* labels,
                            float conf, float nms, uint32_t npu_core);

/*
 * 执行目标检测
 * @d：检测器实例
 * @bgr：BGR 图像数据（裸指针，w×h×3）
 * @w, h：图像宽高
 * @result：输出参数，检测结果
 */
void  bridge_detector_detect(void* d, const uint8_t* bgr, int w, int h,
                              detect_result_t* result);

/* 获取模型输入尺寸 */
int   bridge_detector_input_w(void* d);
int   bridge_detector_input_h(void* d);

/* ── Display Bridge（显示桥接） ────────────────────────────────────────── */

/* 创建显示窗口 */
void* bridge_display_create(const char* name);

/* 销毁显示窗口 */
void  bridge_display_destroy(void* d);

/*
 * 显示一帧图像（含 FPS 叠加和检测框绘制）
 * @d：显示窗口实例
 * @bgr：BGR 图像数据
 * @w, h：图像宽高
 * @show_fps：是否显示 FPS
 * @fps：当前 FPS 值
 * @detections：检测结果（含检测框列表）
 */
void  bridge_display_show(void* d, const uint8_t* bgr, int w, int h,
                           int show_fps, double fps,
                           const detect_result_t* detections);

/* ── 工具函数 ─────────────────────────────────────────────────────────── */

/*
 * NV12 格式转 BGR 格式（使用 OpenCV cvtColor，SIMD 加速）
 * @nv12：NV12 原始数据
 * @stride：Y 平面行步长（可能 > width）
 * @w, h：图像宽高
 * @bgr_out：输出 BGR 缓冲区（调用者分配 w×h×3 字节）
 */
void  bridge_nv12_to_bgr(const uint8_t* nv12, int stride, int w, int h, uint8_t* bgr_out);

#ifdef __cplusplus
}
#endif
#endif /* BRIDGE_H */
