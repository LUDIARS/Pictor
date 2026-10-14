#pragma once

// Pictor backend-neutral public face (spec/architecture/multi-rhi.md §1).
// Nothing under pictor/gfx/ names a backend's types, functions, headers or
// macros; tools/consumer_face/check_consumer_face.py (C1) enforces that.

#include "pictor/gfx/backend_kind.h"
#include "pictor/gfx/backend_report.h"
#include "pictor/gfx/compare_op.h"
#include "pictor/gfx/device_caps.h"
#include "pictor/gfx/format.h"
#include "pictor/gfx/handle.h"
#include "pictor/gfx/load_store_op.h"
#include "pictor/gfx/pass_command.h"
#include "pictor/gfx/pass_context.h"
#include "pictor/gfx/pass_recorder.h"
#include "pictor/gfx/pipeline_desc.h"
#include "pictor/gfx/resource_desc.h"
#include "pictor/gfx/shader_bundle_ref.h"
