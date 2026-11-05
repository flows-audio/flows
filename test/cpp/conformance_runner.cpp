// Runs protocol/conformance/session.json against the portable core and writes
// every response as JSON. The same program builds natively and with
// Emscripten (Node), so the two outputs can be validated and compared.
//
//   conformance_runner --repo <source dir> --out <results.json>
//   conformance_runner --repo <source dir> --coverage
#include "app/AppCore.hpp"
#include "app/Files.hpp"
#include "app/Text.hpp"
#include "app/WorkspaceLayout.hpp"
#include "core_harness.hpp"

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
using namespace flows::testing;

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
            // A returned snapshot lists the workspace as the request left it,
            // not as of the worker's last periodic scan.
            if (const auto* listed = result["files"].getArray())
                check(Value(*listed) == Value(workspaceFiles(folder.path / "Workspace")),
                      "the snapshot's file list does not match the workspace");
            if (json::str(step, "waitFor") == "ready") {
                waitReady(*core);
                const auto settled = core->snapshot();
                check(json::str(settled["compile"], "status") == "ready" &&
                          settled["audio"]["programRevision"].asInt() == settled["document"]["revision"].asInt(),
                      "the step did not end compiled and installed (" + json::str(settled["compile"], "status") + ")");
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
