#include "katana/runtime/dynamic_interpreter.hpp"

#include <cstdlib>

namespace {

using DynamicInterpreterEntry = decltype(&katana::runtime::execute_dynamic_sh4_block);

DynamicInterpreterEntry volatile forbidden_interpreter_entry =
    &katana::runtime::execute_dynamic_sh4_block;

} // namespace

int main() {
    const auto retained_entry = forbidden_interpreter_entry;
    return retained_entry == nullptr ? EXIT_FAILURE : EXIT_SUCCESS;
}
