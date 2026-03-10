#pragma once

#include "pictor/core/types.h"

namespace pictor {

/// Classifies objects into Static/Dynamic/GPU Driven pools at registration time (§2.2).
/// Classification is done once at registration; no per-frame flag checking.
class ObjectClassifier {
public:
    struct ClassificationResult {
        PoolType pool_type;
        uint16_t adjusted_flags;
    };

    /// Classify an object based on its descriptor flags
    static ClassificationResult classify(const ObjectDescriptor& desc);

    /// GPU Driven eligibility check (§7.4)
    struct GpuDrivenCriteria {
        uint32_t max_triangle_count        = 50000;
        uint32_t min_instance_count        = 32;
        bool     require_indirect_support  = true;
    };

    static bool is_gpu_driven_eligible(const ObjectDescriptor& desc);
    static bool is_gpu_driven_eligible(const ObjectDescriptor& desc,
                                       const GpuDrivenCriteria& criteria);
};

} // namespace pictor
