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

| 指标          | 目标值                            |
| ------------- | --------------------------------- |
| 采集          | 720P@25fps（NV12）                |
| 单帧 NPU 推理 | ~25ms（YOLOv5s INT8）             |
| 端到端延迟    | < 100ms                           |
| 输出          | H.264 + RTMP 推流 / 本地 MP4 录制 |

### 1.3 技术栈

```
┌──────────────────────────────────────────────┐
│              应用层 (main.cpp)               │
├──────────────────────────────────────────────┤
│          流水线编排 (pipeline)                │
├──────────┬──────────┬──────────┬─────────────┤
│  V4L2    │  RKNN    │  FFmpeg  │  手写图像    │
│  采集    │  推理     │ 编码/封装 │  NV12 处理  │
├──────────┼──────────┼──────────┼─────────────┤
│  mmap    │  INT8    │  H.264   │  letterbox  │
│  零拷贝  │  量化     │ RTMP/MP4 │  画框       │
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
                  ┌──────────────────────────────────┐
                  │        Pipeline（核心调度）       │
                  │                                  │
                  │   采集 ──cap_q──▶ 稳帧器         │
                  │        ──inf_q──▶ 推理           │
                  │        ──enc_q──▶               │
                  │   编码 ─┬─push_q──▶ 推流(RTMP)   │
                  │         └─record_q─▶ 录制(MP4)   │
                  └──────────────────────────────────┘
```

### 2.2 模块组成

| 模块           | 文件                                               | 职责                                        |
| -------------- | -------------------------------------------------- | ------------------------------------------- |
| 类型/配置/调试 | `types.hpp` `config.hpp` `debug.hpp`         | Frame（dmabuf 双来源）、配置、DEBUG 宏      |
| 采集           | `camera_source.*`                                | V4L2 dmabuf 零拷贝 / mp4 解码，回调解耦     |
| 推理           | `inferencer.*`                                   | RGA 前处理 + RKNN 推理 + 后处理 + NV12 画框 |
| 编码           | `h264_encoder.*` `mpp_encoder.*`               | H.264 硬编(MPP)/软编(libx264)，硬编优先     |
| 封装/推流/录制 | `muxer.*` `rtmp_streamer.*` `mp4_recorder.*` | FLV/MP4、RTMP（静音AAC）、MP4 录制          |
| 协调器         | `pipeline.*`                                     | 组合模块、回调解耦、队列、线程编排、监控    |
| 基础设施       | `logger.*` `ring_buffer.hpp`                   | 日志、有界环形缓冲                          |

---

## 3. 设计模式

| 设计模式                | 应用位置                | 说明                                          |
| ----------------------- | ----------------------- | --------------------------------------------- |
| **生产者-消费者** | 各阶段之间              | 有界环形缓冲 + 条件变量解耦，满则丢最旧帧     |
| **策略模式**      | `Encoder` 硬/软编可选 | 硬编 MPP / 软编 libx264 运行时切换（硬编失败自动回退） |
| **单例模式**      | `Logger`              | 全局唯一，各线程直接访问                      |
| **RAII**          | 所有资源类              | V4L2/RKNN/FFmpeg 上下文析构自动释放           |
| **状态机**        | `Pipeline`            | `running_` 标志控制 Idle→Running→Stopping |

---

## 4. 线程模型与数据流

### 4.1 生产者-消费者流水线

```
采集线程 ──cap_q──▶ 稳帧线程 ──inf_q──▶ 推理线程 ──enc_q──▶ 编码线程
   (V4L2/mp4)     (稳帧器)            (RKNN)          (H.264)
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

| 机制                              | 用途                                  |
| --------------------------------- | ------------------------------------- |
| **有界环形缓冲 + 条件变量** | 阶段间帧传递，阻塞等待（非忙轮询）    |
| **丢最旧策略**              | 队列满时丢弃队首旧帧，避免延迟累积    |
| **std::atomic**             | `running_` 标志、性能统计（无锁）   |
| **信号处理**                | SIGINT/SIGTERM 优雅退出，SIGPIPE 忽略 |
| **close() 唤醒**            | 关闭队列唤醒阻塞消费者，实现线程退出  |

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

### 5.2 稳帧器（`pipeline.cpp` 的 `pacerLoop`）

位于**采集之后、推理之前**。以单调时钟为基准，每个周期(1/fps)输出一帧：

- **节拍**：输出帧间隔恒定，消除采集抖动
- **补帧**：某周期无新帧时复制上一帧（`allow_duplicate`），维持恒定帧率
- **丢帧**：采集快于目标帧率时，有界队列丢最旧自然丢弃
- **重新对齐**：落后超一周期时对齐当前时间，避免追帧雪崩

### 5.3 推理 `Inferencer`

真实 RKNN 推理（单一实现，无 mock），完整包含前处理 + 推理 + 后处理 + 画框：

- **前处理**：NV12 → RGB letterbox（等比缩放 + 灰边填充）
- **推理三步曲**：`rknn_inputs_set → rknn_run → rknn_outputs_get`
- **反量化**：`float = (int8 - zp) * scale`（本模型 scale≈0.09、zp≈43~69，非零零点）
- **后处理**：三个输出头解码 + NMS，letterbox 逆映射回原图坐标
- **画框**：`drawBoxesNv12` 直接在 NV12 上画检测框（供推流展示）

### 5.4 编码推流 `H264Encoder` + `Muxer`

- 编码：`hardware=true` → Rockchip **MPP 硬编**（`mpp_encoder.cpp`，失败自动回退）/
  `false` → libx264 软编（`h264_encoder.cpp`）；硬编零 B 帧、低 CPU 占用
- `Muxer`：`flv` → RTMP 推流 / `mp4` → 本地录制。FLV 推流自动补一路静音 AAC 音轨
  （mediamtx v1.9.3 不转发纯视频流，须含音轨才转发视频；静音帧为硬编码字节，非真实音频）

---

## 6. 配置文件

配置文件 `conf/default.yaml`（YAML 格式，自研精简解析器，零第三方依赖），所有字段均可用命令行覆盖。

```yaml
capture:
  source: v4l2            # v4l2=摄像头 / mp4=视频文件
  device: /dev/video0
  file: data/test.mp4
  width: 1280
  height: 720
  fps: 25
  buffer_count: 6
  use_multi_planar: true
inference:
  enabled: true
  model_path: model/yolov5n.rknn
  labels_path: model/coco_80_labels_list.txt
  confidence_threshold: 0.25
  nms_threshold: 0.45
  model_width: 640
  model_height: 640
  npu_core: 0
  use_sigmoid: false    # relu/n 模型输出已是 sigmoid 后值；标准 yolov5s 改为 true
pacer:
  target_fps: 25
  allow_duplicate: true
encode:
  hardware: true         # true=MPP 硬编（失败回退软编）/ false=libx264 软编
  bitrate: 2000000
  gop_size: 10
  preset: ultrafast
  profile: high
stream:
  enabled: true
  url: rtmp://127.0.0.1:1935/live/stream
record:
  enabled: false
  path: output/record.mp4
monitor_interval_ms: 5000
logging:
  level: info
  file: log/rk3568_vision.log
```

命令行参数：

| 参数                 | 说明           |
| -------------------- | -------------- |
| `-c/--config PATH` | 配置文件路径   |
| `-d/--device DEV`  | V4L2 设备      |
| `-W/-H/-f`         | 采集宽/高/帧率 |
| `-s/--stream URL`  | RTMP 推流地址  |
| `--no-stream`      | 不推流         |
| `--no-inference`   | 关闭检测       |
| `--record PATH`    | 启用 MP4 录制  |
| `-v/--verbose`     | debug 日志     |

---

## 7. 构建与部署

### 7.1 构建

```bash
make            # 板端(aarch64)：链接 librknnrt.so 生成可执行文件；x86：仅编译检查
make clean      # 清理
```

### 7.2 双平台（Makefile 按 `uname -m` 自动判断）

| 平台              | 行为                                                                           |
| ----------------- | ------------------------------------------------------------------------------ |
| aarch64（RK3568） | 链接`librknnrt.so`，生成 `output/rk3568_vision`（真实 NPU 推理 + libx264） |
| x86_64（开发机）  | 仅编译到`.o` 做语法检查（不链接/不运行，真实推理需板端 NPU）                 |

---

## 8. 运行

> 完整的分步测试流程（含启动 mediamtx、拉流验证、成功标准）见 **[TESTING.md](TESTING.md)**。
> 下面是两个最常用的启动命令速览。

### 8.1 RK3568 板端：真实摄像头 → RTMP 推流

```bash
cd /home/gx/project/gx/rk3568-vision
make
./output/rk3568_vision -c conf/default.yaml -d /dev/video0
```

### 8.2 板端：mp4 文件输入（无摄像头联调）

```bash
./output/rk3568_vision -c conf/test_mp4.yaml
```

### 8.3 RTMP 拉流验证

```bash
# 一键启动（mediamtx + app 一起，推荐）：./scripts/start.sh -c conf/default.yaml -d /dev/video0
# 或单独启动 mediamtx（单二进制 + 配置都在 third_lib/mediamtx/ 下）
cd third_lib/mediamtx && ./mediamtx &
# 拉流查看（板端流水线推流后；<IP> 用板端 IP，本机可用 127.0.0.1）
ffplay rtmp://127.0.0.1:1935/live/stream
# 检查流信息
ffprobe -v error -show_entries stream=codec_name,width,height \
  -of default=noprint_wrappers=1 rtmp://127.0.0.1:1935/live/stream
```

---

## 9. 性能与延迟分析

### 9.1 延迟分解（实测，板端摄像头 + yolov5n + RGA + 软编）

| 阶段               | 延迟             | 说明                                          |
| ------------------ | ---------------- | --------------------------------------------- |
| 采集（DQBUF）      | ~60-76ms         | V4L2 poll 等待（mmap 零拷贝）                 |
| 前处理（RGA）      | ~2-3ms           | RGA 硬件 NV12→RGB（原 CPU ~50ms）            |
| 推理（NPU）        | ~25-50ms         | yolov5n（INT8 量化）                          |
| 后处理（解码+NMS） | ~19-21ms         | CPU 解码 + NMS                                |
| 编码               | ~15-25ms         | libx264 软编 ultrafast                        |
| **实际帧率** | **~25fps** | 纯推流（不推理）达 25fps；推理链路由 NPU 决定 |

### 9.2 关键优化

- V4L2 mmap 零拷贝
- **RGA 硬件加速 NV12→RGB 前处理**（CPU 浮点 → RGA 2D 加速器，50ms→3ms）
- **yolov5n 模型**（n 尺寸，NPU 更快）+ INT8 量化
- 稳帧器保证输出码率稳定
- 零 B 帧编码降低延迟
- 有界队列 + 丢最旧，避免延迟累积
- 独立推流/录制线程隔离网络与磁盘阻塞

---

## 10. 目录结构

```
rk3568-vision/
├── Makefile                  # 构建（平台自动判断：aarch64 链接 rknnrt，x86 仅编译）
├── README.md                 # 设计文档
├── conf/
│   ├── default.yaml          # 默认配置（摄像头检测输入）
│   ├── test_mp4.yaml         # mp4 输入测试配置（无摄像头联调）
│   └── camera_push.yaml      # 摄像头纯推流配置（不推理，最低延迟）
├── include/vision/           # 头文件（namespace vision）
│   ├── types.hpp             # 核心类型（Frame dmabuf 双来源、DetectResult）
│   ├── config.hpp            # 配置结构体
│   ├── debug.hpp             # DEBUG 条件编译宏
│   ├── camera_source.hpp     # 采集（V4L2 零拷贝/mp4，回调解耦）
│   ├── inferencer.hpp        # 推理（RGA + RKNN + 后处理 + 画框）
│   ├── h264_encoder.hpp      # H264 软编（libx264）
│   ├── mpp_encoder.hpp       # H264 硬编（Rockchip MPP）
│   ├── muxer.hpp             # FLV/MP4 封装
│   ├── rtmp_streamer.hpp     # RTMP 推流（静音 AAC）
│   ├── mp4_recorder.hpp      # MP4 录制
│   ├── pipeline.hpp          # 协调器（组合模块、队列、线程）
│   ├── logger.hpp            # 日志
│   └── ring_buffer.hpp       # 环形缓冲
├── src/                      # 对应 .cpp + main.cpp + config.cpp
├── model/                    # yolov5n.rknn + yolov5s.rknn + yolov5s_relu.rknn + coco_80_labels_list.txt
├── third_lib/                # 三方依赖（不入库，fetch_deps.sh 拉取）
│   ├── librknn_api/          #   librknnrt.so(2.3.2) + rknn_api.h
│   └── mediamtx/             #   mediamtx 推流服务器（单二进制 + 配置）
└── scripts/                  # setup_env.sh / start.sh / fetch_deps.sh / verify_rtmp.sh
```

---

## 11. 依赖与拉取

| 依赖                                     | 用途      | 获取                                                                            |
| ---------------------------------------- | --------- | ------------------------------------------------------------------------------- |
| FFmpeg（libavcodec/format/util/swscale） | 软编/封装 | `apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev` |
| g++ (C++17) + make + pkg-config          | 构建      | `apt-get install build-essential pkg-config`                                  |
| RGA（librga）                            | 前处理    | `apt-get install librga-dev`（RK3568 镜像通常自带）                             |
| MPP（librockchip-mpp）                   | 硬编      | `apt-get install librockchip-mpp-dev`（RK3568 镜像通常自带）                    |
| RKNN 运行时（librknnrt.so + rknn_api.h） | NPU 推理  | `./scripts/fetch_deps.sh`（拉取到 third_lib/librknn_api/）                    |
| mediamtx                                 | RTMP 推流 | `./scripts/fetch_deps.sh`（拉取到 third_lib/mediamtx/）                       |

> RTMP 服务器用 **mediamtx**（单二进制），**不依赖 nginx / nginx-rtmp-module**。仓库里
> `package/` 下的 nginx 源码和 `conf/nginx-rtmp.conf` 是旧方案残留（已被 .gitignore 忽略），
> 无需部署。

> 一键部署（全新板端）：`./scripts/setup_env.sh`；一键启动：`./scripts/start.sh`。

> 配置解析用自研 YAML 解析器，无额外依赖。

---

## 12. 测试状态

| 项                                          | 状态      | 说明                                             |
| ------------------------------------------- | --------- | ------------------------------------------------ |
| mp4 输入 + 真实 NPU 推理 + 软编 + RTMP 推流 | ✅ 已验证 | 板端推流到 mediamtx，拉流端收到 h264 1280x720 帧 |
| 真实摄像头采集 + 推理 + 编码 + RTMP 推流    | ✅ 已验证 | /dev/video0 1280x720@25fps，0 丢帧，退出干净     |
| x86 编译检查                                | ✅ 已验证 | make 编译通过                                    |
| 硬件编码（Rockchip MPP）                    | ✅ 已验证 | `mpp_encoder.cpp`，enc≈4ms、CPU≈5%（软编 15~25ms/35%+） |

> 已修复一个关键 bug：NV12→RGB 前处理的 UV 双重偏移（`uv_row` 已定位到行，`uv_off`
> 又重复加了行偏移），导致越界读、偶发段错误，并污染模型输入的色度（正是「框大小
> 不匹配 / 一直是 person」的诱因之一）。

---

**文档版本**：v4.3.0（摄像头全链路验证通过 + mediamtx 推流 + 静音 AAC 音轨）
