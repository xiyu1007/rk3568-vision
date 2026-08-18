# 测试说明

本项目按下面的顺序测试，从简单到完整：

1. **测试 A：mp4 输入 → RTMP 推流**（不需要摄像头，先验证「解码 → 推理 → 编码 → 推流」全链路）
2. **测试 B：真实摄像头 → RTMP 推流**（完整链路：采集 → 推理 → 编码 → 推流）

两个测试都在 **rk3568 板端**运行；ubuntu 虚拟机 / WSL 22.04 做 x86 交叉编译（编出板端可运行二进制）。

---

## 0. 测试环境

| 环境          | 登录方式       | 架构    | 用途                                |
| ------------- | -------------- | ------- | ----------------------------------- |
| rk3568 板端   | `ssh rk3568` | aarch64 | 编译 + 运行 + 测试（真实 NPU 推理） |
| ubuntu 虚拟机 / WSL 22.04 | `ssh ubuntu` / `ssh wsl-22.04` | x86_64  | x86 交叉编译 aarch64（`make CROSS_COMPILE=aarch64-linux-gnu-`） |
| Windows 本机  | —             | —      | 用 VLC 拉流看画面（可选）           |

> 两端代码相同。Makefile 按 `uname -m` / `CROSS_COMPILE` 自动判断：aarch64 链接 `librknnrt.so`
> 生成可执行文件；x86 交叉编译用 `third_lib/aarch64-sysroot` 编出 aarch64 二进制（交叉工具链
> GLIBC 需 ≤ 板端 2.35，用 Ubuntu 22.04 工具链即可）；`make check` 则只做语法检查。

---

## 1. 一次性准备（每台板端开机 / 重装后做一次）

### 1.1 编译

在**板端**执行：

```bash
cd /home/gx/project/gx/rk3568-vision
./install.sh            # 一键部署+编译+运行（自动检测/装依赖，编译后直接跑）
# 或手动：make          # 生成 output/rk3568_vision
```

看到 `==> 构建完成: output/rk3568_vision` 即成功。

> 依赖：`build-essential`、`pkg-config`、FFmpeg/RGA/MPP 开发库；RKNN 运行时/mediamtx 均已入库
> `third_lib/`，`./install.sh` 会自动装依赖并编译，无需手动处理。

### 1.2 确认 RTMP 服务器（mediamtx）在运行

流水线把流推到板端本机的 mediamtx（监听 1935 端口）。在**板端**确认：

```bash
ss -tlnp | grep 1935        # 应看到 LISTEN 0.0.0.0:1935
```

- 已监听 → 直接进入测试。
- 未监听 → 启动 mediamtx（单二进制 + 配置都在 `third_lib/mediamtx/` 下），或直接用一键启动脚本：

```bash
cd third_lib/mediamtx && ./mediamtx &       # 后台启动，默认监听 1935
# 或一键启动（mediamtx + app）：./scripts/start.sh -c conf/test_mp4.yaml
```

---

## 2. 测试 A：mp4 输入 → RTMP 推流（推荐先做）

**目的**：不用摄像头，用 `data/test.mp4` 循环播放作视频源，验证整条流水线
（mp4 解码 → 稳帧 → NPU 推理画检测框 → H.264 编码 → RTMP 推流）。

### 第 1 步（板端）：启动推流

```bash
cd /home/gx/project/gx/rk3568-vision
./output/rk3568_vision -c conf/test_mp4.yaml
```

> 程序会一直跑（mp4 循环播放），按 `Ctrl+C` 停止。
> 配置文件 `conf/test_mp4.yaml` 与 `conf/default.yaml` 的唯一区别是 `capture.source: mp4`。

### 第 2 步（任意机器）：拉流验证

**方式一：快速确认流存在**（板端本机执行最快）

```bash
ffprobe -v error -show_entries stream=codec_name,codec_type,width,height \
  -of default=noprint_wrappers=1 rtmp://127.0.0.1:1935/live/stream
```

**方式二：严格确认视频帧在传输**（拉 3 秒存文件，看 frame 数）

```bash
ffmpeg -hide_banner -i rtmp://127.0.0.1:1935/live/stream -t 3 -c copy -f flv /tmp/out.flv
# 输出最后一行应看到 frame=NN（NN>0），且 /tmp/out.flv 文件大小 > 几百 KB
```

**方式三：人眼确认**（需要图形界面，在 ubuntu 虚拟机或 Windows 上）

```bash
# ubuntu 虚拟机执行（板端 IP 192.168.31.46）
ffplay rtmp://192.168.31.46:1935/live/stream
```

Windows 用 VLC：「媒体」→「打开网络串流」→ 输入 `rtmp://192.168.31.46:1935/live/stream`。

### 怎么算成功

- 板端日志出现 `muxer: opened flv -> rtmp://127.0.0.1:1935/live/stream`；
- `monitor` 行里 `push=N` 持续增长，`drop=0`；
- 拉流端能拿到 `h264 1280x720`（+ `aac` 静音轨）和真实视频帧；
- 能看到画面上有**检测框**（NPU 推理的框，处理画面）。

看到这些即说明「解码→推理→编码→推流」全链路通了，可进入测试 B。

---

## 3. 测试 B：真实摄像头 → RTMP 推流（完整链路）

**目的**：用 IMX415 摄像头采集真实画面，跑完整链路「采集 → 推理 → 编码 → RTMP 推流」。

### 前置：确认摄像头被检测到

在**板端**执行：

```bash
v4l2-ctl --list-devices        # 应看到 rkisp_mainpath 下的 /dev/video0
```

看到 `/dev/video0`（rkisp_mainpath）即摄像头正常。若看不到，检查 MIPI 排线/供电。

### 第 1 步（板端）：启动推流

```bash
cd /home/gx/project/gx/rk3568-vision
./output/rk3568_vision -c conf/default.yaml -d /dev/video0 -W 720 -H 480 -f 25
# ./output/rk3568_vision -c conf/default.yaml -d /dev/video0 -W 1280 -H 720 -f 25
```

> `conf/default.yaml` 的 `capture.source` 默认就是 `v4l2`，分辨率 1280x720@25fps。
> 程序持续运行，`Ctrl+C` 停止。

### 第 2 步：拉流验证

同「测试 A 第 2 步」，把 `rtmp://127.0.0.1:1935/live/stream` 换成拉流地址即可
（板端本机用 127.0.0.1，跨机器用 `rtmp://192.168.31.46:1935/live/stream`）。

### 怎么算成功

- 板端日志出现 `v4l2: /dev/video0 1280x720@25`（采集成功）；
- 接着出现 `muxer: opened flv -> rtmp://127.0.0.1:1935/live/stream`；
- `monitor` 行 `frames=N push=N drop=0`，N 持续增长；
- 拉流端拿到摄像头实时画面（带检测框）。

看到这些即摄像头完整链路通过。

---

## 4. 测试 C（可选）：本地录制 MP4（不推流）

只想把处理画面存成本地文件、不推流时：

```bash
# mp4 输入
./output/rk3568_vision -c conf/test_mp4.yaml --no-stream --record output/test.mp4

# 摄像头输入
./output/rk3568_vision -c conf/default.yaml --no-stream --record output/test.mp4
```

录制一段时间后 `Ctrl+C` 停止，检查文件：

```bash
ffprobe -v error -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 output/test.mp4
```

---

## 4. 测试 D：摄像头纯推流（不推理）

只想把摄像头画面直接推流、不做检测（最低延迟、最小 CPU）时，用 `conf/camera_push.yaml`
（`inference.enabled: false`，跳过 RKNN 推理和画框）：

```bash
./scripts/start.sh -c conf/camera_push.yaml -d /dev/video0
# 或
./output/rk3568_vision -c conf/camera_push.yaml -d /dev/video0
```

**怎么算成功**：日志里**没有** `rknn: model input`（推理关闭），`monitor` 行 `inf=0ms`，
拉流端能拿到摄像头画面（无检测框）。

---

## 5. 验证命令速查

| 目标         | 命令                                                                                           |
| ------------ | ---------------------------------------------------------------------------------------------- |
| 流是否存在   | `ffprobe -v error -show_entries stream=codec_name,width,height rtmp://<IP>:1935/live/stream` |
| 帧是否在传输 | `ffmpeg -i rtmp://<IP>:1935/live/stream -t 3 -c copy -f flv /tmp/out.flv`                    |
| 人眼预览     | `ffplay rtmp://<IP>:1935/live/stream`                                                        |
| 检查录制文件 | `ffprobe -v error -show_streams output/test.mp4`                                             |
| 看性能/延迟  | 日志`monitor: ... enc=Xms inf=Xms cap=Xms \| frames=N drop=N`                                 |

> `<IP>` 填板端 IP `192.168.31.46`；在板端本机可直接用 `127.0.0.1`。

---

## 6. 常见问题

1. **x86 `make` 不产生可执行文件** —— 正常：x86 默认只做编译检查；要产出 aarch64 可执行文件用
   `make CROSS_COMPILE=aarch64-linux-gnu-`（交叉编译），真实推理仍需在板端 NPU 上运行。
2. **硬编失败回退软编** —— `encode.hardware=true` 时优先用 Rockchip MPP 硬编，MPP 打不开会自动回退 libx264 软编；日志里 `encoder=hw(mpp)` / `encoder=sw(libx264)` 可见当前用哪种。
3. **`v4l2: open /dev/video0 failed`** —— 摄像头未检测到，先 `v4l2-ctl --list-devices` 确认，检查排线/供电。
4. **mediamtx 拉流只有流信息、0 视频帧** —— mediamtx v1.9.3 不转发「纯视频、无音频」的 RTMP 流
   （ffmpeg `-c:v libx264 -an` 推流同样复现：拉流端 0 帧）。本项目已在 FLV 中自动补一路静音 AAC 音轨
   （muxer.cpp 硬编码静音帧实现），无需手动处理；也可升级 mediamtx 到修复该问题的版本。
5. **改 `include/*.hpp` 后程序莫名段错误** —— Makefile 规则 `build/%.o: src/%.cpp` 不跟踪头文件依赖，
   改头文件后必须 `make clean` 全量重编，否则新旧 `.o` 布局不一致会段错误。
6. **拉流长时间 0 帧，但板端 `push=N` 正常增长、日志含 `silent AAC track added`** —— 静音音轨时间戳
   滞后会导致 mediamtx 只发 FLV 头、不转发视频帧：早前 writeSilentAudio 每视频帧只写 1 个音频帧，实际
   帧率远低于 AAC 采样率所需，音频时间戳严重落后于视频。现已改为按视频时间戳对齐音频时间戳（写足帧）。
   若仍复现，检查音视频时间戳是否对齐。
