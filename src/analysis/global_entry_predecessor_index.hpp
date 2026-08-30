#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <utility>

namespace katana::analysis::detail {

struct GlobalEntryEdge final {
    std::uint32_t source = 0u;
    std::uint32_t target = 0u;

    [[nodiscard]] friend bool operator==(
        const GlobalEntryEdge&,
        const GlobalEntryEdge&) noexcept = default;
};

struct GlobalEntryPredecessorDelta final {
    std::set<std::uint32_t> changed_boundaries;
    std::set<std::uint32_t> changed_predecessor_targets;

    [[nodiscard]] bool empty() const noexcept {
        return changed_boundaries.empty() &&
               changed_predecessor_targets.empty();
    }

    [[nodiscard]] bool affects_interval(
        const std::uint32_t seed,
        const std::uint32_t load) const noexcept {
        if (seed >= load) return true;
        const auto boundary = changed_boundaries.upper_bound(seed);
        if (boundary != changed_boundaries.end() && *boundary <= load)
            return true;
        const auto predecessor =
            changed_predecessor_targets.upper_bound(seed);
        return predecessor != changed_predecessor_targets.end() &&
               *predecessor <= load;
    }

    void merge(const GlobalEntryPredecessorDelta& other) {
        changed_boundaries.insert(other.changed_boundaries.begin(),
                                  other.changed_boundaries.end());
        changed_predecessor_targets.insert(
            other.changed_predecessor_targets.begin(),
            other.changed_predecessor_targets.end());
    }
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
        ++boundaries_[address];
    }

    void remove_boundary(const std::uint32_t address) noexcept {
        const auto found = boundaries_.find(address);
        if (found == boundaries_.end()) return;
        if (--found->second == 0u) boundaries_.erase(found);
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

    [[nodiscard]] GlobalEntryPredecessorDelta difference(
        const GlobalEntryPredecessorIndex& next) const {
        GlobalEntryPredecessorDelta delta;
        const auto collect_membership_changes = [](
            const auto& previous,
            const auto& current,
            auto&& changed) {
            auto left = previous.begin();
            auto right = current.begin();
            while (left != previous.end() || right != current.end()) {
                if (right == current.end() ||
                    (left != previous.end() && left->first < right->first)) {
                    changed(left->first);
                    ++left;
                } else if (left == previous.end() ||
                           right->first < left->first) {
                    changed(right->first);
                    ++right;
                } else {
                    ++left;
                    ++right;
                }
            }
        };
        collect_membership_changes(
            boundaries_, next.boundaries_, [&](const auto address) {
                delta.changed_boundaries.insert(address);
            });

        auto previous_target = predecessors_.begin();
        auto next_target = next.predecessors_.begin();
        while (previous_target != predecessors_.end() ||
               next_target != next.predecessors_.end()) {
            if (next_target == next.predecessors_.end() ||
                (previous_target != predecessors_.end() &&
                 previous_target->first < next_target->first)) {
                delta.changed_predecessor_targets.insert(
                    previous_target->first);
                ++previous_target;
                continue;
            }
            if (previous_target == predecessors_.end() ||
                next_target->first < previous_target->first) {
                delta.changed_predecessor_targets.insert(
                    next_target->first);
                ++next_target;
                continue;
            }
            bool membership_changed = false;
            collect_membership_changes(
                previous_target->second,
                next_target->second,
                [&](const auto) { membership_changed = true; });
            if (membership_changed)
                delta.changed_predecessor_targets.insert(
                    previous_target->first);
            ++previous_target;
            ++next_target;
        }
        return delta;
    }

    [[nodiscard]] GlobalEntryPredecessorDelta replace(
        GlobalEntryPredecessorIndex next) {
        auto delta = difference(next);
        *this = std::move(next);
        return delta;
    }

    template <typename Edge, std::size_t Extent>
    [[nodiscard]] bool closes(
        const std::uint32_t seed,
        const std::uint32_t load,
        const std::span<Edge, Extent> allowed_internal_edges,
        const GlobalEntryPredecessorIndex* const additional = nullptr) const {
        if (seed >= load) return false;
        const auto boundary = boundaries_.upper_bound(seed);
        if (boundary != boundaries_.end() && boundary->first <= load)
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

    std::map<std::uint32_t, std::size_t> boundaries_;
    std::map<std::uint32_t,
             std::map<std::uint32_t, std::size_t>> predecessors_;
};

} // namespace katana::analysis::detail
