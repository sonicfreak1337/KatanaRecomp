#pragma once

#include "katana/runtime/block_table.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace katana::runtime {

enum class CodeWriteSource : std::uint8_t { Cpu, Dma, Copy };

struct ExecutableBlockRegistration {
    std::string identity;
    std::uint32_t physical_start = 0u;
    std::uint32_t size = 0u;
    std::string provenance;
    std::set<std::string> incoming_links;
};

struct CodeInvalidationResult {
    std::vector<std::string> invalidated_blocks;
    std::vector<std::string> unlinked_sources;
    std::vector<std::uint32_t> changed_pages;
    CodeWriteSource source = CodeWriteSource::Cpu;
    bool byte_identical = false;
};

class ExecutableCodeTracker {
public:
    static constexpr std::uint32_t page_size = 4096u;

    void register_block(ExecutableBlockRegistration block);
    [[nodiscard]] CodeInvalidationResult observe_write(
        std::uint32_t address,
        std::size_t size,
        CodeWriteSource source,
        bool bytes_changed = true
    );
    [[nodiscard]] bool valid(const std::string& identity) const;
    [[nodiscard]] std::uint64_t page_generation(std::uint32_t address) const noexcept;
    [[nodiscard]] std::uint64_t invalidation_count() const noexcept;
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& hotspots() const noexcept;

private:
    struct Tracked { ExecutableBlockRegistration block; bool valid = true; };
    std::vector<Tracked> blocks_;
    std::map<std::uint32_t, std::uint64_t> generations_;
    std::map<std::uint32_t, std::uint64_t> hotspots_;
    std::uint64_t invalidation_count_ = 0u;
};

} // namespace katana::runtime
