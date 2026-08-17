# AGENTS.md

本文件为 Codex 在本仓库工作时提供指引。

## 构建与运行

```bash
make            # 板端(aarch64)：链接 librknnrt.so 生成可执行文件；x86：仅编译检查
make clean      # 清理
./scripts/fetch_deps.sh   # 拉取 RKNN 依赖（rknn_api.h + librknnrt.so）
```

命令行参数：`-c 配置 -d 设备 -W/-H/-f 宽高帧率 -s RTMP地址 --no-stream --no-inference --record 路径 -v`

## 架构

纯 C++17（`namespace vision`），一条 V4L2 采集 → 稳帧 → RKNN 推理 → H.264 编码 →
RTMP 推流 / MP4 录制的多线程流水线。头文件在 `include/vision/`，源文件在 `src/`。

- **采集** `camera_source.cpp`：V4L2 dmabuf 零拷贝 / mp4 解码，回调解耦（回调里只入队）
- **推理** `inferencer.cpp`：RGA 前处理 + RKNN 推理 + YOLOv5 后处理 + NV12 画框
- **编码** `h264_encoder.cpp`（软编 libx264）/ `mpp_encoder.cpp`（硬编 Rockchip MPP），硬编优先
- **封装/推流/录制** `muxer.cpp` / `rtmp_streamer.cpp` / `mp4_recorder.cpp`
- **协调器** `pipeline.cpp`：组合模块、回调解耦、队列、线程编排、监控
- **线程通信**：有界环形缓冲 + 条件变量（`ring_buffer.hpp`），生产者-消费者，满则丢最旧
- **配置**：`conf/default.yaml`（自研 YAML 解析器，零第三方依赖），命令行可覆盖

## 构建（Makefile 按 `uname -m` 自动判断）

- **aarch64**：链接 `third_lib/librknn_api/aarch64/librknnrt.so`，生成可执行文件
- **x86_64**：仅编译到 `.o` 做语法检查（真实 RKNN 推理需板端 NPU，x86 不运行）

## 依赖

FFmpeg（libavcodec/format/util/swscale）、g++(C++17)、make、pkg-config。
三方库统一放 `third_lib/`（不入库，`fetch_deps.sh` 拉取）：`librknn_api/`（RKNN 2.3.2）
与 `mediamtx/`（RTMP 推流服务器）。

## 测试环境

- **rk3568 板端**：`ssh rk3568`，工作目录 `/home/gx/project/gx/rk3568-vision`
- **ubuntu 虚拟机**：`ssh ubuntu`，仅做 x86 编译检查
- **RTMP 服务器**：板端 mediamtx（`third_lib/mediamtx/`，监听 1935），`./scripts/start.sh` 一键启动
- 测试顺序：先 mp4 输入（`source=mp4`）验证全链路，再切真实摄像头

## 注意事项

- 硬编用板端 Rockchip MPP（`mpp_encoder.cpp`，链接 `-lrockchip_mpp`），软编用 libx264；`encode.hardware=true` 时硬编优先、失败自动回退软编
- 板端 IMX415 已检测到，`/dev/video0` 采集正常；前处理已用 RGA 硬件加速
- 改 `include/vision/*.hpp` 后必须 `make clean` 全量重编（Makefile 不跟踪头文件依赖）
- 模型可选 `model/yolov5n.rknn`（默认）/ `yolov5s.rknn` / `yolov5s_relu.rknn`，
  换模型须同步 `inference.use_sigmoid`（n/relu=`false`、标准 yolov5s=`true`）
