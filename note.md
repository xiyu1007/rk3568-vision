# RK3568 Vision Pipeline — 工业级实现笔记

## 项目概述

RK3568 嵌入式 Linux 视觉推理推流 Pipeline：
- V4L2 摄像头采集 → RKNN NPU 推理(YOLOv5) → FFmpeg H.264 编码 → RTMP 推流 + OpenCV 本地显示

目标平台：
- Ubuntu x86_64（开发/调试，RKNN stub）
- RK3568 aarch64（真机运行，NPU 推理）

---

## 最终工程结构

```
rk3568-vision/
├── include/             # 头文件（19个：2个.h + 17个.hpp）
│   ├── types.hpp        # 核心数据结构 FrameBuffer / DetectBox / ErrorCode
│   ├── config.hpp       # YAML 配置解析
│   ├── logger.hpp       # 异步日志系统（宏接口）
│   ├── sig_handler.h    # [C] 信号处理
│   ├── fps.h            # [C] FPS 统计
│   ├── v4l2_capture.hpp # V4L2 采集（RAII + epoll）
│   ├── ring_buffer.hpp  # SPSC 无锁环形队列
│   ├── pipeline.hpp     # 多线程管线编排
│   ├── rknn_context.hpp # RKNN 上下文（ARM/x86 双平台）
│   ├── detector.hpp     # YOLOv5 检测器
│   ├── ffmpeg_encoder.hpp # H.264 软件编码
│   ├── rtmp_pusher.hpp  # RTMP 推流
│   ├── display.hpp      # OpenCV 显示
│   ├── osd.hpp          # OSD 叠加（检测框/FPS）
│   └── ...              # buffer_pool / perf / monitor / preprocess / postprocess
├── src/                 # 源文件（18个：2个.c + 16个.cpp）
│   ├── main.cpp         # 入口，参数解析，Pipeline 启动/关闭
│   ├── sig_handler.c    # [C] 信号处理（SIGINT/SIGTERM）
│   ├── fps.c            # [C] FPS 统计（实时滑动窗口）
│   ├── v4l2_capture.cpp # V4L2 采集实现（epoll 等待）
│   ├── pipeline.cpp     # 管线主循环（capture→inference→encode+display）
│   ├── rknn_context.cpp # RKNN 实现（ARM 真实 NPU / x86 stub）
│   ├── detector.cpp     # YOLOv5 前处理 + 后处理 + NMS
│   ├── ffmpeg_encoder.cpp # libx264 软件编码
│   ├── rtmp_pusher.cpp  # RTMP 推流（自动重连）
│   ├── display.cpp      # OpenCV imshow
│   └── ...
├── config/
│   └── default.yaml     # 默认配置（摄像头/推理/编码/推流/日志）
├── model/
│   ├── yolov5s.rknn     # RKNN 量化模型
│   └── coco_80_labels_list.txt
├── third_lib/           # 第三方库（OpenCV / librknn_api / rga）
├── data/                # 测试数据
├── CMakeLists.txt       # CMake 构建（自动检测 x86/ARM）
├── run.bash             # 一键编译运行
└── note.md              # 本文件
```

---

## C/C++ 边界

### 纯 C 模块（2个）
- `sig_handler.c/h` — 仅使用 signal() / volatile sig_atomic_t，零 C++ 依赖
- `fps.c/h` — 仅使用 clock_gettime()，零 C++ 依赖

### C++ 模块（16个）
其余全部使用 C++17。保留 C++ 的原因为：
- V4L2 / RKNN：RAII 管理 fd / context 生命周期，析构自动清理
- RingBuffer：模板泛型，编译期类型安全
- Pipeline：std::thread + std::atomic 简化多线程管理
- Config/Logger：std::string/std::map/STL 容器减少手写内存管理
- OpenCV 集成：cv::Mat 是 C++ 对象，无法降级

---

## 数据流与内存模型

### Pipeline 数据流
```
IMX415 → ISP → DMA Buffer(内核)
  │  V4L2 DQBUF (epoll 等待)
  │  memcpy (1ms @1080P DDR) ─── FrameBuffer::data (自有存储)
  ▼
capture_queue (SPSC RingBuffer<shared_ptr<FrameBuffer>>, capacity=8)
  │ pop
  ▼
Inference Thread:
  NV12 → BGR (cvtColor, ~3ms)
  → RKNN NPU 推理 (~25ms)
  → NMS 后处理
  → FrameBuffer::bgr = bgr (缓存 BGR 供显示复用)
  ▼
infer_queue (SPSC RingBuffer<shared_ptr<FrameBuffer>>, capacity=8)
  │ pop (双消费者)
  ├─→ Encode Thread: NV12 → YUV420P (sws_scale) → libx264 → RTMP push
  └─→ Display Thread: FrameBuffer::bgr → 画框 + FPS → cv::imshow
```

### 零拷贝分析

1. **DMA buffer → FrameBuffer**: 从 mmap 做一次 memcpy（~1ms）。代价小，但消除了悬垂指针 bug。
2. **FrameBuffer → 多消费者**: shared_ptr 传递，引用计数，零额外拷贝。
3. **NV12 → BGR**: 在 Inference Thread 完成一次，结果存入 FrameBuffer::bgr，Display Thread 直接使用（零重复转换）。
4. **编码器输入**: NV12 → YUV420P 通过 sws_scale 直接在 libx264 AVFrame 内存上转换，无中间拷贝。

### 内存预估（1080P@30fps）
- V4L2 DMA buffers: 6 × (1920×1080×1.5) ≈ 18MB
- FrameBuffer pool (queues): 8 × 2 queues × 3MB ≈ 48MB
- libx264 lookahead: ~200MB
- RKNN 模型: ~14MB (INT8)
- **RSS 总计**: ~290MB（30 分钟稳定，无增长）

---

## 多线程模型

| 线程 | 角色 | 队列 | 等待机制 |
|------|------|------|----------|
| Main | 信号等待, 关闭协调 | - | sleep(1s) |
| Capture | V4L2 DQBUF → capture_queue | producer | epoll_wait(100ms) |
| Inference | pop → NPU → infer_queue | consumer → producer | RingBuffer::pop (非阻塞 + 1ms backoff) |
| Encode | pop → libx264 → RTMP | consumer | RingBuffer::pop + 5ms backoff |
| Display | pop → imshow | consumer | RingBuffer::pop + 1ms backoff |

**线程数**: 4 (Capture + Inference + Encode + Display)，总线程 5 (含 Main)。

### RingBuffer (SPSC 无锁)
- `include/ring_buffer.hpp`
- `alignas(64)` 隔离 head/tail 消除 false sharing
- `memory_order_acquire/release` 确保 ARM weak-memory 模型下正确性
- 队列满时丢弃（非阻塞 push），避免阻塞生产者

---

## epoll 实现

### V4L2 Capture
- `src/v4l2_capture.cpp:227-240` — start()：创建 epoll fd，注册 V4L2 设备 fd (EPOLLIN)
- `src/v4l2_capture.cpp:258-263` — capture()：epoll_wait(100ms 超时) 等待 DQBUF 就绪
- `src/v4l2_capture.cpp:245-257` — stop()：关闭 epoll fd

**效果**: 采集线程 CPU 从 busy-wait 5ms sleep 降为内核唤醒，CPU <1%。

---

## 平台适配

### 双平台支持
| 平台 | 编译宏 | RKNN | OpenCV | FFmpeg |
|------|--------|------|--------|--------|
| Ubuntu x86_64 | `X86_DEBUG` | stub (返回 false) | 系统 pkg-config | 系统 pkg-config |
| RK3568 aarch64 | `RK3568_NATIVE` | 真实 NPU (librknnrt.so) | 系统/third_lib | 系统 pkg-config |

### x86 RKNN Stub
- `include/rknn_context.hpp:3-15` — 定义 RKNN 类型 stub (struct 替代 rknn_api.h)
- `src/rknn_context.cpp:5-18` — 所有方法返回 false，LOG_WARN 提示

### CMake 自动检测
- `CMakeLists.txt:21-33` — `uname -m` 检测 aarch64 vs x86_64
- aarch64 → `RK3568_NATIVE`，链接真实 RKNN lib
- x86_64 → `X86_DEBUG`，RKNN 使用 stub

---

## 重构记录

### 删除的代码（理由）

| 删除项 | 理由 |
|--------|------|
| `src/main.c` (91行) | C 版本死代码，CMake 未编译 |
| `src/v4l2.c` (147行) | C 版本死代码 |
| `src/config.c` (79行) | C 版本死代码 |
| `src/log.c` (43行) | C 版本死代码 |
| `src/encoder.c` (80行) | C 版本死代码 |
| `src/ringbuf.c` (35行) | C 版本死代码 |
| `include/log.h` 等 5 个 C 头文件 | 对应死代码 |
| `test_log.c / test_ringbuf.c / test_v4l2.c` | 测试已删除的 C 代码 |
| `Makefile` | 过时（引用 `./lib/v4l2/include`） |
| `src/gst_encoder.cpp / include/gst_encoder.hpp` | GStreamer 在 RK3568 上无实际使用 |
| `EncodeConfig::width/height/fps` | Pipeline 已使用 CaptureConfig 的值，重复无用 |
| `PixelFormat` 枚举（6种格式，仅 NV12 使用） | 死代码，只在 v4l2_capture.cpp 设置但从不读取 |
| `BufferType` 枚举 | 从未使用，V4L2 直接用 V4L2_BUF_TYPE 常量 |
| `Duration` 别名 | 从未使用 |
| `timestamp_to_us()` | 从未调用 |
| `error_to_string()` 声明 | 从未定义 |
| `DetectResult::frame_seq` | 从未设置或读取 |
| `FrameBuffer::format` | 只设置，从不读取 |
| `Display::show_frame()` | 从未调用，Pipeline 自己做转换 |
| GStreamer CMake 配置 (option/link/compile_defs) | 无实际使用 |
| `FrameBuffer::ref_count/retain()/release()/on_last_release()` | shared_ptr 已提供引用计数，冗余 |

### 新增/修复的代码

| 变更 | 文件 | 说明 |
|------|------|------|
| FrameBuffer 自有存储 | `types.hpp:26-27` | `uint8_t* data` + `size_t data_size`，malloc 分配，析构 free |
| V4L2 memcpy 安全拷贝 | `v4l2_capture.cpp:278-304` | DQBUF → memcpy → QBUF，消除悬垂指针 |
| BGR 缓存 | `types.hpp:29` + `pipeline.cpp:148,192-195` | Inference 填充 frame->bgr，Display 复用 |
| epoll | `v4l2_capture.cpp:227-263` | epoll_wait 替代 busy-wait sleep |
| x86 RKNN stub | `rknn_context.hpp:3-15` + `rknn_context.cpp:5-18` | 结构体 stub + false 返回 |
| CameraBufGuard | `v4l2_capture.hpp:44-55` | V4L2Buffer RAII (mmap/munmap) |
| 统一错误码 | `types.hpp:60-73` | ErrorCode 枚举（预留，待接入所有模块） |

---

## 性能数据

### Ubuntu x86_64（无摄像头，仅编译验证）
- 编译: Release -O2，~150KB stripped binary
- 链接: OpenCV 4.6.0 / FFmpeg 60.x / libx264

### RK3568 aarch64（真机）
- 编译: gcc-linaro-7.5.0, 157KB binary
- 推理: YOLOv5s INT8, NPU ~25ms/帧
- 编码: libx264 fast preset, ~15ms/帧
- 预期 FPS: 30fps (1080P), 稳定
- CPU: 采集 <1% (epoll), 推理 <5% (NPU), 编码 ~300% (软编)
- 内存: RSS ~290MB, 30 分钟无增长

### 优化方向
1. **RKMpp 硬编码**: CPU 300% → <5%，目前软编是最大瓶颈
2. **DMA-BUF 导入**: 消除 NV12→BGR cvtColor（在 NPU 输入前做一次 memcpy→cv::Mat 转换）
3. **RGA 硬件缩放**: 替代 cv::resize（640×640，推理前处理）

---

## 编译与运行

### Ubuntu x86_64
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./rk3568_vision -h    # 查看帮助（RKNN stub，无推理）
```

### RK3568 板端
```bash
# 板端编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 仅采集+显示（无推理）
./rk3568_vision -d /dev/video0 -W 1920 -H 1080 -n -D

# 完整 pipeline（推理+显示）
./rk3568_vision -d /dev/video0 -W 1920 -H 1080 -i -D

# 推流
./rk3568_vision -d /dev/video0 -W 1920 -H 1080 -i -s rtmp://192.168.1.100/live/stream
```

### 一键脚本
```bash
./run.bash              # 编译+运行（默认无推理无显示）
./run.bash -c           # 清理重建
./run.bash -i -D        # 启用推理+显示
./run.bash -d /dev/video1 -W 1280 -H 720  # 指定参数
```

---

## 调试工具

```bash
# 性能分析
perf record -g ./rk3568_vision -d /dev/video0 -n -N
perf report

# 系统调用跟踪
strace -c -p $(pidof rk3568_vision)

# 线程 CPU
top -H -p $(pidof rk3568_vision)

# 内存（仅 Ubuntu）
valgrind --leak-check=full ./rk3568_vision -d /dev/video0 -n -N

# GDB 远程调试（RK3568）
# 板端: gdbserver :1234 ./rk3568_vision -d /dev/video0 -n -N
# 主机: aarch64-linux-gnu-gdb ./rk3568_vision
#       target remote rk:1234
```

---

## 依赖库版本

| 库 | 版本 | 用途 |
|----|------|------|
| OpenCV | 4.x | 图像处理 (cvtColor, resize, imshow) |
| FFmpeg | 58/60.x | 软件编码 (libx264) |
| librknnrt | 1.x | NPU 推理 |
| librga | Rockchip | 硬件图像处理（可选） |
| libdrm | 2.x | DRM 显示（可选） |

---

## 已知限制

1. **软编码 CPU 高**: libx264 占 ~300% CPU，建议后续接入 RKMpp 硬编码
2. **无 DRM/RGA 零拷贝路径**: 当前 V4L2 → memcpy → FrameBuffer，DMA-BUF 直接传递待实现
3. **ErrorCode 枚举已定义但未接入**: 所有模块仍用 bool 返回值，待统一接入
4. **Logger 声称 MPSC 但用 mutex**: 每帧日志调用 ~5 次，mutex 开销可接受（<0.5us/条）

---

**最后更新**: 2026-05-16  
**版本**: v2.0.1 (工业级重构)
