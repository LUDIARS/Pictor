#pragma once

// Pictor — Rendering Pipeline Module v2.1
// Data-Driven, GPU-Optimized Rendering Architecture
//
// Backend-neutral umbrella: nothing reachable from this header includes a GPU
// API header (spec/architecture/multi-rhi.md §6 S0). The Vulkan-path API
// (PictorRenderer, VulkanContext, compiled graph recording, GPU timers,
// host-driven post-process) moved to "pictor/pictor_vulkan.h", which includes
// this header first.

// Backend-neutral rendering face
#include "pictor/gfx/gfx.h"

// Core types
#include "pictor/core/types.h"

// Memory subsystem
#include "pictor/memory/frame_allocator.h"
#include "pictor/memory/pool_allocator.h"
#include "pictor/memory/gpu_memory_allocator.h"
#include "pictor/memory/memory_subsystem.h"

// Scene management
#include "pictor/scene/soa_stream.h"
#include "pictor/scene/object_pool.h"
#include "pictor/scene/object_classifier.h"
#include "pictor/scene/scene_registry.h"

// Data update
#include "pictor/update/job_dispatcher.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/update/single_thread_dispatcher.h"

// Batching & sorting
#include "pictor/batch/radix_sort.h"
#include "pictor/batch/batch_builder.h"

// Culling
#include "pictor/culling/flat_bvh.h"
#include "pictor/culling/culling_system.h"

// GPU driven
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/gpu/gpu_driven_pipeline.h"

// Pipeline
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/pipeline_builder.h"

// Profiler (CPU side; GPU timers are in pictor_vulkan.h)
#include "pictor/profiler/cpu_timer.h"

// Material system
#include "pictor/material/material_property.h"
#include "pictor/material/base_material_builder.h"

// GI system
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_bake.h"

// Post-process effect descriptions (the host-driven pipeline is in pictor_vulkan.h)
#include "pictor/postprocess/postprocess_effect.h"

// Surface abstraction
#include "pictor/surface/surface_provider.h"
#include "pictor/surface/glfw_surface_provider.h"

// Data handler
#include "pictor/data/texture_registry.h"
#include "pictor/data/vertex_data_uploader.h"
#include "pictor/data/model_data_types.h"
#include "pictor/data/model_data_handler.h"
#include "pictor/data/data_handler.h"
#include "pictor/data/data_query_api.h"

// Text rendering
#include "pictor/text/text_types.h"
#include "pictor/text/font_loader.h"
#include "pictor/text/text_image_renderer.h"
#include "pictor/text/text_svg_renderer.h"
#include "pictor/text/text_rasterizer.h"

// Animation system
#include "pictor/animation/animation_types.h"
#include "pictor/animation/animation_clip.h"
#include "pictor/animation/skeleton.h"
#include "pictor/animation/ik_solver.h"
#include "pictor/animation/motion_estimator.h"
#include "pictor/animation/fbx_importer.h"
#include "pictor/animation/bvh_importer.h"
#include "pictor/animation/animation_2d.h"
#include "pictor/animation/rive_animation.h"
#include "pictor/animation/lottie_animation.h"
#include "pictor/animation/vector_animation.h"
#include "pictor/animation/montage.h"
#include "pictor/animation/animation_system.h"
