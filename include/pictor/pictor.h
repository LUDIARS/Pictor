#pragma once

// Pictor — Rendering Pipeline Module v2.1
// Data-Driven, GPU-Optimized Rendering Architecture

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
#include "pictor/pipeline/render_pass_scheduler.h"
#include "pictor/pipeline/command_encoder.h"

// Profiler
#include "pictor/profiler/cpu_timer.h"
#include "pictor/profiler/gpu_timer.h"
#include "pictor/profiler/profiler.h"
#include "pictor/profiler/overlay_renderer.h"
#include "pictor/profiler/data_exporter.h"

// Material system
#include "pictor/material/material_property.h"
#include "pictor/material/base_material_builder.h"

// GI system
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_bake.h"

// Surface abstraction
#include "pictor/surface/surface_provider.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"

// Data handler
#include "pictor/data/texture_registry.h"
#include "pictor/data/vertex_data_uploader.h"
#include "pictor/data/data_handler.h"
#include "pictor/data/data_query_api.h"

// Text rendering
#include "pictor/text/text_types.h"
#include "pictor/text/font_loader.h"
#include "pictor/text/text_image_renderer.h"
#include "pictor/text/text_svg_renderer.h"
#include "pictor/text/text_rasterizer.h"

// Public API
#include "pictor/core/pictor_renderer.h"
