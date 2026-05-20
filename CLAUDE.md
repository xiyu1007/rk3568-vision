# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
# Build + run (default)
./run.bash

# Build only
./run.bash -b

# Run only (skip build)
./run.bash -r

# Clean rebuild
./run.bash -c

# Manual build
mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

# Run directly with CLI overrides
./build/rk3568_vision -c config/default.yaml -d /dev/video0 -W 640 -H 480 -f 30 -n  # -n disables inference
```

CLI flags: `-d DEV` (V4L2 device), `-W/H` (resolution), `-f FPS`, `-s URL` (RTMP), `-n` (disable inference), `-N` (disable display), `-v` (verbose).

## Architecture

This is a real-time **V4L2 capture → RKNN inference → H.264 encode → RTMP stream + local display** pipeline for the Rockchip RK3568 (ARM + 1 TOPS NPU).

### Pipeline (4 threads, connected by lock-free SPSC ring buffers)

```
capture_thread ──cap_q──▶ inference_thread ──inf_q──▶ encode_thread ──▶ RTMP push
                              │
                              └──disp_q──▶ display_thread (OpenCV imshow)
```

- **capture**: V4L2 mmap/DMABUF → `frame_t` (NV12) → push to `cap_q`
- **inference**: pop `cap_q` → NV12→BGR (OpenCV cvtColor) → YOLOv5s INT8 on NPU (via RKNN API) → push to `inf_q` + `disp_q`
- **encode**: pop `inf_q` → FFmpeg libx264 hardware encode → callback → RTMP push
- **display**: pop `disp_q` → draw FPS/detection boxes → `cv::imshow`

When inference is disabled, capture pushes directly to `inf_q` (bypass for encode) and `disp_q`.

### Hybrid C/C++ with bridge pattern

The core pipeline is **C** (no exceptions, RAII). AI and display modules are **C++**. `bridge.cpp` provides C-callable wrappers so the pipeline never includes C++ headers directly.

| Layer | Language | Files |
|-------|----------|-------|
| Pipeline core | C | `src/main.c`, `src/pipeline.c`, `src/v4l2.c`, `src/encoder.c`, `src/rtmp.c` |
| Utilities | C | `src/config.c`, `src/logger.c`, `src/fps.c`, `src/perf.c`, `src/monitor.c`, `src/sig.c` |
| NPU context | C++ | `src/rknn_context.cpp` — RAII wrapper for RKNN API |
| Detector | C++ | `src/detector.cpp` — YOLOv5s post-processing (decode + NMS) |
| Display | C++ | `src/display.cpp` — OpenCV window wrapper |
| C↔C++ bridge | C++ | `src/bridge.cpp` — `extern "C"` wrappers for detector, display, NV12→BGR |

### Lock-free ring buffer (`include/ringbuf.h`)

SPSC, cache-line-padded head/tail (64-byte aligned to avoid false sharing). Caller provides storage — zero dynamic allocation. Used for all three inter-thread queues (`cap_q`, `inf_q`, `disp_q`).

### Dual-platform build

- **aarch64 (RK3568)**: Real RKNN API linked from `third_lib/librknn_api/aarch64/librknnrt.so`. Optionally links RGA (hardware resize) and DRM (zero-copy display). Macro `RK3568_NATIVE` defined.
- **x86_64 (dev)**: RKNN types stubbed in `include/rknn_context.hpp` under `#ifdef X86_DEBUG`. Code compiles but inference won't run. Macro `X86_DEBUG` defined.

### Key types (`include/types.h`)

- `frame_t` — NV12 frame buffer + metadata (seq, timestamps, BGR cache, detection results, atomic refcount)
- `detect_result_t` / `detect_box_t` — detection output (up to 64 boxes)
- `app_cfg_t` — full app config, loaded from YAML by `config_load()`
- `ringbuf_t` — SPSC lock-free queue

### Configuration

YAML-based (`config/default.yaml`), loaded via a minimal C YAML parser in `src/config.c`. All sections (capture, inference, encode, stream, display, monitor) can be overridden via CLI flags.

## Key dependencies

- **OpenCV 4**: `imgproc`, `highgui`, `imgcodecs`, `videoio` (bundled in `third_lib/opencv/`)
- **RKNN API**: NPU inference runtime (bundled in `third_lib/librknn_api/`)
- **FFmpeg**: `libavcodec`, `libavformat`, `libavutil`, `libswscale` (system/pkg-config)
- **RGA** (optional, aarch64 only): Rockchip hardware image resize/color convert
- **DRM** (optional, aarch64 only): Direct Rendering Manager for zero-copy display


我已经大幅修改代码，现在有功能需要测试和解决，尽可能做最小的代码改动，避免不必要修改,现在有几个问题待解决：
1. 摄像头模糊（跟代码无关，是跟rk3568的底层驱动有关吗，感觉焦距什么的不对），能否ssh rk (工作目录为/home/gx/linux/rk3568/rk3568-vision,修改代码后需要同步过去，并ssh rk连接进行测试)调试解决，
2. 不知道是不是摄像头太模糊，导致识别结果一直是person,还有绘制的方框大小根本不匹配
3. 一些功能没实现，RTMP 推流，我需要测试从rk3568推流到ubuntu，确保ubuntu能正确接收
下面完成上面3个功能 