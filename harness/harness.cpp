#include "harness.hpp"

namespace harness {

namespace plugin = yoke::plugin;

const plugin::Declaration &declaration() {
    static const plugin::Declaration d{
        "com.yoke.conformance.cpp",
        {},
        {{"conformance.data"}, {"conformance.frames", true}},
        {"calibrate"},
        {"status"},
        {"conformance.drift"},
        {
            {"stream.data.publish", {plugin::Governs::stream, "conformance.data"}},
            {"stream.frames.publish", {plugin::Governs::stream, "conformance.frames"}},
            {"command.calibrate.accept", {plugin::Governs::command, "calibrate"}},
            {"query.status.answer", {plugin::Governs::query, "status"}},
            {"event.drift.report", {plugin::Governs::occurrence, "conformance.drift"}},
        },
    };
    return d;
}

int serve(const yoke::Getenv &) { return 1; }

} // namespace harness
