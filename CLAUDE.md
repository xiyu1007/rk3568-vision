# CLAUDE.md

本文件为 Claude Code 在本仓库工作时提供指引。

## 构建与运行

```bash
./scripts/setup_env.sh   # 一键部署环境（全新板端：装 FFmpeg 开发库 + 配 librknnrt + 部署 mediamtx + 编译）
./scripts/fetch_deps.sh  # 拉取三方依赖到 third_lib（rknn_api.h + librknnrt 2.3.2 + mediamtx）
./scripts/start.sh       # 一键启动（mediamtx + rk3568_vision，参数透传，不做自动重启）
make            # release：-O2 优化，生成 output/rk3568_vision
make debug      # debug：-DVISION_DEBUG -g -O0，性能分析/队列深度/详细日志
make clean      # 清理 build/ 与 output/
```

命令行参数：`-c 配置 -d 设备 -W/-H/-f 宽高帧率 -s RTMP地址 --no-stream --no-inference --record 路径 -v`

常见运行方式：

- 摄像头检测推流：`./scripts/start.sh -c conf/default.yaml -d /dev/video0`
- 摄像头纯推流（不推理）：`./scripts/start.sh -c conf/camera_push.yaml -d /dev/video0`
- mp4 联调推流：`./scripts/start.sh -c conf/test_mp4.yaml`

三方库统一放 `third_lib/`（不入库，fetch_deps.sh 拉取）：

- `third_lib/librknn_api/`：rknn_api.h + librknnrt.so（2.3.2，模型需 2.x 运行时；程序经 rpath 引用，不覆盖系统 /lib 的 2.1.0）
- `third_lib/mediamtx/`：mediamtx 推流服务器（单二进制 + 配置）

## 架构

纯 C++17（`namespace vision`），一条 V4L2 采集 → 稳帧 → RKNN 推理 → H.264 编码 →
RTMP 推流 / MP4 录制的多线程流水线。头文件在 `include/vision/`，源文件在 `src/`。
模块划分借鉴 around_view_app（分层 + 回调解耦），协调器只协调不承载具体逻辑。

| 模块           | 文件                                               | 职责                                          |
| -------------- | -------------------------------------------------- | --------------------------------------------- |
| 类型/配置/调试 | `types.hpp` `config.hpp` `debug.hpp`         | Frame（dmabuf 双来源）、配置、DEBUG 宏        |
| 采集           | `camera_source.*`                                | V4L2 dmabuf 零拷贝 / mp4 解码，回调解耦       |
| 推理           | `inferencer.*`                                   | RGA 前处理 + RKNN 推理 + YOLOv5 后处理 + 画框 |
| 编码           | `h264_encoder.*`                                 | H264 硬编/软编                                |
| 封装/推流/录制 | `muxer.*` `rtmp_streamer.*` `mp4_recorder.*` | FLV/MP4 封装、RTMP 推流（静音 AAC）、MP4 录制 |
| 协调器         | `pipeline.*`                                     | 组合模块、回调解耦、队列、线程编排、监控      |
| 基础设施       | `logger.*` `ring_buffer.hpp`                   | 日志、有界环形缓冲                            |

- **回调解耦**：`CameraSource::RegisterFrameCallback` 注册回调，采集线程回调里只入队，
  不做重活（推理/编码在协调器里）
- **零拷贝**：V4L2 mmap + EXPBUF 导出 dmabuf fd，采集不 memcpy，Frame 引用 mmap 地址，
  shared_ptr 自定义 deleter 引用归零时 QBUF 归还 buffer
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
- **RTMP 服务器**：板端 mediamtx（`third_lib/mediamtx/` 下，`cd third_lib/mediamtx && ./mediamtx`，监听 1935），非 nginx-rtmp；可用 `./scripts/start.sh` 一键启动
- 测试顺序：先 mp4 输入（`source=mp4`）验证全链路，再切真实摄像头

## 注意事项

- 板端 FFmpeg 无 h264_rkmpp，当前用 libx264 软编；硬件编码需 Rockchip FFmpeg 分支
- 板端 IMX415 传感器已检测到，`/dev/video0` 1280x720@25fps 采集正常
- 摄像头画面发绿/发暗是**板端系统问题、非本项目代码**：系统升级到 ubuntu22.04 + kernel-6.1 后，
  `camera_engine_rkaiq`（6.8.0）**删掉了 IMX415 的 IQ 标定文件**（旧 ubuntu20.04/debian11 的 5.0x4.1
  包里还有，但那是 AIQ 5.x 格式，喂给 AIQ 6.x 的 `rkaiq_3A_server` 会段错误），导致 `rkaiq_3A_server`
  初始化失败（"can't find sensor"），ISP 无自动白平衡/自动曝光/自动增益，raw 输出偏绿偏暗（analog gain=0）。
  **修复**：从 `debian12/packages/arm64/rkaiq/camera_engine_rkaiq_rk3568_arm64.deb`（AIQ 6.9.0）取
  `imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json`（631KB，AIQ 6.x 格式）放到 `/etc/iqfiles/`，
  并把 rkaiq 升到 6.9.0（`dpkg -i` 该 deb），最后 `systemctl start rkaiq_3A`（须 root，普通用户跑会段错误）。
  本项目 pipeline 只是忠实编码摄像头 NV12，不参与调色。
- RTMP 拉流的「数秒延迟」主要来自**拉流端缓冲**而非本 pipeline：pipeline 稳态延迟约 60~100ms
  （稳帧器 40ms + 软编 ~15ms + 封装 ~5ms），ffprobe 实测流 pts 随实时推进、`frames=push` 无积压。
  VLC 默认 RTMP 缓冲 ~1s+，用 `vlc --network-caching=200 rtmp://...` 或在「输入/编解码器→网络缓存」
  里调到 100~200ms 即可降到亚秒级；ffmpeg 用 `-fflags nobuffer -flags low_delay -analyzeduration 0 -probesize 32`。
- mediamtx v1.9.3 不转发「纯视频、无音频」的 RTMP 流（拉流端 0 帧），须在 FLV 补一路
  静音 AAC 轨；`muxer.cpp` 用 ffmpeg CLI 预生成的静音帧硬编码实现（板端原生 AAC 编码器
  版本错配会段错误，勿改回 avcodec 编码方式）
- 静音 AAC 轨的时间戳必须与视频对齐，且要**换算到 FLV 封装的 time_base（毫秒）**：FLV muxer
  会把音/视频流的 time_base 强制为 1/1000，`WriteSilentAudio` 若把 44100Hz 采样数直接当 pts 写入
  （不 `av_packet_rescale_ts` 换算），音频时间戳会比视频大 ~44 倍，拉流端（VLC/ffmpeg）因 A/V
  严重错位而长时间缓冲、画面停留数秒前。已按 1/44100→流 time_base 换算修复。
- `RtmpStreamer::Push` 首次连接不应 sleep `reconnect_delay_ms`（仅断线重连才等），否则启动会
  白睡 2s、期间编码包堆积丢旧，流起始时间戳跳变、拉流端缓冲。
- Makefile 规则 `build/%.o: src/%.cpp` 不跟踪头文件依赖，改 `include/vision/*.hpp` 后必须
  `make clean` 全量重编，否则新旧 .o 布局不一致会导致段错误
- 前处理已用 RGA 硬件加速（`inferencer.cpp` 用 im2d 的 `imresize` 做 NV12→RGB 转换+缩放），
  从 ~50ms 降到 ~3ms；注意 `wrapbuffer_virtualaddr` 参数顺序是 va,width,height,format,wstride,hstride
- 模型可选：`model/yolov5n.rknn`（n 尺寸，默认）/ `model/yolov5s.rknn`（标准 silu）/ `model/yolov5s_relu.rknn`（relu 激活）。
  后处理差异在 sigmoid：relu/n 版输出已是 sigmoid 后的值、标准 yolov5s 是 logits。由配置
  `inference.use_sigmoid` 决定（relu/n=`false`、标准=`true`），`inferencer.cpp` 按此切换，
  换模型时务必同步改该配置，否则阈值失效导致候选暴增、后处理慢到 2 秒/帧
- `librknnrt.so` 用 **2.3.2**（在 `third_lib/`，程序经 Makefile rpath 引用），**不替换系统 `/lib`**
  （系统保持默认 2.1.0）。模型用 rknn-toolkit2 2.3.2 转换，需 2.x 运行时（1.4/1.5 会 rknn_init 报 -6）
- 转模型用 rknn-toolkit2 2.3.2（已装在 ubuntu 虚拟机，Python 3.12 + onnx 1.16.1），脚本见
  rknn_model_zoo `examples/yolov5`；目标平台 `rk3568`、量化 `i8`（量化集 coco_subset_20）
