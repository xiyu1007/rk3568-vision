# CLAUDE.md

本文件为 Claude Code 在本仓库工作时提供指引。

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
- **RTMP 服务器**：板端 mediamtx（项目 `tmp/` 下运行，`cd tmp && ./mediamtx`，监听 1935），非 nginx-rtmp
- 测试顺序：先 mp4 输入（`source=mp4`）验证全链路，再切真实摄像头

## 注意事项

- 板端 FFmpeg 无 h264_rkmpp，当前用 libx264 软编；硬件编码需 Rockchip FFmpeg 分支
- 板端 IMX415 传感器已检测到，`/dev/video0` 1280x720@25fps 采集正常
- mediamtx v1.9.3 不转发「纯视频、无音频」的 RTMP 流（拉流端 0 帧），须在 FLV 补一路
  静音 AAC 轨；encoder.cpp 用 ffmpeg CLI 预生成的静音帧硬编码实现（板端原生 AAC 编码器
  版本错配会段错误，勿改回 avcodec 编码方式）
- 静音 AAC 轨的时间戳必须跟随视频时间戳对齐（writeSilentAudio 按视频 pts 换算采样数写足帧），
  否则音频时间戳滞后，mediamtx 转发时只发 FLV 头、不转发视频帧（同样表现为拉流 0 帧）
- Makefile 规则 `build/%.o: src/%.cpp` 不跟踪头文件依赖，改 `include/*.hpp` 后必须
  `make clean` 全量重编，否则新旧 .o 布局不一致会导致段错误
- 前处理已用 RGA 硬件加速（`inferencer.cpp` 用 im2d 的 `imresize` 做 NV12→RGB 转换+缩放），
  从 ~50ms 降到 ~3ms；当前推理链路 pre≈3ms + rknn≈45ms + post≈20ms ≈ 68ms（约 15fps）
- 模型可选：`model/yolov5s_relu.rknn`（relu 激活，NPU 更快，默认）或 `model/yolov5s.rknn`（标准 silu）。
  两者后处理差异在 sigmoid：relu 版输出已是 sigmoid 后的值、标准版是 logits。由配置
  `inference.use_sigmoid` 决定（relu=`false`、标准=`true`），`inferencer.cpp` 按此切换，
  换模型时务必同步改该配置，否则阈值失效导致候选暴增、后处理慢到 2 秒/帧
- `librknnrt.so` 已升级到 **2.3.2**（系统 `/lib` 和 `third_lib/` 都是；旧 1.5.0 备份在
  `/lib/librknnrt.so.bak`）。板端 NPU 内核驱动是 rknpu 0.8.2（2022 旧版），只兼容 rknn-toolkit2
  1.4/1.5 转换的模型，故 2.x 转换的模型必须配 2.x 运行时才能加载（否则 rknn_init 报 -6）
- 转模型用 rknn-toolkit2 2.3.2（已装在 ubuntu 虚拟机，Python 3.12 + onnx 1.16.1），脚本见
  rknn_model_zoo `examples/yolov5`；目标平台 `rk3568`、量化 `i8`（量化集 coco_subset_20）
