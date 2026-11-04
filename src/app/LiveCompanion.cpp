// Live bridge: a private folder of JSON request/response files that a local
// companion (such as Flows Live) uses to read, preview, apply and restore
// source proposals. Desktop hosts enable it; the browser host does not.
#include "AppCore.hpp"
#include "CoreDetail.hpp"
#include "Files.hpp"
#include "Text.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace flows::app {
using namespace detail;
namespace {
namespace fs = std::filesystem;
void need(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}
void privateFile(const fs::path& file, int mode) {
#if !defined(_WIN32)
    if (::chmod(file.c_str(), static_cast<mode_t>(mode)) != 0)
        throw std::runtime_error(std::string("Cannot set Live bridge permissions: ") + std::strerror(errno));
#else
    (void)file;
    (void)mode;
#endif
}
// POSIX rename replaces a file in the same directory atomically, so readers
// never miss instance.json while it is replaced.
void atomicJSON(const fs::path& file, const json::Value& value) {
    auto temp = file;
    temp += ".tmp";
    try {
        files::writeAtomic(temp, json::stringify(value));
    } catch (...) {
        files::remove(temp);
        throw std::runtime_error("Cannot write Live bridge response");
    }
    try {
        privateFile(temp, 0600);
    } catch (...) {
        files::remove(temp);
        throw;
    }
    std::error_code error;
    fs::rename(temp, file, error);
    if (error) {
        files::remove(temp);
        throw std::runtime_error("Cannot publish Live bridge response: " + error.message());
    }
}
std::string toolError(const json::Value& result, const char* fallback) {
    auto reason = json::str(result, "error");
    return !reason.empty() ? reason : std::string(fallback);
}
}  // namespace

json::Value AppCore::companionRequest(const std::string& method, const json::Value& params,
                                      const std::function<bool()>& cancelled) {
    const auto stopNow = [&] { return static_cast<bool>(cancelled) && cancelled(); };
    if (method == "read") {
        std::lock_guard lock(mutex_);
        return json::object({{"documentId", document_.path},
                             {"revision", static_cast<std::int64_t>(document_.revision)},
                             {"source", document_.text},
                             {"compileStatus", compiled_.status},
                             {"name", files::stem(files::fileName(document_.path))}});
    }
    if (method == "compile" || method == "render" || method == "compare") {
        std::string current;
        {
            std::lock_guard lock(mutex_);
            current = document_.text;
        }
        return runAITool(method == "compile" ? "compile_snapshot" : method == "render" ? "render_preview" : "compare_observations",
                         params, current, stopNow);
    }
    if (method == "propose") {
        const auto source = json::str(params, "source"), documentId = json::str(params, "documentId");
        const auto revision = static_cast<std::uint64_t>(params["baseRevision"].asInt());
        std::string before;
        need(!source.empty() && source.size() <= 262144, "Source is empty or too large");
        {
            std::lock_guard lock(mutex_);
            need(!documentId.empty() && documentId == document_.path && revision == document_.revision,
                 "Instrument changed; read it again");
            before = document_.text;
        }
        auto checked = runAITool("compile_snapshot", json::object({{"source", source}}), {}, stopNow);
        if (json::str(checked, "status") != "compiled")
            return json::object(
                {{"ok", false}, {"error", toolError(checked, "Candidate did not compile")}, {"diagnostics", checked}});
        auto rendered = runAITool("render_preview",
                                  json::object({{"source", source}, {"duration_seconds", 1.5}, {"pitch", 42}}), {}, stopNow);
        if (json::str(rendered, "status") != "rendered")
            return json::object(
                {{"ok", false}, {"error", toolError(rendered, "Candidate did not render")}, {"diagnostics", rendered}});
        auto comparison = runAITool("compare_observations",
                                    json::object({{"source", source},
                                                  {"before_source", before},
                                                  {"after_source", source},
                                                  {"duration_seconds", 1.5},
                                                  {"pitch", 42}}),
                                    {}, stopNow);
        // The candidate already compiled and rendered. A baseline that cannot render
        // must not block a repair. Cancellation still rejects the proposal.
        if (json::str(comparison, "status") != "compared" &&
            (stopNow() || toolError(comparison, "Candidate comparison failed") == "AI request cancelled."))
            return json::object({{"ok", false},
                                 {"error", toolError(comparison, "Candidate comparison failed")},
                                 {"diagnostics", comparison}});
        std::lock_guard lock(mutex_);
        need(documentId == document_.path && revision == document_.revision && before == document_.text,
             "Instrument changed during preview");
        // Same guards as apply and restore. Restored proposals and proposals the
        // document has moved past are not usable, so they do not consume the cap.
        for (auto it = companionProposals_.begin(); it != companionProposals_.end();) {
            const auto& p = it->second;
            const bool applicable = !p.applied && !p.restored && document_.path == p.documentId &&
                                    document_.revision == p.baseRevision && document_.text == p.before;
            const bool restorable = p.applied && !p.restored && document_.path == p.documentId &&
                                    document_.revision == p.appliedRevision && document_.text == p.source;
            if (applicable || restorable)
                ++it;
            else
                it = companionProposals_.erase(it);
        }
        need(companionProposals_.size() < 64, "Too many open instrument proposals");
        const auto id = text::randomId();
        companionProposals_[id] = {source, before, documentId, revision};
        return json::object({{"ok", true}, {"proposalId", id}, {"render", rendered}, {"comparison", comparison}});
    }
    if (method == "apply" || method == "restore") {
        std::lock_guard lock(mutex_);
        const auto id = method == "apply" ? json::str(params, "proposalId") : json::str(params["receipt"], "proposalId");
        auto it = companionProposals_.find(id);
        need(it != companionProposals_.end(), "Proposal belongs to another instance session");
        auto& p = it->second;
        need(document_.path == p.documentId, "The active instrument changed");
        if (method == "apply") {
            need(!p.applied && !p.restored, "Proposal already applied");
            need(document_.revision == p.baseRevision && document_.text == p.before, "Source changed since preview");
            applyLocked(p.source);
            p.appliedRevision = document_.revision;
            p.applied = true;
            return json::object(
                {{"applied", true},
                 {"receipt", json::object({{"proposalId", id},
                                           {"instanceId", companionId_},
                                           {"revision", static_cast<std::int64_t>(p.appliedRevision)}})}});
        }
        need(json::str(params["receipt"], "instanceId") == companionId_, "Receipt belongs to another instance");
        need(p.applied && !p.restored && document_.revision == p.appliedRevision && document_.text == p.source,
             "Source changed after apply; preserve your edits");
        applyLocked(p.before);
        p.restored = true;
        return json::object({{"restored", true}, {"receipt", params["receipt"]}});
    }
    throw std::runtime_error("Unsupported Live companion method");
}

void AppCore::companionLoop(std::stop_token stop) {
    const auto root = options_.liveBridgeDirectory;
    const auto dir = root / companionId_, requests = dir / "requests", responses = dir / "responses";
    const auto cancelled = [stop] { return stop.stop_requested(); };
    const auto report = [this](const std::string& message) {
        std::lock_guard lock(mutex_);
        emitLocked(json::object({{"status", "failed"}, {"error", "Live bridge: " + message}}));
    };
    const auto age = [](const fs::path& file) { return text::currentTimeMillis() - files::modifiedMillis(file); };
    try {
        need(files::createDirectories(requests) && files::createDirectories(responses), "Cannot create Live bridge");
        privateFile(root, 0700);
        privateFile(dir, 0700);
        privateFile(requests, 0700);
        privateFile(responses, 0700);
        int ticks = 0;
        std::set<std::string> seen;
        std::map<std::string, json::Value> pending;
        std::string reported;
        while (!stop.stop_requested()) {
            try {
                if (ticks++ % 10 == 0) {
                    auto view = companionRequest("read", {}, cancelled);
                    atomicJSON(dir / "instance.json", json::object({{"id", companionId_},
                                                                     {"name", json::str(view, "name")},
                                                                     {"documentId", view["documentId"]},
                                                                     {"revision", view["revision"]},
                                                                     {"updatedAt", text::currentTimeMillis()}}));
                }
                for (auto it = pending.begin(); it != pending.end();) {
                    if (files::isFile(requests / (it->first + ".json"))) {
                        ++it;
                        continue;
                    }
                    seen.insert(it->first);
                    it = pending.erase(it);
                    if (seen.size() > 512) seen.erase(seen.begin());
                }
                std::vector<fs::path> requestFiles;
                std::error_code error;
                for (fs::directory_iterator iterator(requests, error), end; !error && iterator != end; iterator.increment(error))
                    if (iterator->path().extension() == ".json") requestFiles.push_back(iterator->path());
                std::sort(requestFiles.begin(), requestFiles.end());
                int handled = 0;
                for (const auto& file : requestFiles) {
                    if (stop.stop_requested()) break;
                    const auto id = file.stem().string();
                    if (id.size() < 16 || id.size() > 64 || !text::containsOnly(id, "0123456789abcdef-") || files::isSymlink(file) ||
                        files::size(file) > 524288) {
                        files::remove(file);
                        continue;
                    }
                    if (seen.contains(id)) {
                        files::remove(file);
                        continue;
                    }
                    if (!pending.contains(id)) {
                        // Retries are publishes, not new work. Stop taking new requests once
                        // eight unpublished results are waiting, so a stuck folder cannot
                        // accumulate renders or apply edits the client never hears about.
                        if (handled >= 4 || pending.size() >= 8) continue;
                        ++handled;
                        try {
                            const auto content = files::read(file, 524288);
                            auto input = json::parseBounded(content, 524288);
                            need(json::str(input, "id") == id, "Request identity mismatch");
                            need(age(file) < 60000, "Request expired");
                            pending[id] = json::object(
                                {{"id", id}, {"result", companionRequest(json::str(input, "method"), input["params"], cancelled)}});
                        } catch (const std::exception& e) {
                            pending[id] = json::object({{"id", id}, {"error", std::string(e.what())}});
                        } catch (...) {
                            pending[id] = json::object({{"id", id}, {"error", "Live bridge request failed"}});
                        }
                    }
                    const auto response = responses / (id + ".json");
                    // Rename cannot replace a directory. Forget the request instead of
                    // retrying it on every tick.
                    if (files::exists(response) && !files::isFile(response)) {
                        pending.erase(id);
                        seen.insert(id);
                        files::remove(file);
                        if (seen.size() > 512) seen.erase(seen.begin());
                        continue;
                    }
                    try {
                        atomicJSON(response, pending[id]);
                        pending.erase(id);
                        seen.insert(id);
                        files::remove(file);
                        if (seen.size() > 512) seen.erase(seen.begin());
                    } catch (...) {
                        // Keep the computed result while its request file remains. The next tick
                        // publishes it again without applying or restoring a second time.
                    }
                }
                // Response files older than two minutes are deleted to bound disk use.
                // `seen` suppresses replay only while it remembers an id. Past 512 ids it
                // drops the lexicographically smallest one, so that request can run again.
                if (ticks % 100 == 0)
                    for (fs::directory_iterator iterator(responses, error), end; !error && iterator != end; iterator.increment(error))
                        if (iterator->path().extension() == ".json" && age(iterator->path()) > 120000)
                            files::remove(iterator->path());
            } catch (const std::exception& e) {
                if (reported != e.what()) {
                    reported = e.what();
                    report(reported);
                }
            } catch (...) {
                const std::string fallback = "unexpected failure";
                if (reported != fallback) {
                    reported = fallback;
                    report(reported);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& e) {
        report(e.what());
    } catch (...) {
        report("unexpected failure");
    }
    files::removeAll(dir);
}
}  // namespace flows::app
