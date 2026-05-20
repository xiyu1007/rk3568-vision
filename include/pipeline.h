/*
 * ==========================================================================
 * pipeline.h — 流水线核心调度模块头文件
 * ==========================================================================
 *
 * **pipeline_t**：整个系统的中央调度器
 *   管理 V4L2 采集器、RKNN 推理器、FFmpeg 编码器、RTMP 推流器、OpenCV 显示器
 *   通过 "生产者-消费者" 模式协调四个工作线程
 *
 * **队列容量设计**：
 *   PIPELINE_QUEUE_CAP = 8：每个环形队列可缓存 8 帧
 *   PIPELINE_FRAME_POOL = 16：帧池最大帧数（预分配，减少运行时 malloc）
 *
 * **8 帧缓冲的意义**：
 *   在 30fps 下，8 帧 ≈ 266ms 缓冲
 *   允许下游线程短暂阻塞（如编码器忙）而不丢帧
 *   但不会造成过度延迟（266ms < 500ms 端到端目标）
 */

#ifndef PIPELINE_H
#define PIPELINE_H

#include "types.h"
#include "ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIPELINE_QUEUE_CAP  8   /* 每个环形队列的最大帧数（实际可用 = cap-1） */
#define PIPELINE_FRAME_POOL 16  /* 帧池大小（预分配 frame_t 的最大数量） */

typedef struct pipeline_s pipeline_t;

/*
 * 创建流水线
 * 根据配置初始化所有子模块（V4L2/RKNN/FFmpeg/RTMP/OpenCV）
 * 返回 pipeline_t*，失败返回 NULL
 */
pipeline_t* pipeline_create(const app_cfg_t* cfg);

/* 启动流水线（启动 V4L2 流 + 创建工作线程） */
int         pipeline_start(pipeline_t* p);

/* 停止并销毁流水线（等待线程结束 + 释放所有资源） */
void        pipeline_stop(pipeline_t* p);

/* 检查流水线是否正在运行 */
int         pipeline_running(pipeline_t* p);

#ifdef __cplusplus
}
#endif
#endif /* PIPELINE_H */
