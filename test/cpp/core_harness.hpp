// Shared setup for tests that drive the portable app core directly: a
// temporary workspace, the bundled test instruments, a core prepared for
// 48 kHz / 128-frame blocks, and helpers that render blocks.
#pragma once
#include "app/AppCore.hpp"
#include "app/Files.hpp"
#include "app/Text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace flows::testing {
namespace fs = std::filesystem;
using namespace flows::app;

inline void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryFolder {
    fs::path path = fs::temp_directory_path() / ("flows-core-test-" + text::randomId());
    TemporaryFolder() { fs::create_directories(path); }
    ~TemporaryFolder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
struct Bundle {
    // BundledFile views these strings; a deque keeps them in place as it grows.
    std::deque<std::string> storage;
    std::vector<BundledFile> files;
    explicit Bundle(const fs::path& repo) {
        const auto add = [&](const fs::path& file) {
            storage.push_back(files::read(file, 8 * 1024 * 1024));
            files.push_back({file.filename().string(), storage.back()});
        };
        std::vector<fs::path> presets;
        for (const auto& entry : fs::directory_iterator(repo / "test/presets"))
            if (entry.path().extension() == ".flows") presets.push_back(entry.path());
        std::sort(presets.begin(), presets.end());
        for (const auto& file : presets) add(file);
        add(repo / "test/fixtures/samples/flows-kick.wav");
        add(repo / "test/fixtures/samples/flows-stereo.wav");
    }
};
inline std::unique_ptr<AppCore> makeCore(const fs::path& workspace, const Bundle& bundle, bool standalone = false) {
    CoreOptions options;
    options.workspace = workspace / "Workspace";
    options.workspaceIdentity = "conformance-workspace";
    options.settingsDirectory = workspace / "Settings";
    options.standalone = standalone;
    options.bundled = bundle.files;
    options.secrets = memoryAISecrets(false);
    options.capabilities = json::object({{"host", "embedded"}});
    auto core = std::make_unique<AppCore>(std::move(options));
    core->audio().prepare(48000, 128);
    return core;
}
inline void render(AppCore& core, int blocks) {
    std::array<float, 128> left{}, right{};
    for (int i = 0; i < blocks; ++i) {
        AudioBlock block;
        block.left = left.data();
        block.right = right.data();
        block.frames = 128;
        core.audio().process(block);
    }
}
inline void waitReady(AppCore& core) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        const auto snapshot = core.snapshot();
        const auto status = json::str(snapshot["compile"], "status");
        const auto revision = snapshot["document"]["revision"].asInt();
        const bool installed = snapshot["audio"]["programRevision"].asInt() == revision;
        if ((status == "ready" && installed) || status == "error" || status == "idle") return;
        check(std::chrono::steady_clock::now() < deadline, "Timed out waiting for compilation");
        render(core, 4);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}  // namespace flows::testing
