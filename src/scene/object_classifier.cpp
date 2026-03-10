#include "pictor/scene/object_classifier.h"

namespace pictor {

ObjectClassifier::ClassificationResult
ObjectClassifier::classify(const ObjectDescriptor& desc) {
    ClassificationResult result;
    result.adjusted_flags = desc.flags;

    // Classification logic (§2.2, §4.1):
    // GPU_DRIVEN flag takes highest priority
    if (desc.flags & ObjectFlags::GPU_DRIVEN) {
        result.pool_type = PoolType::GPU_DRIVEN;
        // Ensure the pool flag bits are consistent
        result.adjusted_flags |= ObjectFlags::GPU_DRIVEN;
        result.adjusted_flags &= ~(ObjectFlags::STATIC | ObjectFlags::DYNAMIC);
    }
    else if (desc.flags & ObjectFlags::STATIC) {
        result.pool_type = PoolType::STATIC;
        result.adjusted_flags |= ObjectFlags::STATIC;
        result.adjusted_flags &= ~(ObjectFlags::DYNAMIC | ObjectFlags::GPU_DRIVEN);
    }
    else {
        // Default to Dynamic
        result.pool_type = PoolType::DYNAMIC;
        result.adjusted_flags |= ObjectFlags::DYNAMIC;
        result.adjusted_flags &= ~(ObjectFlags::STATIC | ObjectFlags::GPU_DRIVEN);
    }

    return result;
}

bool ObjectClassifier::is_gpu_driven_eligible(const ObjectDescriptor& desc) {
    return is_gpu_driven_eligible(desc, GpuDrivenCriteria{});
}

bool ObjectClassifier::is_gpu_driven_eligible(const ObjectDescriptor& desc,
                                               const GpuDrivenCriteria& criteria) {
    // §7.4: GPU Driven eligibility criteria
    // Triangle count check would require mesh data lookup;
    // for now, check flag compatibility
    if (desc.flags & ObjectFlags::TRANSPARENT) {
        return false; // Transparent objects use sorted CPU-side rendering
    }

    if (desc.flags & ObjectFlags::INSTANCED) {
        return true; // Instanced objects benefit from GPU Driven
    }

    return true;
}

} // namespace pictor
