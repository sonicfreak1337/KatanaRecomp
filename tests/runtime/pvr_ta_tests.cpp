#include "katana/runtime/pvr.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
katana::runtime::PvrVertex vertex(const float x) { return {x, x + 1.0f, 0.5f, 0.0f, 0.0f, 0xFFFFFFFFu}; }
}

int main() {
    using namespace katana::runtime;
    TileAccelerator ta;
    require(throws<std::logic_error>([&] { ta.submit_vertex(vertex(0.0f), true); }),
        "Vertex ohne offene TA-Liste wird akzeptiert.");
    ta.begin_list(PvrListType::Opaque);
    ta.submit_vertex(vertex(0.0f), false);
    ta.submit_vertex(vertex(1.0f), false);
    ta.submit_vertex(vertex(2.0f), false);
    ta.submit_vertex(vertex(3.0f), true);
    ta.end_list();
    ta.begin_list(PvrListType::Translucent);
    ta.submit_vertex(vertex(4.0f), false);
    ta.submit_vertex(vertex(5.0f), false);
    ta.submit_vertex(vertex(6.0f), true);
    ta.end_list();
    const auto frame = ta.finish_frame();
    require(frame.primitives.size() == 2u && frame.primitives[0].vertices.size() == 4u &&
        frame.primitives[1].list == PvrListType::Translucent,
        "TA verliert Strips, Vertices oder Listenklassifikation.");
    require(ta.finish_frame().primitives.empty(), "TA wiederholt Primitive im naechsten Frame.");

    TileAccelerator short_strip;
    short_strip.begin_list(PvrListType::Opaque);
    short_strip.submit_vertex(vertex(0.0f), false);
    require(throws<std::invalid_argument>([&] { short_strip.submit_vertex(vertex(1.0f), true); }),
        "TA akzeptiert einen Strip mit weniger als drei Vertices.");
    TileAccelerator ordering;
    ordering.begin_list(PvrListType::Translucent);
    ordering.submit_vertex(vertex(0.0f), false);
    ordering.submit_vertex(vertex(1.0f), false);
    ordering.submit_vertex(vertex(2.0f), true);
    ordering.end_list();
    require(throws<std::logic_error>([&] { ordering.begin_list(PvrListType::Opaque); }),
        "TA akzeptiert rueckwaertige Listenreihenfolge.");
    TileAccelerator empty_ordering;
    empty_ordering.begin_list(PvrListType::Translucent);
    empty_ordering.end_list();
    require(throws<std::logic_error>([&] { empty_ordering.begin_list(PvrListType::Opaque); }),
        "TA vergisst die Reihenfolge einer leeren Liste.");

    std::cout << "KR-2803 Tile-Accelerator-Grundpfad erfolgreich.\n";
}
