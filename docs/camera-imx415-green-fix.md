# IMX415 摄像头「画面发绿/发暗」问题排查与解决记录

> 适用环境：LubanCat-RK356x，IMX415 MIPI CSI 摄像头。
> 触发场景：把开发板系统从 ubuntu20.04 + kernel-5.10 升级到 **ubuntu22.04 + kernel-6.1** 后，
> 摄像头画面整体发绿、发暗（之前只是焦距环没拧导致模糊，颜色是正常的）。

---

## 1. 症状

- 升级系统后，摄像头输出画面**偏绿、偏暗**，看起来像灰绿色、没有正常色彩。
- 亮度数据（ffmpeg `signalstats`）：`YAVG≈42`（很暗，正常应 100+）、`UAVG≈121`/`VAVG≈121`（都低于中性 128，呈绿色偏置）。
- 用 V4L2 控制台看到的曝光/增益：`analogue_gain=0`（无增益）、`exposure=2242`（满量程）。

## 2. 定位过程（如何判断是 3A 还是 IQ 文件问题）

按顺序做了下面几步，逐步缩小范围：

1. **确认不是本项目代码问题**：用 `v4l2-ctl` / `ffmpeg` 直接抓 `/dev/video0` 的原始 NV12 帧，
   绕过我们自己的 pipeline。结果原始帧**同样偏绿偏暗** → 说明是摄像头/驱动/ISP 的问题，不是
   pipeline 的编码或转换问题。

2. **确认 ISP 的 3A 引擎没有在跑**：
   ```bash
   ps aux | grep -i rkaiq          # 没有 rkaiq_3A_server 进程
   systemctl status rkaiq_3A       # inactive (dead)
   ```
   Rockchip 的 ISP 需要 `rkaiq_3A_server`（3A = 自动曝光/自动增益/自动白平衡）才能输出正常色彩。
   没有它，传感器 raw 输出就是偏绿偏暗的。

3. **手工运行 3A 服务看报错**：
   ```bash
   /usr/bin/rkaiq_3A_server
   ```
   日志关键报错：
   ```
   XCORE:E:access /etc/iqfiles//imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json && ...bin failed!
   XCORE:E:_rkAiqManager init error!
   CAMHW:E:can't find sensor
   ```
   这说明 3A 服务启动时**找不到 IMX415 对应的 IQ 标定文件**。

4. **确认 IQ 文件缺失**：`ls /etc/iqfiles/` 里只有 gc02m2 / ov8858 等一堆别的传感器的 IQ 文件，
   **没有 imx415 开头的文件**。

> 结论：不是传感器坏、不是排线、不是代码，而是 **`/etc/iqfiles/` 缺了 IMX415 的 IQ 标定文件**，
> 导致 `rkaiq_3A_server` 起不来，ISP 无法做白平衡/曝光/增益。

## 3. 根因：22.04 的 rkaiq 包把 IMX415 IQ 文件删掉了

IQ 文件是 Rockchip 相机引擎 `camera_engine_rkaiq` 包的一部分，随系统镜像分发。
对比 SDK（`ubuntu` 虚拟机 `/home/gx/project/linux/LubanCat_SDK`）里各发行版 rkaiq 包的内容：

| 发行版 | camera_engine_rkaiq 版本 | 是否含 IMX415 IQ 文件 |
|--------|--------------------------|----------------------|
| ubuntu20.04 / debian11 | 5.0x4.1（AIQ 5.x） | ✅ 有（331KB） |
| **ubuntu22.04** | **6.8.0（AIQ 6.x）** | ❌ **没有** |
| **debian12** | **6.9.0（AIQ 6.x）** | ✅ **有（631KB）** |

两个关键点：

1. **22.04 的包把 IMX415 的 IQ 文件删掉了**（33 个 IQ 文件变成 32 个，正好少 imx415 那一个）。
2. **AIQ 版本升级后 IQ 文件格式变了**：20.04 的 IQ 文件是 AIQ 5.x 格式，直接塞给 AIQ 6.x 的
   `rkaiq_3A_server` 会**段错误**（解析器遇到不认识的 enum，随后崩溃）。所以不能简单地把旧文件拷回来。

## 4. 解决方案（已执行并验证）

### 4.1 拿到正确的 IQ 文件

从 SDK 的 **debian12** rkaiq 包里取 AIQ 6.x 格式的 IMX415 IQ 文件：

```bash
# 在 ubuntu 虚拟机（有 SDK）上：
cd /home/gx/project/linux/LubanCat_SDK
dpkg-deb -x debian12/packages/arm64/rkaiq/camera_engine_rkaiq_rk3568_arm64.deb /tmp/deb12
# /tmp/deb12/etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json  ← 631KB，就是要这个
```

文件名为 `imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json`
（CMK-OT1522-FG3 是相机模组、CS-P1150 是镜头型号，必须与硬件模组完全匹配，不能用别的传感器的 IQ 文件）。

### 4.2 部署到板端

```bash
# 1. 把 IQ 文件放到板端 /etc/iqfiles/（需要 root）
sudo cp imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json /etc/iqfiles/

# 2. 升级 rkaiq 到 6.9.0（因为 22.04 的 6.8.0 对 imx415 支持不完整）
sudo dpkg -i camera_engine_rkaiq_rk3568_arm64.deb   # 来自 debian12 的包

# 3. 启动 3A 服务（必须以 root 跑；普通用户跑会段错误，因为要访问 /dev/v4l-subdev0 等设备）
sudo systemctl enable rkaiq_3A --now
```

### 4.3 验证

```bash
systemctl is-active rkaiq_3A   # 应输出 active

# 拉流看亮度/色彩（ffmpeg signalstats）
ffmpeg -i rtmp://<IP>:1935/live/stream -frames:v 1 -vf "signalstats,metadata=print" -f null - 2>&1 | grep -E "YAVG|UAVG|VAVG"
```

修复后 `YAVG≈128`（正常亮度）、`UAVG≈128`、`VAVG≈128`（白平衡中性，绿色消失）。

## 5. 附：画面偏亮/偏暗如何微调（Evbias）

3A 引擎在跑的时候会**持续自动控制曝光和增益**，所以 `v4l2-ctl -d /dev/video0 -c exposure=xxx`
**改不动**（会被 3A 实时覆盖）——这是正常的，说明 3A 已经接管了。

要整体调亮/调暗，改 IQ 文件里的 **AEC 曝光补偿 `Evbias`**（单位 EV，负值=变暗，正值=变亮）：

```bash
# 备份
sudo cp /etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json /tmp/iq.bak.json

# 把 Evbias 从 0 改成 -0.3（约 1/3 档更暗；想更暗用 -0.5 ~ -1.0）
sudo python3 - <<'PY'
import json
p = "/etc/iqfiles/imx415_CMK-OT1522-FG3_CS-P1150-IRC-8M-FAU.json"
d = json.load(open(p))
def f(o):
    if isinstance(o, dict):
        if "Evbias" in o: o["Evbias"] = -0.3
        for v in o.values(): f(v)
    elif isinstance(o, list):
        for v in o: f(v)
f(d)
json.dump(d, open(p, "w"))
PY

# 重启 3A，再重启推流进程（顺序很重要：先起 3A 等它挂到 ISP，再让摄像头重新出流，
# 否则 3A 收不到 stream-start 事件，AEC 不生效）
sudo systemctl restart rkaiq_3A
# 然后重启你的 rk3568_vision 推流进程
```

> 注意：改完 IQ 文件后**必须同时重启 `rkaiq_3A` 和摄像头推流进程**（先 3A 后推流）。
> 若只重启 3A 不重启推流，3A 会卡在 `wait stream start event`，曝光停在很暗的值上。

## 6. 关键结论

- 发绿/发暗是**板端系统配置问题**，不是本项目代码问题。我们的 pipeline 只是忠实编码摄像头 NV12。
- IQ 文件必须与**硬件模组（CMK-OT1522-FG3）+ 镜头（CS-P1150）**匹配，且要与 **AIQ 版本（6.x）**匹配。
- 换系统（尤其升级 ubuntu/内核）后，如果画面发绿发暗，第一件事就是检查 `rkaiq_3A` 服务状态和 `/etc/iqfiles/` 里对应传感器的 IQ 文件。
