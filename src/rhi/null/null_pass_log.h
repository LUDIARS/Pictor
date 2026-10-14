#pragma once

#include "pictor/gfx/pass_command.h"
#include "pictor/gfx/pass_context.h"
#include "pictor/gfx/pass_recorder.h"

#include <cstdint>
#include <vector>

namespace pictor::rhi {

class NullResourceTable;

/// One submitted pass: its context and its range in NullPassLog::commands().
struct NullRecordedPass {
    gfx::PassContext context;
    uint32_t         first_command      = 0;
    uint32_t         command_count      = 0;
    uint32_t         stale_handle_count = 0;  ///< commands naming a dead / never-issued resource
};

/// What the null backend "executes": every submitted pass's commands copied
/// into one flat array, in submission order, with handle liveness checked
/// against the resource table at submit time.
class NullPassLog {
public:
    void clear() noexcept {
        commands_.clear();
        passes_.clear();
    }

    /// Copies the recorder's commands and returns the stored pass entry.
    const NullRecordedPass& append(const gfx::PassRecorder& recorder,
                                   const gfx::PassContext& context,
                                   const NullResourceTable& resources);

    const std::vector<gfx::PassCommand>&  commands() const noexcept { return commands_; }
    const std::vector<NullRecordedPass>&  passes() const noexcept { return passes_; }

private:
    std::vector<gfx::PassCommand> commands_;
    std::vector<NullRecordedPass> passes_;
};

} // namespace pictor::rhi
