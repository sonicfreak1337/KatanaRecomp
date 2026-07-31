#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/progress.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace katana::codegen::detail {

class StructuredControlFlowProgress final {
  public:
    StructuredControlFlowProgress(
        const katana::ProgressReporter& reporter,
        std::string label)
        : control_(reporter.begin(
              katana::ProgressOperation::ControlFlowAnalysis,
              katana::ProgressUnit::Steps,
              std::nullopt,
              std::move(label))) {}

    void update(
        const katana::analysis::ControlFlowAnalysisProgress& progress) {
        katana::ProgressCounterSnapshot control_counters;
        control_counters.iteration = progress.iteration;
        control_counters.discovered = progress.seeds;
        control_counters.started = progress.instructions;
        control_counters.queued_work = progress.contexts;
        control_.update(
            progress.iteration,
            std::move(control_counters));
        if (!progress.function_value_active) return;

        if (!function_values_) {
            function_values_.emplace(
                control_.child_reporter().begin(
                    katana::ProgressOperation::FunctionValueAnalysis,
                    katana::ProgressUnit::Steps,
                    std::nullopt,
                    "function-value-analysis"));
        }
        katana::ProgressCounterSnapshot function_counters;
        function_counters.iteration =
            progress.function_value_iterations;
        function_counters.pass = progress.iteration;
        function_counters.active_workers =
            progress.function_value_active_workers;
        function_counters.queued_work =
            progress.function_value_pending;
        function_counters.discovered =
            progress.function_value_functions;
        function_counters.started =
            progress.function_value_physical_evaluations;
        function_counters.cache_hits =
            progress.function_value_session_cache_hits;
        function_counters.cache_misses =
            progress.function_value_session_cache_misses;
        function_values_->update(
            progress.function_value_logical_evaluations,
            std::move(function_counters));

        if (progress.function_value_resolution_functions_total !=
                0u &&
            !resolution_) {
            resolution_.emplace(
                function_values_->child_reporter().begin(
                    katana::ProgressOperation::CandidateResolution,
                    katana::ProgressUnit::Functions,
                    progress
                        .function_value_resolution_functions_total,
                    "function-value-resolution"));
        }
        if (resolution_) {
            katana::ProgressCounterSnapshot resolution_counters;
            resolution_counters.pass = progress.iteration;
            resolution_counters.active_workers =
                progress.function_value_active_workers;
            resolution_counters.queued_work =
                progress
                    .function_value_resolution_functions_total -
                std::min(
                    progress
                        .function_value_resolution_functions_total,
                    progress
                        .function_value_completed_functions);
            resolution_counters.discovered =
                progress
                    .function_value_resolution_functions_total;
            resolution_counters.started =
                progress.function_value_logical_evaluations;
            resolution_counters.cache_hits =
                progress.function_value_session_cache_hits;
            resolution_counters.cache_misses =
                progress.function_value_session_cache_misses;
            resolution_->update(
                std::min(
                    progress
                        .function_value_resolution_functions_total,
                    progress
                        .function_value_completed_functions),
                std::move(resolution_counters));
        }

        if (progress.phase.starts_with(
                "function-values-complete") ||
            progress.phase.find("budget-exhausted") !=
                std::string_view::npos)
            close_function_values();
    }

    void complete(const std::size_t iterations) {
        close_function_values();
        control_.complete(iterations);
    }

  private:
    void close_function_values() {
        if (resolution_) {
            resolution_->complete();
            resolution_.reset();
        }
        if (function_values_) {
            function_values_->complete();
            function_values_.reset();
        }
    }

    katana::ProgressScope control_;
    std::optional<katana::ProgressScope> function_values_;
    std::optional<katana::ProgressScope> resolution_;
};

} // namespace katana::codegen::detail
