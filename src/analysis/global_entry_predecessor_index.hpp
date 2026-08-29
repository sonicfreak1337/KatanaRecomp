#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>

namespace katana::analysis::detail {

struct GlobalEntryEdge final {
    std::uint32_t source = 0u;
    std::uint32_t target = 0u;

    [[nodiscard]] friend bool operator==(
        const GlobalEntryEdge&,
        const GlobalEntryEdge&) noexcept = default;
};

// Whole-program predecessor universe for locally bounded producer proofs.
// Counts preserve duplicate semantic carriers without allowing removal of one
// family to erase an identical edge still owned by another family.
class GlobalEntryPredecessorIndex final {
  public:
    void clear() noexcept {
        boundaries_.clear();
        predecessors_.clear();
    }

    void add_boundary(const std::uint32_t address) {
        boundaries_.insert(address);
    }

    void add_edge(const std::uint32_t source,
                  const std::uint32_t target) {
        ++predecessors_[target][source];
    }

    void remove_edge(const std::uint32_t source,
                     const std::uint32_t target) noexcept {
        const auto target_entry = predecessors_.find(target);
        if (target_entry == predecessors_.end()) return;
        const auto source_entry = target_entry->second.find(source);
        if (source_entry == target_entry->second.end()) return;
        if (--source_entry->second == 0u)
            target_entry->second.erase(source_entry);
        if (target_entry->second.empty())
            predecessors_.erase(target_entry);
    }

    template <typename Edge, std::size_t Extent>
    [[nodiscard]] bool closes(
        const std::uint32_t seed,
        const std::uint32_t load,
        const std::span<Edge, Extent> allowed_internal_edges,
        const GlobalEntryPredecessorIndex* const additional = nullptr) const {
        if (seed >= load) return false;
        const auto boundary = boundaries_.upper_bound(seed);
        if (boundary != boundaries_.end() && *boundary <= load)
            return false;
        if (!predecessors_close(seed, load, allowed_internal_edges))
            return false;
        return additional == nullptr ||
               additional->predecessors_close(
                   seed, load, allowed_internal_edges);
    }

    [[nodiscard]] friend bool operator==(
        const GlobalEntryPredecessorIndex&,
        const GlobalEntryPredecessorIndex&) = default;

  private:
    template <typename Edge, std::size_t Extent>
    [[nodiscard]] bool predecessors_close(
        const std::uint32_t seed,
        const std::uint32_t load,
        const std::span<Edge, Extent> allowed_internal_edges) const {
        for (auto target = predecessors_.upper_bound(seed);
             target != predecessors_.end() && target->first <= load;
             ++target) {
            for (const auto& [source, count] : target->second) {
                static_cast<void>(count);
                const auto allowed = std::find_if(
                    allowed_internal_edges.begin(),
                    allowed_internal_edges.end(),
                    [&](const auto& edge) {
                        return edge.source == source &&
                               edge.target == target->first;
                    });
                if (allowed == allowed_internal_edges.end()) return false;
            }
        }
        return true;
    }

    std::set<std::uint32_t> boundaries_;
    std::map<std::uint32_t,
             std::map<std::uint32_t, std::size_t>> predecessors_;
};

} // namespace katana::analysis::detail
