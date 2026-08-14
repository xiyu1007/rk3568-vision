# rk3568-vision — RK3568 边缘端实时目标检测系统

> **硬件平台**：Rockchip RK3568（ARM Cortex-A55 ×4 + 1 TOPS NPU）
> **摄像头**：Sony IMX415（MIPI CSI）
> **技术栈**：Linux / C++17 / V4L2 / RKNN / YOLOv5 / FFmpeg / RTMP / 多线程
> **端到端延迟目标**：< 100ms（单帧 NPU 推理 ~25ms）

一套在 RK3568 上运行的实时视频采集 + AI 目标检测 + H.264 编码 + RTMP 推流的边缘视觉流水线。

---

## 目录

1. [项目概述](#1-项目概述)
2. [系统架构](#2-系统架构)
3. [设计模式](#3-设计模式)
4. [线程模型与数据流](#4-线程模型与数据流)
5. [模块详解](#5-模块详解)
6. [配置文件](#6-配置文件)
7. [构建与部署](#7-构建与部署)
8. [运行](#8-运行)
9. [性能与延迟分析](#9-性能与延迟分析)
10. [目录结构](#10-目录结构)
11. [依赖与拉取](#11-依赖与拉取)
12. [待验证事项](#12-待验证事项)

---

## 1. 项目概述

### 1.1 目标

在 RK3568 边缘平台构建实时目标检测系统，通过 V4L2 采集 IMX415 摄像头视频流，
利用 RKNN 部署 YOLOv5 模型进行 NPU 加速推理，结合 FFmpeg 实现 H.264 编码与
RTMP 推流，端到端延迟控制在 100ms 以内。

### 1.2 核心指标

| 指标 | 目标值 |
|------|--------|
| 采集 | 720P@25fps（NV12） |
| 单帧 NPU 推理 | ~25ms（YOLOv5s INT8） |
| 端到端延迟 | < 100ms |
| 输出 | H.264 + RTMP 推流 / 本地 MP4 录制 |

### 1.3 技术栈

```
┌──────────────────────────────────────────────┐
│              应用层 (main.cpp)                 │
├──────────────────────────────────────────────┤
│          流水线编排 (pipeline)                  │
├──────────┬──────────┬──────────┬─────────────┤
│  V4L2    │  RKNN    │  FFmpeg  │  手写图像     │
│  采集     │  推理     │ 编码/封装 │  NV12 处理    │
├──────────┼──────────┼──────────┼─────────────┤
│  mmap    │  INT8    │  H.264   │  letterbox   │
│  零拷贝   │  量化     │  RTMP/MP4│  画框        │
├──────────┴──────────┴──────────┴─────────────┤
│          Linux Kernel (RK3568 BSP)           │
│   V4L2 Driver │ NPU Driver │ MPP 硬编        │
└──────────────────────────────────────────────┘
```

---

## 2. 系统架构

### 2.1 总体架构图

```
                    ┌────────────────────────────┐
                    │          main.cpp          │
                    │  信号处理 → 配置加载 →      │
                    │  日志初始化 → 流水线生命周期 │
                    └─────────────┬──────────────┘
                                  ▼
                    ┌────────────────────────────┐
                    │        Pipeline（核心调度）  │
                    │                            │
                    │   采集 ──cap_q──▶ 稳帧器     │
                    │              ──inf_q──▶ 推理 │
                    │                  ──enc_q──▶  │
                    │   编码 ─┬─push_q──▶ 推流(RTMP)│
                    │         └─record_q─▶ 录制(MP4)│
                    └────────────────────────────┘
```

### 2.2 模块组成

| 模块 | 文件 | 职责 |
|------|------|------|
| 类型定义 | `types.hpp` | `Frame`/`DetectBox`/`DetectResult` 等核心数据结构 |
| 环形缓冲 | `ring_buffer.hpp` | 有界环形缓冲 + 条件变量（线程通信核心） |
| 配置管理 | `config.hpp/.cpp` | JSON 配置加载（nlohmann/json）+ 命令行覆盖 |
| 日志系统 | `logger.hpp/.cpp` | 分级异步日志 + 文件轮转（单例） |
| 视频采集 | `v4l2_capture.hpp/.cpp` | V4L2 mmap 零拷贝采集 NV12 |
| 稳帧器 | `frame_pacer.hpp/.cpp` | 帧率稳定节拍器（采集后、推理前） |
| 推理接口 | `inferencer.hpp` | 推理抽象（策略模式） |
| RKNN 推理 | `rknn_inferencer.hpp/.cpp` | 真实 NPU 推理（仅 aarch64） |
| 假推理 | `mock_inferencer.hpp/.cpp` | x86 联调用假检测 |
| 后处理 | `detector.hpp/.cpp` | YOLOv5 解码 + NMS |
| 图像工具 | `image_utils.hpp/.cpp` | NV12→RGB letterbox + NV12 画框 |
| 编码器 | `encoder.hpp/.cpp` | H.264 硬编/软编（FFmpeg） |
| 封装器 | `muxer.hpp/.cpp` | FLV(RTMP) / MP4 封装输出 |
| 系统监控 | `monitor.hpp/.cpp` | CPU/内存/温度 + 性能统计（单例） |
| 性能统计 | `perf.hpp` | 各阶段延迟原子统计 |
| 流水线 | `pipeline.hpp/.cpp` | 线程编排 + 状态机 + 优雅退出 |

---

## 3. 设计模式

| 设计模式 | 应用位置 | 说明 |
|----------|----------|------|
| **生产者-消费者** | 各阶段之间 | 有界环形缓冲 + 条件变量解耦，满则丢最旧帧 |
| **策略模式** | `Inferencer`、`Encoder` | 推理（rknn/mock）、编码（硬/软）可替换 |
| **单例模式** | `Logger`、`Monitor`、`Perf` | 全局唯一，各线程直接访问 |
| **RAII** | 所有资源类 | V4L2/RKNN/FFmpeg 上下文析构自动释放 |
| **状态机** | `Pipeline` | `running_` 标志控制 Idle→Running→Stopping |

---

## 4. 线程模型与数据流

### 4.1 生产者-消费者流水线

```
采集线程 ──cap_q──▶ 稳帧线程 ──inf_q──▶ 推理线程 ──enc_q──▶ 编码线程
   (V4L2)         (FramePacer)        (RKNN)          (H.264)
                                                        │
                                          ┌─────────────┴─────────────┐
                                       push_q                     record_q
                                          │                          │
                                          ▼                          ▼
                                      推流线程                     录制线程
                                      (RTMP)                      (MP4)
```

- **采集线程**：V4L2 `mmap` 零拷贝取帧 → 拷贝到 `Frame` → 入 `cap_q`
- **稳帧线程**：按目标帧率节拍输出，源帧抖动时补帧/丢帧 → 入 `inf_q`
- **推理线程**：NV12→RGB letterbox → NPU 推理 → 后处理(NMS) → 入 `enc_q`
- **编码线程**：NV12 画框 → H.264 编码 → 包分发到 `push_q`/`record_q`
- **推流线程**：封装 FLV → RTMP 网络写（含断线重连）
- **录制线程**：封装 MP4 → 本地文件写（配置开关）
- **监控线程**：周期采样 CPU/内存/温度并输出状态日志

### 4.2 线程同步机制

| 机制 | 用途 |
|------|------|
| **有界环形缓冲 + 条件变量** | 阶段间帧传递，阻塞等待（非忙轮询） |
| **丢最旧策略** | 队列满时丢弃队首旧帧，避免延迟累积 |
| **std::atomic** | `running_` 标志、性能统计（无锁） |
| **信号处理** | SIGINT/SIGTERM 优雅退出，SIGPIPE 忽略 |
| **close() 唤醒** | 关闭队列唤醒阻塞消费者，实现线程退出 |

### 4.3 数据流

```
IMX415 ──MIPI──▶ ISP ──NV12──▶ V4L2 DMA buffer
                                   │ mmap 映射
                                   ▼
                          采集线程 memcpy（唯一必要拷贝）
                                   │
                    Frame(nv12) ──cap_q──▶ 稳帧器 ──inf_q──▶ 推理
                                                              │
                                             NV12→RGB letterbox → NPU
                                                              │
                                            后处理解码 + NMS → detect 结果
                                                              │
                                   Frame(nv12+detect) ──enc_q──▶ 编码
                                                              │
                                        NV12 画框 → H.264 编码 → AVPacket
                                                              │
                                      ┌───────────────────────┴────────┐
                                      ▼                                 ▼
                               push_q ──▶ FLV 封装 ──▶ RTMP      record_q ──▶ MP4 封装 ──▶ 文件
```

---

## 5. 模块详解

### 5.1 视频采集 `V4l2Capture`

V4L2 标准采集流程：`open → QUERYCAP → S_FMT(NV12) → S_PARM(fps) → REQBUFS → mmap → QBUF → STREAMON`。

- **mmap 零拷贝**：内核 DMA buffer 映射到用户空间，采集不产生内核态拷贝
- **唯一必要拷贝**：DMA buffer 循环复用，`read()` 把 NV12 拷到 `Frame` 自有内存，供下游异步处理
- **poll 超时**：`read()` 用 poll 限时 500ms，使采集线程能周期检查退出标志

### 5.2 稳帧器 `FramePacer`

位于**采集之后、推理之前**。以单调时钟为基准，每个周期(1/fps)输出一帧：

- **节拍**：输出帧间隔恒定，消除采集抖动
- **补帧**：某周期无新帧时复制上一帧（`allow_duplicate`），维持恒定帧率
- **丢帧**：采集快于目标帧率时，有界队列丢最旧自然丢弃
- **重新对齐**：落后超一周期时对齐当前时间，避免追帧雪崩

### 5.3 推理 `Inferencer` + `RknnInferencer`

- `Inferencer` 抽象接口，`RknnInferencer`（aarch64 真实 NPU）/ `MockInferencer`（x86 假推理）两实现
- 真实推理三步曲：`rknn_inputs_set → rknn_run → rknn_outputs_get`
- INT8 输出在 CPU 侧反量化：`float = (int8 - zp) * scale`

### 5.4 后处理 `YoloDecoder`

- 三个输出头（stride 8/16/32），每个 `[1, 255, H, W]`（NCHW），255 = 3 anchor × 85
- 边界框解码：`bx=(σ(tx)*2-0.5+gx)*stride`，`bw=(σ(tw)*2)²*anchor_w`
- 置信度过滤 + 按类别 NMS
- **letterbox 逆映射**：`原图坐标 = (模型坐标 - padding) / scale`（保证框与原图对齐）

### 5.5 编码 `H264Encoder`

- `hardware=true` → `h264_rkmpp`（MPP 硬编，NV12 零拷贝直通）
- `hardware=false` → `libx264`（软编，NV12→YUV420P 经 swscale）
- **零 B 帧**（`max_b_frames=0`）+ `tune=zerolatency`，降低编码延迟

### 5.6 封装输出 `Muxer`

- `format="flv"` + `url=rtmp://...` → RTMP 推流
- `format="mp4"` + `url=文件路径` → 本地录制
- 标准 libavformat 流程：`alloc_context → new_stream → avio_open → write_header → write_frame → write_trailer`

---

## 6. 配置文件

配置文件 `conf/default.json`（JSON 格式），所有字段均可用命令行覆盖。

```jsonc
{
  "capture":   { "device": "/dev/video0", "width": 1280, "height": 720, "fps": 25, "pixel_format": "NV12" },
  "inference": { "enabled": true, "backend": "auto", "model_path": "model/yolov5s.rknn",
                 "conf_threshold": 0.25, "nms_threshold": 0.45 },
  "pacer":     { "enabled": true, "target_fps": 25, "allow_duplicate": true },
  "encode":    { "enabled": true, "hardware": true, "bitrate": 4000000, "gop_size": 50 },
  "stream":    { "enabled": true, "url": "rtmp://127.0.0.1/live/stream", "reconnect": true },
  "record":    { "enabled": false, "path": "output/record.mp4" },
  "monitor":   { "enabled": true, "log_interval_ms": 5000 },
  "logging":   { "level": "info", "file": "log/rk3568_vision.log", "async": true }
}
```

命令行参数：

| 参数 | 说明 |
|------|------|
| `-c/--config PATH` | 配置文件路径 |
| `-d/--device DEV` | V4L2 设备 |
| `-W/-H/-f` | 采集宽/高/帧率 |
| `-s/--stream URL` | RTMP 推流地址 |
| `--no-stream` | 不推流 |
| `--no-inference` | 关闭检测 |
| `--record PATH` | 启用 MP4 录制 |
| `-v/--verbose` | debug 日志 |

---

## 7. 构建与部署

### 7.1 一键脚本

```bash
./build.sh build              # 构建（自动检测平台）
./build.sh run                # 构建 + 运行（默认含推流）
./build.sh run --no-stream    # 运行但不推流
./build.sh clean              # 清理
./build.sh fetch-deps         # 拉取第三方依赖
./build.sh sync <rk_host>     # 同步到 RK 板
./build.sh test-rtmp          # 本地 RTMP 验证指引
```

### 7.2 手动构建

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 7.3 双平台

| 平台 | 宏 | 推理 | 编码 |
|------|-----|------|------|
| aarch64（RK3568） | `VISION_RK3568` | 真实 RKNN | h264_rkmpp 硬编 |
| x86_64（WSL） | `VISION_X86` | mock 假推理 | libx264 软编 |

---

## 8. 运行

### 8.1 x86 开发机（无硬件，mock 模式）

无摄像头时可用任意 V4L2 设备或 `--no-inference` 跑通编码链路；推理层为 mock。

```bash
# 构建 + 运行（mock 推理 + libx264 + 本地 RTMP）
./build.sh run --no-stream          # 不推流，仅本地编码/录制
./build.sh run -s rtmp://127.0.0.1/live/stream   # 推流到本地 nginx-rtmp
```

### 8.2 RK3568 板上（真实硬件）

```bash
# 交叉编译或板上原生编译后，直接运行
./rk3568_vision -c conf/default.json -d /dev/video0 -s rtmp://<server>/live/stream
```

### 8.3 RTMP 拉流验证

```bash
# 启动本地 nginx-rtmp
sudo nginx -c $(pwd)/scripts/nginx-rtmp.conf -p /tmp/nginx-rtmp
# 拉流查看
ffplay rtmp://127.0.0.1/live/stream
# 检查流信息
./scripts/verify_rtmp.sh rtmp://127.0.0.1/live/stream
```

---

## 9. 性能与延迟分析

### 9.1 延迟分解（估算）

| 阶段 | 延迟 | 说明 |
|------|------|------|
| 采集（DQBUF+拷贝） | ~1ms | mmap 零拷贝 + 一次必要拷贝 |
| 稳帧缓冲 | 0~40ms | 1 帧 @25fps 缓冲 |
| 推理（NPU） | ~25ms | 主要瓶颈 |
| 编码 | ~5ms | 软编；硬编 <2ms |
| 推流封装 | ~1ms | FLV + TCP |
| **端到端** | **~40~70ms** | < 100ms 目标 ✅ |

### 9.2 关键优化

- V4L2 mmap 零拷贝
- 稳帧器保证输出码率稳定
- NPU 硬件加速 + INT8 量化
- 零 B 帧编码降低延迟
- 有界队列 + 丢最旧，避免延迟累积
- 独立推流/录制线程隔离网络与磁盘阻塞

---

## 10. 目录结构

```
rk3568-vision/
├── CMakeLists.txt            # 构建配置（平台自动检测）
├── build.sh                  # 一键构建/运行/部署脚本
├── README.md                 # 本文档
├── conf/
│   └── default.json          # 默认配置
├── include/                  # 头文件（namespace vision）
│   ├── types.hpp             # 核心类型
│   ├── ring_buffer.hpp       # 环形缓冲
│   ├── config.hpp            # 配置
│   ├── logger.hpp            # 日志
│   ├── v4l2_capture.hpp      # 采集
│   ├── frame_pacer.hpp       # 稳帧器
│   ├── inferencer.hpp        # 推理接口
│   ├── rknn_inferencer.hpp   # 真实推理
│   ├── mock_inferencer.hpp   # 假推理
│   ├── detector.hpp          # 后处理
│   ├── image_utils.hpp       # 图像工具
│   ├── encoder.hpp           # 编码
│   ├── muxer.hpp             # 封装输出
│   ├── encoded_packet.hpp    # 编码包类型
│   ├── monitor.hpp           # 监控
│   ├── perf.hpp              # 性能统计
│   └── pipeline.hpp          # 流水线编排
├── src/                      # 源文件（对应 .cpp）
├── model/                    # 模型与标签
│   ├── yolov5s.rknn
│   └── coco_80_labels_list.txt
├── third_lib/                # 第三方依赖（不入库，fetch_deps.sh 拉取）
│   ├── rknn/                 #   librknnrt.so + rknn_api.h
│   └── json/                 #   nlohmann/json.hpp
└── scripts/                  # 辅助脚本
    ├── fetch_deps.sh
    ├── nginx-rtmp.conf
    └── verify_rtmp.sh
```

---

## 11. 依赖与拉取

| 依赖 | 用途 | 获取 |
|------|------|------|
| FFmpeg（libavcodec/format/util/swscale） | 编码/封装 | `apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev` |
| CMake ≥ 3.16 + g++ (C++17) | 构建 | `apt-get install build-essential cmake` |
| RKNN 运行时（librknnrt.so + rknn_api.h） | NPU 推理 | `./build.sh fetch-deps`（或手动放 third_lib/rknn/） |
| nlohmann/json.hpp | 配置解析 | `./build.sh fetch-deps` |

---

## 12. 待验证事项

> 以下项需在真实 RK3568 硬件上验证（当前无硬件环境）：

1. **摄像头画质**：IMX415 采集模糊问题（疑似驱动/焦距，非代码），板上用 `v4l2-ctl` 排查
2. **检测框匹配**：已按标准 letterbox 逆映射重写后处理，板上验证框与原图对齐
3. **RTMP 推流**：rkmpp 硬编路径的 SPS/PPS 与 FLV 封装需板上实测（x86 软编路径已可本地验证）

---

**文档版本**：v4.0.0（纯 C++ 重写）
