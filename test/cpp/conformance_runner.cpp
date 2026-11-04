// Runs protocol/conformance/session.json against the portable core and writes
// every response as JSON. The same program builds natively and with
// Emscripten (Node), so the two outputs can be validated and compared.
//
//   conformance_runner --repo <source dir> --out <results.json>
//   conformance_runner --repo <source dir> --coverage
#include "app/AppCore.hpp"
#include "app/Files.hpp"
#include "app/Text.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
using flows::json::Value;
using namespace flows;
using namespace flows::app;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryFolder {
    fs::path path = fs::temp_directory_path() / ("flows-conformance-" + text::randomId());
    TemporaryFolder() { fs::create_directories(path); }
    ~TemporaryFolder() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
struct Bundle {
    std::vector<std::string> storage;
    std::vector<BundledFile> files;
    explicit Bundle(const fs::path& repo) {
        storage.reserve(16);
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
std::unique_ptr<AppCore> makeCore(const fs::path& workspace, const Bundle& bundle) {
    CoreOptions options;
    options.workspace = workspace / "Workspace";
    options.workspaceIdentity = "conformance-workspace";
    options.settingsDirectory = workspace / "Settings";
    options.standalone = false;
    options.bundled = bundle.files;
    options.secrets = memoryAISecrets(false);
    options.capabilities = json::object({{"host", "embedded"}});
    auto core = std::make_unique<AppCore>(std::move(options));
    core->audio().prepare(48000, 128);
    return core;
}
void render(AppCore& core, int blocks) {
    std::array<float, 128> left{}, right{};
    for (int i = 0; i < blocks; ++i) {
        AudioBlock block;
        block.left = left.data();
        block.right = right.data();
        block.frames = 128;
        core.audio().process(block);
    }
}
void waitReady(AppCore& core) {
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
const Value& lookup(const Value& root, const std::string& path) {
    const Value* current = &root;
    for (const auto& part : text::split(path, ".")) {
        if (const auto* array = current->getArray()) {
            const auto index = static_cast<std::size_t>(std::stoul(part));
            check(index < array->size(), "Index out of range in $" + path);
            current = &(*array)[index];
        } else
            current = &(*current)[part];
    }
    return *current;
}
Value resolve(const Value& value, const Value& snapshot, const Value& results) {
    if (value.isString() && value.string().starts_with("$")) {
        const auto path = value.string().substr(1);
        if (path.starts_with("snapshot.")) return lookup(snapshot, path.substr(9));
        if (path.starts_with("result.")) return lookup(results, path.substr(7));
        throw std::runtime_error("Unknown reference " + value.string());
    }
    if (const auto* array = value.getArray()) {
        json::Array resolved;
        for (const auto& item : *array) resolved.push_back(resolve(item, snapshot, results));
        return Value(std::move(resolved));
    }
    if (const auto* object = value.getObject()) {
        json::Object resolved;
        for (const auto& [key, item] : *object) resolved.set(key, resolve(item, snapshot, results));
        return Value(std::move(resolved));
    }
    return value;
}

int runScript(const fs::path& repo, const fs::path& out) {
    const auto script = json::parse(files::read(repo / "protocol/conformance/session.json", 1024 * 1024));
    TemporaryFolder folder;
    const Bundle bundle(repo);
    auto core = makeCore(folder.path, bundle);
    waitReady(*core);
    json::Array log;
    Value results = Value::object();
    for (const auto& step : *script["steps"].getArray()) {
        const auto label = json::str(step, "label");
        const auto method = json::str(step, "method");
        const auto snapshot = core->snapshot();
        const auto params = resolve(step["params"], snapshot, results);
        auto entry = json::object({{"label", label}, {"method", method}, {"params", params}});
        try {
            auto result = core->request(method, params);
            if (json::str(step, "waitFor") == "ready") {
                waitReady(*core);
                if (result.has("sessionId")) result = core->snapshot();
            }
            check(!step.has("expectError"), label + ": expected an error, got a result");
            results.set(label, result);
            entry.set("ok", true);
            entry.set("result", std::move(result));
        } catch (const std::exception& error) {
            const auto expected = json::str(step, "expectError");
            if (expected.empty() || std::string(error.what()).find(expected) == std::string::npos)
                throw std::runtime_error(label + ": " + error.what());
            entry.set("ok", false);
            entry.set("error", std::string(error.what()));
        }
        log.push_back(std::move(entry));
    }
    core.reset();
    files::writeAtomic(out, json::stringifyPretty(json::object({{"steps", std::move(log)}})));
    std::cout << "Conformance session: " << script["steps"].size() << " steps written to " << out.string() << "\n";
    return 0;
}

// Every core method in the schema must be dispatched by the core.
int checkCoverage(const fs::path& repo) {
    const auto protocol = json::parse(files::read(repo / "protocol/flows-protocol.json", 4 * 1024 * 1024));
    TemporaryFolder folder;
    const Bundle bundle(repo);
    auto core = makeCore(folder.path, bundle);
    int count = 0;
    for (const auto& [method, description] : *protocol["methods"].getObject()) {
        if (json::str(description, "owner") != "core") continue;
        ++count;
        try {
            (void)core->request(method, Value::object());
        } catch (const std::exception& error) {
            check(std::string(error.what()).find("Unknown native method") == std::string::npos,
                  "Schema method is not implemented by the core: " + method);
        }
    }
    try {
        (void)core->request("no.such.method", Value::object());
        throw std::runtime_error("Unknown methods must be rejected");
    } catch (const std::exception& error) {
        check(std::string(error.what()).find("Unknown native method") != std::string::npos, error.what());
    }
    std::cout << "Protocol coverage: all " << count << " core methods are dispatched.\n";
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        fs::path repo, out;
        bool coverage = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--repo" && i + 1 < argc) repo = argv[++i];
            else if (arg == "--out" && i + 1 < argc) out = argv[++i];
            else if (arg == "--coverage") coverage = true;
        }
        check(!repo.empty(), "Usage: conformance_runner --repo <source> (--out <file> | --coverage)");
        return coverage ? checkCoverage(repo) : runScript(repo, out.empty() ? fs::path("conformance.json") : out);
    } catch (const std::exception& error) {
        std::cerr << "Conformance failure: " << error.what() << "\n";
        return 1;
    }
}
