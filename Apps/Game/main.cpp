// GalaxyEngine graphical client (vertical slice M2). The simulation itself lives in the libraries and runs
// identically headless; this program only presents it and turns player input into commands.
//
//   gx_game [--seed <n>] [--frames <n> --screenshot <file.png>] [--prerun-hours <h>] [--zoom <m/px>]
//           [--follow-star] [--select] [--truth] [--fly-to <port index>] [--engage-nearest]

#include "Apps/Game/GameApp.h"

#include <charconv>
#include <cstdio>
#include <string_view>

namespace {

template <typename T>
bool parseNumber(std::string_view text, T& out) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv) {
    gx::GameApp::Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string_view {
            return i + 1 < argc ? std::string_view(argv[++i]) : "";
        };
        bool ok = true;
        if (arg == "--seed") {
            ok = parseNumber(value(), options.seed);
        } else if (arg == "--frames") {
            ok = parseNumber(value(), options.frames);
        } else if (arg == "--screenshot") {
            options.screenshot = std::string(value());
        } else if (arg == "--prerun-hours") {
            ok = parseNumber(value(), options.prerunHours);
        } else if (arg == "--zoom") {
            ok = parseNumber(value(), options.metersPerPixel);
        } else if (arg == "--follow-star") {
            options.followStar = true;
        } else if (arg == "--fly-to") {
            ok = parseNumber(value(), options.flyToPort);
        } else if (arg == "--truth") {
            options.showTruth = true;
        } else if (arg == "--engage-nearest") {
            options.engageNearest = true;
        } else if (arg == "--select") {
            options.select = true;
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "invalid argument: %.*s\n", static_cast<int>(arg.size()), arg.data());
            return 2;
        }
    }
    gx::GameApp app;
    return app.run(options);
}
