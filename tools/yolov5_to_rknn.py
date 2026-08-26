#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# yolov5_to_rknn.py — YOLOv5(ONNX) → RKNN 模型转换脚本
#
# 作用：把 YOLOv5 的 ONNX 模型转换成 RK3568 NPU 能直接跑的 .rknn 模型。
# 依赖：rknn-toolkit2 >= 2.3.2（支持 Python 3.8~3.12，装在本仓库的 ubuntu 虚拟机）
#
# 用法：
#   python3 yolov5_to_rknn.py <onnx路径> [平台] [量化类型] [输出rknn路径]
#
#   示例：
#   python3 yolov5_to_rknn.py yolov5n.onnx rk3568 i8 ../model/yolov5n.rknn
#   python3 yolov5_to_rknn.py yolov5s.onnx rk3568 i8 ../model/yolov5s.rknn
#   python3 yolov5_to_rknn.py yolov5s_relu.onnx rk3568 i8 ../model/yolov5s_relu.rknn
#
# ============================================================================
# 三个核心概念：量化 / 调参 / 轻量化
# ----------------------------------------------------------------------------
# 【1. 量化（Quantization）—— 让模型又小又快】
#   训练出来的模型默认是 FP32（每个权重 4 字节浮点）。量化就是把 FP32 的数
#   映射到低比特（RK3568 NPU 最常用 INT8，每个权重 1 字节整数）：
#     - 体积：FP32 → INT8，约缩到 1/4；
#     - 速度：NPU 上 INT8 比 FP16/FP32 快 2~4 倍；
#     - 精度：INT8 会损失一点精度，通常 mAP 掉 0.5~2%，检测场景可接受。
#   量化分两类：
#     - 训练后量化（PTQ）：本项目用的就是这种，不需要重新训练，只需一张量
#       化校准集（几十张代表性图片）统计各层激活值的分布，算出 scale/zero_point。
#     - 量化感知训练（QAT）：训练时就模拟量化，精度更高，但要重新训练，成本大。
#   关键参数：do_quantization（是否量化）+ dataset（校准集）。
#
# 【2. 调参（Tuning）—— 转换时能调的旋钮】
#   a) mean/std 归一化：输入图片先 (x - mean) / std。YOLOv5 惯例 mean=[0,0,0]、
#      std=[255,255,255]，即把 [0,255] 像素归一化到 [0,1]。改错会导致检测全错。
#   b) 量化校准集（dataset）：图片越多越有代表性，量化精度越高，但转换越慢。
#      20~100 张通常够用。
#   c) 量化算法（quantized_algorithm）：normal（默认，快）/ mmse（更准，慢）/
#      kl_divergence 等，精度与转换时间的取舍。
#   d) 优化等级（optimization_level）：0~3，越高优化越激进、可能略掉精度。
#   e) 目标平台（target_platform）：rk3568 / rk3588 / rk3566 等，决定 NPU 的
#      算子支持，必须和实际板子一致。
#   f) 后处理阈值（conf/NMS）不在转换脚本里调，在 conf/*.yaml 的 inference 段调。
#
# 【3. 轻量化（Lightweight）—— 换更小的模型 / 更省算力的激活】
#   a) 模型尺寸：YOLOv5 有 n(1.9M) < s(7.2M) < m(21M) < l(46M) < x(87M)。
#      越小越快、精度越低。RK3568（1TOPS NPU）上：
#         yolov5n  ≈ 25ms/帧，yolov5s ≈ 70ms/帧（差距约 3 倍）。
#   b) 激活函数：silu（标准）在 NPU 上要算 sigmoid+乘法，较慢；改成 relu 后
#      NPU 上快不少。这就是 model/yolov5s_relu.rknn 的由来（精度略降、速度提升）。
#   c) 量化本身也是轻量化：INT8 把权重压到 1/4。
#
# 本项目实际用的三种模型及后处理差异（对应 conf 里 inference.use_sigmoid）：
#   - yolov5n.rknn   ：n 尺寸，输出已是 sigmoid 后的值 → use_sigmoid = false
#   - yolov5s_relu.rknn：s 尺寸 + relu 激活，输出已是 sigmoid 后值 → use_sigmoid = false
#   - yolov5s.rknn   ：标准 silu，输出是 logits（原始分数）→ use_sigmoid = true
#   （这个差异来自导出 ONNX 时是否把 sigmoid 并进模型，与转换脚本无关，换模型时
#    务必同步改 use_sigmoid，否则阈值失效、候选暴增、后处理慢到秒级。）
# ============================================================================

import sys
import argparse

from rknn.api import RKNN

# ============================================================================
# 一、可调参数区（转换前按需修改）
# ============================================================================

# 量化校准集：txt 里每行一张图片路径（相对或绝对）。
# 来源：rknn_model_zoo 仓库 datasets/COCO/coco_subset_20（20 张 COCO 图）。
# 自建时准备 20~100 张与业务场景接近的图即可。
DATASET_PATH = "datasets/COCO/coco_subset_20.txt"

# 归一化参数：YOLOv5 固定 mean=[0,0,0]、std=[255,255,255]。
MEAN_VALUES = [[0, 0, 0]]
STD_VALUES = [[255, 255, 255]]

# 默认目标平台：RK3568（本项目板子）。
DEFAULT_PLATFORM = "rk3568"

# 默认量化类型："i8"（INT8，推荐）/ "fp"（FP16）/ "hybrid"（混合）。
DEFAULT_QUANT = "i8"


def parse_args():
    """解析命令行参数，返回 (onnx路径, 平台, 是否量化, 输出路径)。"""
    parser = argparse.ArgumentParser(
        description="YOLOv5 ONNX 转 RKNN（量化/调参/轻量化见脚本头部注释）")
    parser.add_argument("onnx", help="输入 ONNX 模型路径")
    parser.add_argument("platform", nargs="?", default=DEFAULT_PLATFORM,
                        help=f"目标平台，默认 {DEFAULT_PLATFORM}（可选 rk3568/rk3588/rk3566...）")
    parser.add_argument("quant", nargs="?", default=DEFAULT_QUANT,
                        choices=["i8", "fp", "hybrid"],
                        help=f"量化类型，默认 {DEFAULT_QUANT}（i8=INT8/fp=FP16/hybrid=混合）")
    parser.add_argument("output", nargs="?",
                        help="输出 .rknn 路径，默认输出到 onnx 同目录、同名 .rknn")
    args = parser.parse_args()

    do_quant = args.quant == "i8" or args.quant == "hybrid"
    output = args.output if args.output else args.onnx.rsplit(".", 1)[0] + ".rknn"
    return args.onnx, args.platform, do_quant, output


def main():
    onnx_path, platform, do_quant, output_path = parse_args()

    # 1. 创建 RKNN 对象（verbose=True 会打印转换详细日志，方便排查）。
    rknn = RKNN(verbose=True)

    print(f"--> 转换参数：onnx={onnx_path}, platform={platform}, "
          f"quant={'i8/hybrid' if do_quant else 'fp'}, output={output_path}")

    # 2. 配置：归一化 + 目标平台 + 量化算法。
    #    mean/std 是前处理的归一化参数，必须和训练/推理时一致（YOLOv5 固定 0/255）。
    #    quantized_algorithm="normal" 是默认快速量化；精度不满意可改 "mmse"。
    print("--> 配置模型")
    rknn.config(
        mean_values=MEAN_VALUES,
        std_values=STD_VALUES,
        target_platform=platform,
        # quantized_algorithm="normal",   # 可选：mmse 更准但慢
        # optimization_level=3,           # 可选：0~3，越高优化越激进
    )
    print("    配置完成")

    # 3. 加载 ONNX 模型。
    print("--> 加载 ONNX")
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        print(f"    加载失败！错误码 {ret}")
        sys.exit(ret)
    print("    加载完成")

    # 4. 构建（关键一步：这里决定是否量化 + 用什么校准集）。
    #    do_quantization=True  → INT8 量化，用 dataset 做校准；
    #    do_quantization=False → FP16，不需要 dataset。
    print("--> 构建模型" + ("（INT8 量化，用校准集）" if do_quant else "（FP16）"))
    ret = rknn.build(do_quantization=do_quant, dataset=DATASET_PATH)
    if ret != 0:
        print(f"    构建失败！错误码 {ret}")
        sys.exit(ret)
    print("    构建完成")

    # 5. 导出 .rknn 模型。
    print("--> 导出 RKNN")
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        print(f"    导出失败！错误码 {ret}")
        sys.exit(ret)
    print(f"    导出完成 → {output_path}")

    # 6. 释放资源。
    rknn.release()
    print("--> 转换结束")


if __name__ == "__main__":
    main()
