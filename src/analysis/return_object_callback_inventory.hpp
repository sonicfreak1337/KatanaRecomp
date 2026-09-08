#pragma once

#include "katana/codegen/latent_aot_registry.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <span>
#include <vector>

namespace katana::analysis::detail {

class GuardedNativeEntryShapeCache;

// Finds only the bounded, straight-line diagnostic shape in which a direct
// call's r0 return is copied through GPRs and a PC-relative literal is stored
// to a displacement field.  The result is deliberately never an execution or
// closure input; callers must not feed it into any root/seed collection.
[[nodiscard]] std::vector<
    katana::codegen::LatentAotReturnObjectCallbackCandidate>
discover_return_object_callback_candidates(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> decoded_lines,
    std::span<const katana::ir::Function> program,
    GuardedNativeEntryShapeCache& native_entry_shapes);

} // namespace katana::analysis::detail
