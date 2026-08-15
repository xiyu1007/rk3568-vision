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
RTMP 推流 / MP4 录制的多线程流水线。共 7 个模块（`common/ring_buffer/logger/capture/inferencer/encoder/pipeline`）。

- **采集** `capture.cpp`：`source=v4l2`(摄像头) / `source=mp4`(视频文件) 二选一
- **推理** `inferencer.cpp`：真实 RKNN + YOLOv5 前后处理 + NV12 画框（无 mock）
- **编码** `encoder.cpp`：H264 硬编(h264_rkmpp)/软编(libx264) + FLV/MP4 封装
- **流水线** `pipeline.cpp`：线程编排 + 稳帧器 + 监控 + 性能统计
- **线程通信**：有界环形缓冲 + 条件变量（`ring_buffer.hpp`），生产者-消费者，满则丢最旧
- **配置**：`conf/default.yaml`（自研 YAML 解析器，零第三方依赖），命令行可覆盖

## 构建（Makefile 按 `uname -m` 自动判断）

- **aarch64**：链接 `third_lib/librknn_api/aarch64/librknnrt.so`，生成可执行文件
- **x86_64**：仅编译到 `.o` 做语法检查（真实 RKNN 推理需板端 NPU，x86 不运行）

## 依赖

FFmpeg（libavcodec/format/util/swscale）、g++(C++17)、make、pkg-config。
RKNN 运行时放 `third_lib/librknn_api/`（不入库，`fetch_deps.sh` 拉取）。

## 测试环境

- **rk3568 板端**：`ssh rk3568`，工作目录 `/home/gx/project/gx/rk3568-vision`
- **ubuntu 虚拟机**：`ssh ubuntu`，仅做 x86 编译检查
- 测试顺序：先 mp4 输入（`source=mp4`）验证全链路，再切真实摄像头

## 注意事项

- 板端 FFmpeg 无 h264_rkmpp，当前用 libx264 软编；硬件编码需 Rockchip FFmpeg 分支
- 板端 IMX415 传感器目前 I2C 读回 0x000000（未检测到，排线/供电问题，非代码）
- 实测单帧推理链路约 97ms，瓶颈在前置处理 NV12→RGB 的 CPU 浮点运算（可后续用 RGA 加速）
