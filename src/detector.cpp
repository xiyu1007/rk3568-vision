#include "detector.hpp"
#include "logger.hpp"

#include <opencv2/imgproc.hpp>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace rk3568_vision {

static constexpr int OBJ_CLASS_NUM  = 80;
static constexpr int OBJ_NUMB_MAX   = 64;
static constexpr int PROP_BOX_SIZE  = 85;
static constexpr int NMS_THRESH     = 0.45f;
static constexpr int BOX_THRESH     = 0.25f;

static int clamp_i(float val, int min, int max) {
    return (val > min) ? ((val < max) ? (int)val : max) : min;
}

static float sigmoid_f(float x) { return 1.0f / (1.0f + expf(-x)); }

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

// 计算两个框的 IoU
static float calc_iou(float x1_a, float y1_a, float x2_a, float y2_a,
                      float x1_b, float y1_b, float x2_b, float y2_b) {
    float inter_w = fmax(0.0f, fmin(x2_a, x2_b) - fmax(x1_a, x1_b) + 1.0f);
    float inter_h = fmax(0.0f, fmin(y2_a, y2_b) - fmax(y1_a, y1_b) + 1.0f);
    float inter   = inter_w * inter_h;
    float area_a  = (x2_a - x1_a + 1.0f) * (y2_a - y1_a + 1.0f);
    float area_b  = (x2_b - x1_b + 1.0f) * (y2_b - y1_b + 1.0f);
    float union_area = area_a + area_b - inter;
    return (union_area <= 0.0f) ? 0.0f : (inter / union_area);
}

Detector::Detector()  = default;
Detector::~Detector() = default;

bool Detector::init(const std::string& model_path, const std::string& labels_path,
                    float conf_threshold, float nms_threshold, uint32_t npu_core) {
    conf_threshold_ = conf_threshold;
    nms_threshold_  = nms_threshold;

    // 加载标签文件
    std::ifstream labels_file(labels_path);
    if (labels_file.is_open()) {
        std::string line;
        while (std::getline(labels_file, line)) {
            if (!line.empty()) labels_.push_back(line);
        }
        LOG_INFO("Labels loaded: %s (%zu classes)", labels_path.c_str(), labels_.size());
    }

    // 初始化 RKNN
    rknn_ = std::make_unique<RknnContext>();
    if (!rknn_->init(model_path, npu_core)) {
        LOG_ERROR("RKNN initialization failed");
        return false;
    }

    initialized_ = true;
    LOG_INFO("Detector initialized: %ux%u@%.2f/%.2f",
             rknn_->input_width(), rknn_->input_height(),
             conf_threshold_, nms_threshold_);
    return true;
}

uint32_t Detector::input_width()  const { return rknn_ ? rknn_->input_width()  : 640; }
uint32_t Detector::input_height() const { return rknn_ ? rknn_->input_height() : 640; }
uint32_t Detector::output_count() const { return rknn_ ? rknn_->output_count() : 3; }

cv::Mat Detector::preprocess(const cv::Mat& bgr) {
    cv::Mat rgb, resized;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

    uint32_t mw = rknn_->input_width();
    uint32_t mh = rknn_->input_height();

    if ((uint32_t)rgb.cols != mw || (uint32_t)rgb.rows != mh) {
        cv::resize(rgb, resized, cv::Size(mw, mh), 0, 0, cv::INTER_LINEAR);
    } else {
        resized = rgb;
    }
    return resized;
}

DetectResult Detector::detect(const cv::Mat& bgr) {
    DetectResult result{};
    if (!initialized_ || !rknn_) return result;

    cv::Mat input = preprocess(bgr);

    int img_w = bgr.cols, img_h = bgr.rows;
    int mw = rknn_->input_width(), mh = rknn_->input_height();
    float scale_w = (float)mw / img_w;
    float scale_h = (float)mh / img_h;
    float scale   = std::min(scale_w, scale_h);

    rknn_input rknn_in[1];
    memset(rknn_in, 0, sizeof(rknn_in));
    rknn_in[0].index        = 0;
    rknn_in[0].type         = RKNN_TENSOR_UINT8;
    rknn_in[0].size         = mw * mh * 3;
    rknn_in[0].fmt          = RKNN_TENSOR_NHWC;
    rknn_in[0].buf          = input.data;

    if (!rknn_->set_inputs(rknn_in, 1)) return result;
    if (!rknn_->run())               return result;

    std::vector<rknn_output> outputs(rknn_->output_count());
    memset(outputs.data(), 0, outputs.size() * sizeof(rknn_output));
    for (uint32_t i = 0; i < rknn_->output_count(); ++i)
        outputs[i].want_float = 0;

    if (!rknn_->get_outputs(outputs.data(), rknn_->output_count())) return result;

    // 后处理：遍历三个输出层
    struct Detection { float x, y, w, h; float conf; int cls; };
    std::vector<Detection> dets;

    const int strides[3] = {8, 16, 32};
    for (uint32_t o = 0; o < rknn_->output_count() && o < 3; ++o) {
        auto& attr = rknn_->output_attr(o);
        int stride = strides[o];
        int grid_h = mh / stride;
        int grid_w = mw / stride;
        float qscale = attr.scale;
        int32_t qzp  = attr.zp;

        int8_t* data = static_cast<int8_t*>(outputs[o].buf);
        int grid_len = grid_h * grid_w;

        for (int a = 0; a < 3; ++a) {
            for (int gy = 0; gy < grid_h; ++gy) {
                for (int gx = 0; gx < grid_w; ++gx) {
                    int offset = (PROP_BOX_SIZE * a) * grid_len + gy * grid_w + gx;
                    int8_t obj_conf_i8 = data[offset + 4 * grid_len];
                    float obj_conf = deqnt_affine_to_f32(obj_conf_i8, qzp, qscale);
                    if (obj_conf < conf_threshold_) continue;

                    // 找最大类别概率
                    int8_t max_cls_i8 = data[offset + 5 * grid_len];
                    int    max_cls    = 0;
                    for (int c = 1; c < OBJ_CLASS_NUM; ++c) {
                        int8_t p = data[offset + (5 + c) * grid_len];
                        if (p > max_cls_i8) { max_cls_i8 = p; max_cls = c; }
                    }
                    float cls_prob = deqnt_affine_to_f32(max_cls_i8, qzp, qscale);
                    float score = obj_conf * cls_prob;
                    if (score < conf_threshold_) continue;

                    float bx = (sigmoid_f(deqnt_affine_to_f32(data[offset + 0 * grid_len], qzp, qscale)) * 2.0f - 0.5f + gx) * stride;
                    float by = (sigmoid_f(deqnt_affine_to_f32(data[offset + 1 * grid_len], qzp, qscale)) * 2.0f - 0.5f + gy) * stride;
                    float bw = powf(sigmoid_f(deqnt_affine_to_f32(data[offset + 2 * grid_len], qzp, qscale)) * 2.0f, 2) * (float)((a == 0) ? 10 : (a == 1) ? 16 : 30);
                    float bh = powf(sigmoid_f(deqnt_affine_to_f32(data[offset + 3 * grid_len], qzp, qscale)) * 2.0f, 2) * (float)((a == 0) ? 13 : (a == 1) ? 30 : 62);

                    float x1 = (bx - bw * 0.5f) / scale;
                    float y1 = (by - bh * 0.5f) / scale;
                    float x2 = (bx + bw * 0.5f) / scale;
                    float y2 = (by + bh * 0.5f) / scale;

                    dets.push_back({x1, y1, x2 - x1, y2 - y1, score, max_cls});
                }
            }
        }
    }

    rknn_->release_outputs(outputs.data(), rknn_->output_count());

    // NMS (按类别)
    std::vector<int> keep(dets.size(), 1);
    for (size_t i = 0; i < dets.size(); ++i) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (!keep[j] || dets[i].cls != dets[j].cls) continue;
            float iou = calc_iou(dets[i].x, dets[i].y, dets[i].x + dets[i].w, dets[i].y + dets[i].h,
                                 dets[j].x, dets[j].y, dets[j].x + dets[j].w, dets[j].y + dets[j].h);
            if (iou > nms_threshold_) keep[j] = 0;
        }
    }

    for (size_t i = 0; i < dets.size() && result.count < OBJ_NUMB_MAX; ++i) {
        if (!keep[i]) continue;
        auto& d = dets[i];
        auto& box = result.boxes[result.count];
        box.x = clamp_i(d.x, 0, img_w);
        box.y = clamp_i(d.y, 0, img_h);
        box.width  = clamp_i(d.x + d.w, 0, img_w) - box.x;
        box.height = clamp_i(d.y + d.h, 0, img_h) - box.y;
        box.confidence = d.conf;
        box.class_id   = d.cls;
        if (d.cls >= 0 && (size_t)d.cls < labels_.size())
            strncpy(box.label, labels_[d.cls].c_str(), sizeof(box.label) - 1);
        else
            snprintf(box.label, sizeof(box.label), "cls_%d", d.cls);
        result.count++;
    }

    return result;
}

} // namespace rk3568_vision
