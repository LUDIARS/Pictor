#pragma once

// Pictor — Vulkan-path umbrella (interop during the multi-RHI migration).
//
// Everything in "pictor/pictor.h" plus the API that still exposes Vulkan types
// or includes <vulkan/vulkan.h>: PictorRenderer, VulkanContext, compiled graph
// recording, GPU timers / profiler, host-driven post-process. Code that used
// to include "pictor/pictor.h" for these includes this header instead.
//
// This header is transitional (spec/architecture/multi-rhi.md §5, §6): the
// types below move behind the backend-neutral face (pictor/gfx/) in S1-S4 and
// consumers migrate off it by S9. tools/consumer_face/check_consumer_face.py
// (C4) counts its uses under demo/ tools/ tests/.

#include "pictor/pictor.h"

// Pipeline (compiled graph recording)
#include "pictor/pipeline/render_pass_scheduler.h"
#include "pictor/pipeline/compiled_path_driver.h"
#include "pictor/pipeline/compiled_batch_recorder.h"

// Profiler (GPU timers)
#include "pictor/profiler/gpu_timer.h"
#include "pictor/profiler/profiler.h"
#include "pictor/profiler/overlay_renderer.h"
#include "pictor/profiler/data_exporter.h"

// Post-process pipeline (host-driven, real Vulkan)
#ifdef PICTOR_HAS_POSTPROCESS
#include "pictor/postprocess/postprocess_pipeline.h"
#endif

// Surface (Vulkan context)
#include "pictor/surface/vulkan_context.h"

// Public API
#include "pictor/core/pictor_renderer.h"
