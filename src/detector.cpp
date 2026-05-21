/*
 * ==========================================================================
 * detector.cpp — YOLOv5 目标检测器（完整前处理 + 后处理流水线）
 * ==========================================================================
 *
 * **检测流程**：
 *   1. 前处理（preprocess）
 *   2. NPU 推理（rknn_->set_inputs → run → get_outputs）
 *   3. 后处理（INT8 反量化 + 解码边界框 + NMS 去重）
 *
 * **YOLOv5 模型结构（输出头）**：
 *   输入：640×640×3 RGB 图像
 *   输出：三个不同尺度（stride）的检测头
 *     Output[0] stride=8  → 80×80 网格 → 检测小目标
 *     Output[1] stride=16 → 40×40 网格 → 检测中目标
 *     Output[2] stride=32 → 20×20 网格 → 检测大目标
 *
 * **每个网格单元的预测内容（85 个值）**：
 *   [0:4]   — tx, ty, tw, th（边界框回归参数）
 *   [4]     — objectness（目标置信度，是否包含目标）
 *   [5:85]  — class probabilities（80 个 COCO 类别概率）
 *
 * **INT8 量化理解**：
 *   INT8 推理时 NPU 输出的是 int8 整数，需要反量化回 float32：
 *   float_val = (int8_val - zero_point) * scale
 *   例如：int8_val=50, zp=0, scale=0.004 → float_val=0.2
 *
 * **边界框解码公式**：
 *   bx = (sigmoid(tx) * 2 - 0.5 + grid_x) * stride
 *   by = (sigmoid(ty) * 2 - 0.5 + grid_y) * stride
 *   bw = (sigmoid(tw) * 2)^2 * anchor_w
 *   bh = (sigmoid(th) * 2)^2 * anchor_h
 *
 * **NMS（非极大值抑制）**：
 *   同一目标可能被多个重叠框检测到，NMS 保留置信度最高的框
 *   IoU（交并比）> nms_threshold 的框被抑制（丢弃）
 */

#include "detector.hpp"

extern "C" {
#include "logger.h"
}

#include <opencv2/imgproc.hpp>     /* cv::resize, cv::cvtColor        */
#include <fstream>                 /* std::ifstream（读取标签文件）   */
#include <cstring>                 /* strncpy                        */
#include <cmath>                   /* expf, powf, fmax, fmin        */

namespace rk3568_vision {

/* ── 常量定义 ──────────────────────────────────────────────────────────── */

static constexpr float OBJ_CLASS_NUM = 80.0f;   /* COCO 类别总数                  */
static constexpr int   OBJ_NUMB_MAX  = 64;      /* 单帧最大输出框数                */
static constexpr int   PROP_BOX_SIZE = 85;      /* 每个网格单元的预测值数量         */
                                                 /* (x,y,w,h,obj + 80 classes)    */

/* ── 工具函数 ──────────────────────────────────────────────────────────── */

/*
 * 将浮点坐标钳制（clamp）到 [min, max] 范围
 * 确保检测框不超出图像边界
 */
static int clamp_i(float val, int min, int max) {
    return (val > min) ? ((val < max) ? (int)val : max) : min;
}

/*
 * Sigmoid 激活函数
 * 将任意实数映射到 (0, 1) 区间
 * 用于 YOLOv5 的边界框参数和目标置信度
 *
 * 为什么 YOLOv5 输出需要 sigmoid？
 *   - tx, ty 的范围被约束到 [0, 1]，表示相对于网格单元的偏移
 *   - objectness 和 class prob 被转换为 [0, 1] 的概率
 */
static float sigmoid_f(float x) { return 1.0f / (1.0f + expf(-x)); }

/*
 * INT8 仿射反量化（Affine Dequantization）
 *
 * NPU 输出的是 int8 整数，需要转换回 float32 供后处理使用
 * 反量化公式：float_val = (int8_val - zero_point) * scale
 *
 * @qnt：int8 量化值
 * @zp：零点（zero point），INT8 量化的偏移量
 * @scale：缩放因子（由 RKNN Toolkit 量化校准得出）
 * 返回：float32 浮点值
 */
static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

/*
 * 计算两个检测框的 IoU（Intersection over Union，交并比）
 *
 * IoU 是目标检测中衡量两个框重叠程度的标准指标
 * IoU = 交集面积 / 并集面积
 *
 * @x1_a, y1_a, x2_a, y2_a：框 A 的左上角和右下角坐标
 * @x1_b, y1_b, x2_b, y2_b：框 B 的左上角和右下角坐标
 * 返回：IoU 值 [0, 1]，0 表示不相交，1 表示完全重叠
 */
static float calc_iou(float x1_a, float y1_a, float x2_a, float y2_a,
                      float x1_b, float y1_b, float x2_b, float y2_b) {
    /* 交集区域：左上角取两框的最大值，右下角取两框的最小值 */
    float inter_w = fmax(0.0f, fmin(x2_a, x2_b) - fmax(x1_a, x1_b) + 1.0f);
    float inter_h = fmax(0.0f, fmin(y2_a, y2_b) - fmax(y1_a, y1_b) + 1.0f);
    float inter   = inter_w * inter_h;

    /* 各框面积 */
    float area_a  = (x2_a - x1_a + 1.0f) * (y2_a - y1_a + 1.0f);
    float area_b  = (x2_b - x1_b + 1.0f) * (y2_b - y1_b + 1.0f);

    /* 并集面积 = 面积A + 面积B - 交集面积 */
    float uni = area_a + area_b - inter;
    return (uni <= 0.0f) ? 0.0f : inter / uni;
}


/* ==========================================================================
 *  Detector 实现
 * ========================================================================== */

Detector::Detector() = default; // 类内声明，类外实现，这里的实现是 = default 是函数体（让编译器生成默认实现）
Detector::~Detector() = default;  /* rknn_ 的 unique_ptr 自动释放 */

/*
 * 初始化检测器
 *
 * 流程：
 *   1. 设置置信度阈值和 NMS 阈值
 *   2. 加载 COCO 80 类标签（person, bicycle, car...）
 *   3. 创建并初始化 RknnContext（加载模型、查询属性）
 *
 * @model_path：.rknn 模型文件路径
 * @labels_path：类别标签文件路径（每行一个类别名）
 * @conf：置信度阈值（低于该值的检测结果被丢弃）
 * @nms：NMS IoU 阈值（高于该值的重叠框被抑制）
 * @npu_core：NPU 核心编号
 */
bool Detector::init(const std::string& model_path, const std::string& labels_path,
                    float conf, float nms, uint32_t npu_core) {
    conf_threshold_ = conf; nms_threshold_ = nms;

    // 成员函数 init 内部可以直接使用 labels_、conf_threshold_ 等成员变量，
    // 是因为每个成员函数都有一个隐藏的 this 指针，指向调用该函数的对象。

    /* 加载标签文件（COCO 80 类） */
    std::ifstream lf(labels_path);
    if (lf.is_open()) {
        std::string line;
        while (std::getline(lf, line))
            if (!line.empty()) labels_.push_back(line);
        LOG_INFO("labels loaded: %s (%zu classes)", labels_path.c_str(), labels_.size());
    }

    /* 创建 RKNN 上下文并初始化 */
    rknn_ = std::make_unique<RknnContext>();
    if (!rknn_->init(model_path, npu_core)) {
        LOG_ERROR("rknn init failed"); return false;
    }
    initialized_ = true;
    LOG_INFO("detector initialized: %ux%u", rknn_->input_width(), rknn_->input_height());
    return true;
}

/* 属性访问器：返回模型输入尺寸 */
uint32_t Detector::input_width()  const { return rknn_ ? rknn_->input_width()  : 640; }
uint32_t Detector::input_height() const { return rknn_ ? rknn_->input_height() : 640; }
uint32_t Detector::output_count() const { return rknn_ ? rknn_->output_count() : 3; }

/*
 * 图像预处理
 *
 * 将采集到的 BGR 图像转换为模型输入格式：
 *   1. BGR → RGB（YOLOv5 训练时使用 RGB 图像）
 *   2. resize 到模型输入尺寸（如 640×640）
 *
 * 为什么使用 cv::INTER_LINEAR 而非 cv::INTER_NEAREST？
 *   - 双线性插值的质量更好，目标边缘更平滑
 *   - 对于 resize 操作，双线性插值的额外计算开销很小
 *
 * 返回：预处理后的 cv::Mat（RGB, 模型输入尺寸）
 */
cv::Mat Detector::preprocess(const cv::Mat& bgr) {
    cv::Mat rgb, resized;
    /* BGR → RGB 颜色空间转换 */
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    uint32_t mw = rknn_->input_width(), mh = rknn_->input_height();
    /* 如果尺寸不匹配，缩放到模型输入尺寸 */
    if ((uint32_t)rgb.cols != mw || (uint32_t)rgb.rows != mh)
        cv::resize(rgb, resized, cv::Size(mw, mh), 0, 0, cv::INTER_LINEAR);
    else
        resized = rgb;
    return resized;
}

/*
 * 执行目标检测 — 核心函数
 *
 * 完整推理流程：
 *   【前处理】
 *   1. preprocess()：BGR→RGB + resize 到 640×640
 *
 *   【NPU 推理】
 *   2. rknn_->set_inputs()：将预处理后的图像传入 NPU
 *   3. rknn_->run()：执行 NPU 硬件推理
 *   4. rknn_->get_outputs()：获取三个输出头的 INT8 张量
 *
 *   【后处理】对三个输出头分别处理
 *   5. INT8 反量化 → float32
 *   6. 解码边界框坐标（sigmoid + 网格映射 + anchor）
 *   7. 计算综合置信度 = objectness × class_prob
 *   8. 过滤低于 confidence_threshold 的检测
 *   9. 将所有有效检测收集到 dets 列表
 *
 *   【NMS】
 *   10. 按类别分组执行 NMS 去重
 *   11. 保留置信度最高的框，抑制 IoU 过高的重叠框
 *
 *   【输出】
 *   12. 坐标从模型空间映射回原始图像空间
 *   13. 限制框坐标在图像边界内（clamp）
 *   14. 填充类别标签字符串
 *
 * @bgr：输入图像（原始 BGR 格式，来自采集/转换）
 * 返回：detect_result_t（检测结果）
 */
DetectResult Detector::detect(const cv::Mat& bgr) {
    DetectResult result{};
    if (!initialized_ || !rknn_) return result;

    /* ── 前处理 ───────────────────────────────────────────────── */
    cv::Mat input = preprocess(bgr);
    int img_w = bgr.cols, img_h = bgr.rows;
    int mw = rknn_->input_width(), mh = rknn_->input_height();
    float scale_x = (float)img_w / (float)mw;
    /*
     * 计算缩放系数
     * 用于将检测框坐标从模型空间映射回原始图像空间
     * scale = min(mw/img_w, mh/img_h) 保证等比例缩放
     */
    float scale_y = (float)img_h / (float)mh;

    /*
     * 设置 NPU 输入
     * type=UINT8：输入数据类型为 8-bit 无符号整数
     * fmt=NCHW：通道优先布局（Channel, Height, Width）
     * size：输入数据总字节数 = 640×640×3 = 1,228,800 字节
     */
    rknn_input rknn_in[1];
    memset(rknn_in, 0, sizeof(rknn_in));
    rknn_in[0].index = 0;
    rknn_in[0].type  = RKNN_TENSOR_UINT8;
    rknn_in[0].size  = mw * mh * 3;
    rknn_in[0].fmt   = RKNN_TENSOR_NHWC;
    rknn_in[0].buf   = input.data;      /* cv::Mat 的内部数据指针 */
    if (!rknn_->set_inputs(rknn_in, 1)) return result;

    /* ── NPU 硬件推理 ──────────────────────────────────────────── */
    /* 这一行是性能关键路径：NPU 推理 ~25ms，CPU 在此期间可处理其他任务 */
    if (!rknn_->run()) return result;

    /*
     * 获取 NPU 输出
     * 三个输出张量在 NPU 侧的 DMA 内存中
     * want_float=0：要求返回 INT8 原始数据（我们在 CPU 上做反量化）
     */
    std::vector<rknn_output> outputs(rknn_->output_count());
    memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
    for (uint32_t i = 0; i < rknn_->output_count(); i++)
        outputs[i].want_float = 0;  /* 获取 INT8 原始数据 */
    if (!rknn_->get_outputs(outputs.data(), rknn_->output_count())) return result;

    /*
     * ── 后处理：解码三个输出头的检测结果 ─────────────────────────
     *
     * 三个输出头对应三个不同尺度（stride）：
     *   strides[0]=8  → 80×80 网格 → 检测小目标（person 的远处小人）
     *   strides[1]=16 → 40×40 网格 → 检测中目标（car 的正常尺寸）
     *   strides[2]=32 → 20×20 网格 → 检测大目标（truck 的近景）
     *
     * 每个网格单元有 3 个 anchor（预设锚框），用于检测不同宽高比的目标
     * 小尺度 anchor 适合瘦高目标（如 person），大尺度适合宽扁目标（如 car）
     */
    struct Detection { float x, y, w, h, conf; int cls; };
    std::vector<Detection> dets;
    const int strides[3] = {8, 16, 32};  /* 三个输出头的下采样倍率 */
    const int anchors[3][6] = {
        {10,13, 16,30, 33,23},     // stride 8  (small objects)
        {30,61, 62,45, 59,119},    // stride 16 (medium objects)
        {116,90, 156,198, 373,326} // stride 32 (large objects)
    };

    for (uint32_t o = 0; o < rknn_->output_count() && o < 3; o++) {
        auto& attr = rknn_->output_attr(o);
        int stride  = strides[o];             /* 该输出头的下采样倍率 */
        int gh = mh / stride, gw = mw / stride; /* 该尺度的网格分辨率 */
        float qscale = (float)attr.scale;     /* INT8 量化 scale */
        int32_t qzp  = attr.zp;               /* INT8 量化 zero point */
        int8_t* data = (int8_t*)outputs[o].buf; /* NPU 输出 INT8 数据 */
        int glen = gh * gw;                   /* 该尺度的网格点总数 */

        /* 遍历 3 个 anchor */
        for (int a = 0; a < 3; a++) {
            /* 遍历每个网格单元 */
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    /* 计算当前网格单元在 INT8 数据数组中的基偏移 */
                    int off = (PROP_BOX_SIZE * a) * glen + gy * gw + gx;

                    /*
                     * 第一步：检查 objectness 置信度
                     * data[off + 4*glen] 是当前网格单元的 objectness 值
                     * 先反量化 INT8→float，再经 sigmoid 转换为概率
                     * 如果 objectness 低于阈值，跳过该网格单元（快速剪枝）
                     */
                    int8_t obj_conf_i8 = data[off + 4 * glen];
                    float obj_conf = sigmoid_f(deqnt_affine_to_f32(obj_conf_i8, qzp, qscale));
                    if (obj_conf < conf_threshold_) continue;  /* 快速剪枝：节省后续计算 */

                    /*
                     * 第二步：找出 80 个类别中置信度最高的类别
                     * data[off + 5*glen] 到 data[off + 84*glen] 是 80 个类别概率
                     * 遍历所有类别找出最大值及其索引
                     */
                    int8_t max_cls_i8 = data[off + 5 * glen];
                    int max_cls = 0;
                    for (int c = 1; c < (int)OBJ_CLASS_NUM; c++) {
                        int8_t p = data[off + (5 + c) * glen];
                        if (p > max_cls_i8) { max_cls_i8 = p; max_cls = c; }
                    }
                    /* 反量化最大类别概率 → sigmoid → 综合置信度 = obj_conf × cls_prob */
                    float cls_prob = sigmoid_f(deqnt_affine_to_f32(max_cls_i8, qzp, qscale));
                    float score = obj_conf * cls_prob;
                    if (score < conf_threshold_) continue;  /* 综合置信度低于阈值则丢弃 */

                    /*
                     * 第三步：解码边界框坐标
                     *
                     * YOLOv5 边界框解码公式：
                     *   bx = (sigmoid(tx) * 2 - 0.5 + gx) * stride
                     *   by = (sigmoid(ty) * 2 - 0.5 + gy) * stride
                     *   bw = (sigmoid(tw) * 2)^2 * anchor_w(a, stride)
                     *   bh = (sigmoid(th) * 2)^2 * anchor_h(a, stride)
                     *
                     * 其中：
                     *   - sigmoid(tx)*2-0.5 将 tx 映射到 [-0.5, 1.5]，允许检测框中心在网格外
                     *   - 加 gx 是当前网格单元的列索引
                     *   - 乘 stride 映射回模型输入空间坐标
                     *   - anchor_w/h 根据尺度（小/中/大）和 anchor 编号有不同的预设值
                     *   - 尺度0（stride=8）：width anchors [10, 16, 30], height [13, 30, 62]
                     *   - 尺度1（stride=16）：略
                     *   - 尺度2（stride=32）：略
                     */

                    /* 反量化 + sigmoid 转换四个回归参数 */
                    float bx = (sigmoid_f(deqnt_affine_to_f32(data[off], qzp, qscale)) * 2.0f - 0.5f + gx) * stride;
                    float by = (sigmoid_f(deqnt_affine_to_f32(data[off + 1*glen], qzp, qscale)) * 2.0f - 0.5f + gy) * stride;
                    float bw = powf(sigmoid_f(deqnt_affine_to_f32(data[off + 2*glen], qzp, qscale)) * 2.0f, 2) * (float)anchors[o][2*a];
                    float bh = powf(sigmoid_f(deqnt_affine_to_f32(data[off + 3*glen], qzp, qscale)) * 2.0f, 2) * (float)anchors[o][2*a+1];

                    dets.push_back({(bx - bw*0.5f) * scale_x, (by - bh*0.5f) * scale_y,
                                    bw * scale_x, bh * scale_y,
                                    score, max_cls});
                }
            }
        }
    }

    /* 释放 NPU 输出缓冲区（重要：防止 NPU 内存泄漏） */
    rknn_->release_outputs(outputs.data(), rknn_->output_count());

    /*
     * ── NMS（Non-Maximum Suppression，非极大值抑制）───
     *
     * 同一目标可能被不同 anchor / 不同网格单元多次检测到
     * NMS 按类别分组，对每组内重叠度高的框只保留置信度最高的一个
     *
     * 算法：
     *   1. 按类别分组
     *   2. 对每组按置信度降序排列
     *   3. 从最高置信度开始，计算与后续框的 IoU
     *   4. IoU > nms_threshold → 抑制（keep[j] = 0）
     *
     * 这是一个 O(n²) 算法（n=候选框数量，通常 < 100）
     * 对于实时应用足够高效
     */
    std::vector<int> keep(dets.size(), 1);  /* 1=保留, 0=抑制 */
    for (size_t i = 0; i < dets.size(); i++) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!keep[j] || dets[i].cls != dets[j].cls) continue;
            if (calc_iou(dets[i].x, dets[i].y, dets[i].x+dets[i].w, dets[i].y+dets[i].h,
                         dets[j].x, dets[j].y, dets[j].x+dets[j].w, dets[j].y+dets[j].h) > nms_threshold_)
                keep[j] = 0;
        }
    }

    /*
     * ── 输出检测结果 ─────────────────────────────────────────────
     *
     * 将 NMS 后的有效检测框填充到 result
     * - 坐标钳制到图像范围内（clamp）
     * - 填充类别标签字符串（如 "person", "car"）
     * - 限制最大输出框数（OBJ_NUMB_MAX=64）
     */
    for (size_t i = 0; i < dets.size() && result.count < OBJ_NUMB_MAX; i++) {
        if (!keep[i]) continue;
        auto& d = dets[i];
        auto& b = result.boxes[result.count];

        /* 钳制坐标到图像边界内 */
        b.x = clamp_i(d.x, 0, img_w);
        b.y = clamp_i(d.y, 0, img_h);
        b.w  = clamp_i(d.x+d.w, 0, img_w) - b.x;    /* 宽度也要钳制 */
        b.h = clamp_i(d.y+d.h, 0, img_h) - b.y;

        b.conf = d.conf;
        b.class_id = d.cls;

        /* 填充类别标签（如 "person", "bicycle", "car"...） */
        if (d.cls >= 0 && (size_t)d.cls < labels_.size())
            strncpy(b.label, labels_[d.cls].c_str(), sizeof(b.label)-1);
        else
            snprintf(b.label, sizeof(b.label), "cls_%d", d.cls);
        result.count++;
    }
    return result;
}

} // namespace rk3568_vision
