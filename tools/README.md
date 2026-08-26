# tools/ — 开发辅助工具

模型转换、调试等辅助脚本集中在此目录。

## 目录

| 文件                 | 说明                                                       |
| -------------------- | ---------------------------------------------------------- |
| `yolov5_to_rknn.py`  | YOLOv5 ONNX → RKNN 模型转换脚本（量化/调参/轻量化注释齐全） |

## 转换模型：YOLOv5 ONNX → RKNN

环境：rknn-toolkit2 >= 2.3.2（装在 ubuntu 虚拟机，Python 3.8~3.12 均可）。

```bash
cd tools
python3 yolov5_to_rknn.py <onnx路径> [平台] [量化类型] [输出rknn路径]

# 示例：三种模型分别转（转完放到 model/）
python3 yolov5_to_rknn.py yolov5n.onnx      rk3568 i8 ../model/yolov5n.rknn
python3 yolov5_to_rknn.py yolov5s.onnx      rk3568 i8 ../model/yolov5s.rknn
python3 yolov5_to_rknn.py yolov5s_relu.onnx rk3568 i8 ../model/yolov5s_relu.rknn
```

> 量化校准集 `datasets/COCO/coco_subset_20.txt` 需先准备（rknn_model_zoo 里有 20 张
> COCO 图），路径在脚本顶部 `DATASET_PATH` 里改。

## 三个核心概念速查

| 概念 | 一句话 | 关键旋钮 |
| ---- | ------ | -------- |
| **量化** | 把 FP32 权重压成 INT8，体积≈1/4、NPU 快 2~4 倍，精度掉 0.5~2% | `do_quantization` + 校准集 `dataset` |
| **调参** | 转换时能调的旋钮，直接影响精度和速度 | mean/std、校准集大小、量化算法、优化等级、平台 |
| **轻量化** | 换更小的模型 / 更省算力的激活 | 模型尺寸（n<s<m）、relu 替代 silu、量化 |

## 本项目三种模型对照

| 模型 | 尺寸 | 激活 | 推理耗时(RK3568 实测) | `inference.use_sigmoid` |
| ---- | ---- | ---- | --------------------- | ----------------------- |
| `yolov5n.rknn`      | nano (1.9M)  | relu  | ~25ms | `false`（输出已是 sigmoid 后值） |
| `yolov5s_relu.rknn` | small (7.2M) | relu  | ~40ms | `false`（输出已是 sigmoid 后值） |
| `yolov5s.rknn`      | small (7.2M) | silu  | ~70ms | `true`（输出是 logits） |

> 换模型务必同步改 `conf/*.yaml` 的 `inference.use_sigmoid`，否则阈值失效、
> 候选框暴增、后处理慢到秒级。
