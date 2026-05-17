# RK3568 Vision Pipeline v3.0.0 - Implementation Notes

## Project Overview

RK3568 embedded Linux vision pipeline: V4L2 capture -> YOLOv5 NPU inference -> H.264 encode -> RTMP push / display.

Platforms: Ubuntu x86_64 (dev, RKNN stub), RK3568 aarch64 (production, NPU hardware).

Version: v3.0.0 (C-core rewrite). Date: 2026-05-17.

## Architecture

Design: "Less code, same function." v2.0.2 C++17 -> v3.0.0 C11 core + minimal C++ OpenCV bridge.

Thread model (7 threads):
- Main: signal wait, lifecycle (sleep 1s)
- Capture: V4L2 DQBUF via epoll_wait(100ms)
- Inference: NV12->BGR -> RKNN NPU -> NMS -> push to 2 queues
- Encode: sws_scale NV12->YUV420P -> libx264 -> callback->RTMP
- Display: cv::imshow + FPS overlay
- Monitor: /proc polling (sleep 2s)
- Logger: async batch fwrite (pthread_cond_timedwait 100ms)

Data flow:
```
IMX415 -> ISP -> DMA Buffer (kernel)
  V4L2 DQBUF (epoll wakeup)
  memcpy(~1ms) -> frame_t::data (NV12)
  push cap_queue (SPSC, cap=8)
    pop
Infer Thread:
  NV12->BGR (software, ~3ms)
  RKNN NPU inference (YOLOv5s INT8, ~25ms)
  NMS post-process (~1ms)
  frame_t::bgr_data cached
    push inf_queue -> Encode Thread
    push disp_queue -> Display Thread
```

## Module Map

### C Modules (12 .c + 14 .h)

| File | Lines | Description |
|------|-------|-------------|
| main.c | 109 | Entry point, getopt CLI, pipeline lifecycle |
| config.c | 192 | Minimal YAML parser, zero external deps |
| logger.c | 192 | Async logger: ring buffer + pthread writer |
| v4l2.c | 217 | V4L2 capture: mmap, epoll, memcpy to frame |
| pipeline.c | 296 | Multi-threaded pipeline: SPSC queues, pthread |
| encoder.c | 127 | libx264 wrapper: NV12->YUV420P sws_scale |
| rtmp.c | 96 | RTMP push via FFmpeg libavformat |
| rknn.c | 130 | RKNN NPU wrapper (ARM real + x86 stub) |
| fps.c | 23 | FPS counter: sliding window, clock_gettime |
| sig.c | 27 | Signal handler: sigaction() for SIGINT/SIGTERM |
| perf.c | 23 | Performance counters |
| monitor.c | 72 | System monitor: /proc/stat, /proc/meminfo |

Key headers: types.h (frame_t, detect_result_t, app_cfg_t), ringbuf.h (SPSC lock-free), bridge.h (C->C++ adapter).

### C++ Modules (4 .cpp + 3 .hpp)

| File | Lines | Why C++ |
|------|-------|---------|
| detector.cpp | 177 | cv::Mat BGR->RGB->resize, YOLOv5 NMS |
| display.cpp | 31 | cv::namedWindow, cv::imshow |
| bridge.cpp | 99 | C<->C++ adapter for detector/display |
| rknn_context.cpp | 104 | RAII rknn_context (used by detector) |

## SPSC Ring Buffer

include/ringbuf.h - single-header, zero dynamic allocation.
Cache-line padded (64B) to prevent false sharing on ARM Cortex-A55.
Uses __atomic_load_n/__atomic_store_n with acquire/release ordering.
Queue full -> push returns false (non-blocking drop).

## V4L2 Capture with Epoll

Open: O_RDWR | O_NONBLOCK, VIDIOC_QUERYCAP, VIDIOC_S_FMT, VIDIOC_REQBUFS, mmap.
Start: VIDIOC_QBUF, VIDIOC_STREAMON, epoll_create1, epoll_ctl(EPOLL_CTL_ADD).
Capture: epoll_wait(100ms), VIDIOC_DQBUF, memcpy to frame_t, VIDIOC_QBUF.
CPU: <1% (epoll kernel wakeup vs busy-wait).

## YOLOv5 Detection Pipeline

Preprocessing (detector.cpp): BGR->RGB (cvtColor), resize to 640x640.
RKNN inference (rknn_context.cpp): rknn_init -> rknn_inputs_set -> rknn_run -> rknn_outputs_get.
Postprocessing (detector.cpp): INT8 dequantize, sigmoid, anchor decode, NMS per class.

## libx264 Encoding

encoder_open(): avcodec_find_encoder_by_name("libx264"), avcodec_alloc_context3, avcodec_open2.
NV12->YUV420P via sws_scale (SWS_FAST_BILINEAR).
On RK3568: ARMv8 NEON SIMD, ~15ms latency at 1080P.

## Platform Detection (CMakeLists.txt)

if(HOST_ARCH MATCHES aarch64|arm64): set(IS_RK3568 ON), add_definitions(-DRK3568_NATIVE)
else: set(IS_RK3568 OFF), add_definitions(-DX86_DEBUG)

x86: RKNN stub (returns NULL), inference auto-disabled. OpenCV/FFmpeg via system pkg-config.
aarch64: Real RKNN via librknnrt.so. libdrm linked for DRM display.

## v3.0.0 Changes from v2.0.2

C Conversion (13 files): v4l2, config, logger, ringbuf, perf, monitor, encoder, rtmp, rknn, pipeline, main, sig, fps - all C++ -> C.

Bug fixes:
- Pipeline::init() always true -> pipeline_create() returns NULL on failure
- NV12->BGR duplication -> cached in frame_t::bgr_data
- signal() -> sigaction() with proper sa_mask
- YAML parser quotes -> trim_quotes() added
- rknn_init name clash -> rknn_ctx_* prefix
- Config loaded twice -> single load path

Deletions: osd.cpp/hpp (merged into bridge), share/ directory, types.hpp (-> types.h), 11 old .hpp, 10 old .cpp.

## Build and Run

### Ubuntu x86_64
```
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
./rk3568_vision -h
```

### RK3568 aarch64
```
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4
./rk3568_vision -d /dev/video0 -W 1920 -H 1080 -n -N
./rk3568_vision -d /dev/video0 -W 640 -H 480 -N -f 15
```

### One-shot
```
./run.bash              # build + run
./run.bash -c           # clean rebuild
./run.bash -i -D        # enable inference + display
```

## Directory Structure (Final)

```
rk3568-vision/
  include/          # 17 headers (14 .h + 3 .hpp)
    types.h, ringbuf.h, logger.h, v4l2.h, pipeline.h
    encoder.h, rtmp.h, rknn.h, config.h, bridge.h
    sig.h, fps.h, perf.h, monitor.h
    detector.hpp, display.hpp, rknn_context.hpp
  src/             # 16 sources (12 .c + 4 .cpp)
    main.c, config.c, logger.c, v4l2.c, pipeline.c
    encoder.c, rtmp.c, rknn.c, fps.c, sig.c, perf.c, monitor.c
    bridge.cpp, detector.cpp, display.cpp, rknn_context.cpp
  config/default.yaml, model/, data/, third_lib/
  build/, CMakeLists.txt, run.bash, note.md
```

## Test Results

### Ubuntu x86_64
- Build: 0 errors, 0 warnings (Release -O2)
- Binary: ELF 64-bit x86-64
- Help: OK, config: 41 entries loaded

### RK3568 aarch64
- Build: 0 errors, 5 benign strncpy warnings
- Binary: ELF 64-bit ARM aarch64
- V4L2: /dev/video0, 1920x1080 and 640x480 tested
- RKNN: model/yolov5s.rknn loaded (7.7MB), NPU running, IO: 1in/3out
- libx264: ARMv8 NEON, ~3779 kbps
- Shutdown: SIGINT -> clean exit
- Warning: "rknn_set_core_mask: No implementation found" (benign, single NPU core)

## Performance (RK3568, 640x480@15fps, YOLOv5s INT8)

| Stage | Latency |
|-------|---------|
| V4L2 capture | <0.1ms |
| memcpy DMA->frame | ~0.5ms |
| NV12->BGR | ~1ms |
| RKNN inference | ~25ms |
| NMS postprocess | ~1ms |
| libx264 encode | ~15ms |
| Total | ~42ms (budget 66ms) |

## Known Limitations

1. malloc per frame (future: frame pool)
2. Software NV12->BGR (future: RGA hardware)
3. Software libx264 (future: RKMpp hardware encoder)
4. No DMA-BUF zero-copy
5. strncpy format-truncation warnings (benign)

## Dependencies

OpenCV 4.6.0, FFmpeg 60.x, librknnrt 1.x, librga (optional), libdrm (optional)
