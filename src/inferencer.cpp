// ============================================================================
// inferencer.cpp — 推理器工厂
// ============================================================================

#include "inferencer.hpp"

#include "mock_inferencer.hpp"

#ifdef VISION_RK3568
#include "rknn_inferencer.hpp"
#endif

namespace vision {

std::unique_ptr<Inferencer> createInferencer(const Config& cfg) {
#ifdef VISION_RK3568
    // aarch64：默认走真实 RKNN，除非显式指定 mock。
    if (cfg.inference.backend != "mock") {
        return std::make_unique<RknnInferencer>(cfg);
    }
#else
    // x86：只有 mock 可用（避免未使用参数的告警）。
    (void)cfg;
#endif
    return std::make_unique<MockInferencer>(cfg);
}

} // namespace vision
