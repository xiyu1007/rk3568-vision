# CLAUDE.md

本文件为 Claude Code 在本仓库工作时提供指引。

## 构建与运行

```bash
./build.sh build              # 构建（自动检测平台）
./build.sh run                # 构建 + 运行（默认含推流）
./build.sh run --no-stream    # 运行但不推流
./build.sh fetch-deps         # 拉取第三方依赖（RKNN/JSON）
./build.sh test-rtmp          # 本地 RTMP 验证指引
```

手动构建：`mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)`

命令行参数：`-c 配置 -d 设备 -W/-H/-f 宽高帧率 -s RTMP地址 --no-stream --no-inference --record 路径 -v`

## 架构

纯 C++17（`namespace vision`）实现，一条 V4L2 采集 → 稳帧 → RKNN 推理 →
H.264 编码 → RTMP 推流 / MP4 录制的多线程流水线。

- **双平台**：aarch64 定义 `VISION_RK3568`（真实 RKNN + h264_rkmpp）；
  x86 定义 `VISION_X86`（mock 推理 + libx264）。
- **线程模型**：阶段间用有界环形缓冲 + 条件变量（`ring_buffer.hpp`），
  生产者-消费者解耦，满则丢最旧。
- **稳帧器**：`FramePacer` 位于采集之后、推理之前，按目标帧率节拍出帧。
- **后处理**：`YoloDecoder` 用 letterbox 逆映射（减 padding 除 scale），
  不是直接拉伸。
- **配置**：`conf/default.json`（nlohmann/json），命令行可覆盖。

## 依赖

FFmpeg（libavcodec/format/util/swscale）、CMake ≥3.16、g++(C++17)。
RKNN 运行时与 nlohmann/json 放在 `third_lib/`（不入库，`fetch_deps.sh` 拉取）。

## 注意事项

- 无硬件时在 WSL 上用 x86 mock 模式联调；WSL 需先装工具链：
  `sudo apt-get install -y build-essential cmake pkg-config libavcodec-dev libavformat-dev libavutil-dev libswscale-dev`
- 三个待验证项（摄像头模糊 / 框大小匹配 / rkmpp 推流）需在 RK3568 板上实测。
