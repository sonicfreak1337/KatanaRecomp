#pragma once

#include "katana/analysis/hardware_audit.hpp"

namespace katana::cli {

[[nodiscard]] inline bool
hardware_audit_has_definite_gap(const analysis::DreamcastHardwareAudit& audit) noexcept {
    return audit.known_gap_addresses != 0u || audit.rejected_addresses != 0u ||
           audit.unmapped_addresses != 0u;
}

[[nodiscard]] inline bool
hardware_audit_failed(const analysis::DreamcastHardwareAudit& audit,
                      const bool fail_on_gap,
                      const bool strict) noexcept {
    const auto definite_gap = hardware_audit_has_definite_gap(audit);
    return (fail_on_gap && definite_gap) ||
           (strict && (definite_gap || audit.partial_addresses != 0u ||
                       audit.unresolved_poll_guard_loops != 0u));
}

} // namespace katana::cli
