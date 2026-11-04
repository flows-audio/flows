#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include "FlowsProcessor.hpp"
namespace flows::app {
namespace {
void need(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}
void privateFile(const juce::File& file, int mode) {
    if (::chmod(file.getFullPathName().toRawUTF8(), static_cast<mode_t>(mode)) != 0)
        throw std::runtime_error(std::string("Cannot set Live bridge permissions: ") +
                                 std::strerror(errno));
}
// POSIX rename replaces a file in the same directory atomically. JUCE's
// moveFileTo deletes the destination first, which hides instance.json.
void atomicJSON(const juce::File& file, const juce::var& value) {
    auto temp = file.getSiblingFile(file.getFileName() + ".tmp");
    if (!temp.replaceWithText(juce::JSON::toString(value, true))) {
        temp.deleteFile();
        throw std::runtime_error("Cannot write Live bridge response");
    }
    try {
        privateFile(temp, 0600);
    } catch (...) {
        temp.deleteFile();
        throw;
    }
    if (::rename(temp.getFullPathName().toRawUTF8(), file.getFullPathName().toRawUTF8()) != 0) {
        const auto error = errno;
        temp.deleteFile();
        throw std::runtime_error(std::string("Cannot publish Live bridge response: ") +
                                 std::strerror(error));
    }
}
juce::String toolError(const juce::var& result, const char* fallback) {
    auto reason = str(result, "error");
    return reason.isNotEmpty() ? reason : juce::String(fallback);
}
}  // namespace
juce::var FlowsProcessor::companionRequest(const juce::String& method, const juce::var& params,
                                           const std::function<bool()>& cancelled) {
    using namespace juce;
    const auto stopNow = [&] { return static_cast<bool>(cancelled) && cancelled(); };
    if (method == "read") {
        std::lock_guard lock(mutex_);
        return object({{"documentId", document_.path},
                       {"revision", static_cast<int64>(document_.revision)},
                       {"source", document_.text},
                       {"compileStatus", compiled_.status},
                       {"name", File(document_.path).getFileNameWithoutExtension()}});
    }
    if (method == "compile" || method == "render" || method == "compare") {
        String current;
        {
            std::lock_guard lock(mutex_);
            current = document_.text;
        }
        return runAITool(method == "compile"  ? "compile_snapshot"
                         : method == "render" ? "render_preview"
                                              : "compare_observations",
                         params, current, stopNow);
    }
    if (method == "propose") {
        const auto source = str(params, "source"), documentId = str(params, "documentId");
        const auto revision =
            static_cast<std::uint64_t>(static_cast<int64>(get(params, "baseRevision")));
        String before;
        need(source.isNotEmpty() && source.getNumBytesAsUTF8() <= 262144,
             "Source is empty or too large");
        {
            std::lock_guard lock(mutex_);
            need(documentId.isNotEmpty() && documentId == document_.path &&
                     revision == document_.revision,
                 "Instrument changed; read it again");
            before = document_.text;
        }
        auto checked = runAITool("compile_snapshot", object({{"source", source}}), {}, stopNow);
        if (str(checked, "status") != "compiled")
            return object({{"ok", false},
                           {"error", toolError(checked, "Candidate did not compile")},
                           {"diagnostics", checked}});
        auto rendered = runAITool(
            "render_preview",
            object({{"source", source}, {"duration_seconds", 1.5}, {"pitch", 42}}), {}, stopNow);
        if (str(rendered, "status") != "rendered")
            return object({{"ok", false},
                           {"error", toolError(rendered, "Candidate did not render")},
                           {"diagnostics", rendered}});
        auto comparison = runAITool("compare_observations",
                                    object({{"source", source},
                                            {"before_source", before},
                                            {"after_source", source},
                                            {"duration_seconds", 1.5},
                                            {"pitch", 42}}),
                                    {}, stopNow);
        // The candidate already compiled and rendered. A baseline that cannot render
        // must not block a repair. Cancellation still rejects the proposal.
        if (str(comparison, "status") != "compared" &&
            (stopNow() ||
             toolError(comparison, "Candidate comparison failed") == "AI request cancelled."))
            return object({{"ok", false},
                           {"error", toolError(comparison, "Candidate comparison failed")},
                           {"diagnostics", comparison}});
        std::lock_guard lock(mutex_);
        need(documentId == document_.path && revision == document_.revision &&
                 before == document_.text,
             "Instrument changed during preview");
        // Same guards as apply and restore. Restored proposals and proposals the
        // document has moved past are not usable, so they do not consume the cap.
        for (auto it = companionProposals_.begin(); it != companionProposals_.end();) {
            const auto& p = it->second;
            const bool applicable = !p.applied && !p.restored && document_.path == p.documentId &&
                                    document_.revision == p.baseRevision &&
                                    document_.text == p.before;
            const bool restorable = p.applied && !p.restored && document_.path == p.documentId &&
                                    document_.revision == p.appliedRevision &&
                                    document_.text == p.source;
            if (applicable || restorable)
                ++it;
            else
                it = companionProposals_.erase(it);
        }
        need(companionProposals_.size() < 64, "Too many open instrument proposals");
        const auto id = Uuid().toString();
        companionProposals_[id] = {source, before, documentId, revision};
        return object(
            {{"ok", true}, {"proposalId", id}, {"render", rendered}, {"comparison", comparison}});
    }
    if (method == "apply" || method == "restore") {
        std::lock_guard lock(mutex_);
        const auto id = method == "apply" ? str(params, "proposalId")
                                          : str(get(params, "receipt"), "proposalId");
        auto it = companionProposals_.find(id);
        need(it != companionProposals_.end(), "Proposal belongs to another instance session");
        auto& p = it->second;
        need(document_.path == p.documentId, "The active instrument changed");
        if (method == "apply") {
            need(!p.applied && !p.restored, "Proposal already applied");
            need(document_.revision == p.baseRevision && document_.text == p.before,
                 "Source changed since preview");
            applyLocked(p.source);
            p.appliedRevision = document_.revision;
            p.applied = true;
            return object(
                {{"applied", true},
                 {"receipt", object({{"proposalId", id},
                                     {"instanceId", companionId_},
                                     {"revision", static_cast<int64>(p.appliedRevision)}})}});
        }
        need(str(get(params, "receipt"), "instanceId") == companionId_,
             "Receipt belongs to another instance");
        need(p.applied && !p.restored && document_.revision == p.appliedRevision &&
                 document_.text == p.source,
             "Source changed after apply; preserve your edits");
        applyLocked(p.before);
        p.restored = true;
        return object({{"restored", true}, {"receipt", get(params, "receipt")}});
    }
    throw std::runtime_error("Unsupported Live companion method");
}
void FlowsProcessor::companionLoop(std::stop_token stop) {
    using namespace juce;
    auto overridePath = SystemStats::getEnvironmentVariable("FLOWS_LIVE_SYNTH_DIR", {});
    auto root = overridePath.isNotEmpty()
                    ? File(overridePath)
                    : File::getSpecialLocation(File::userApplicationDataDirectory)
                          .getChildFile("Flows/LiveBridge");
    auto dir = root.getChildFile(companionId_), requests = dir.getChildFile("requests"),
         responses = dir.getChildFile("responses");
    auto cancelled = [stop] { return stop.stop_requested(); };
    auto report = [this](const String& message) {
        std::lock_guard lock(mutex_);
        if (agentEvents_.size() >= 128) agentEvents_.erase(agentEvents_.begin());
        agentEvents_.push_back(
            object({{"status", "failed"}, {"error", "Live bridge: " + message}}));
    };
    try {
        need(requests.createDirectory().wasOk() && responses.createDirectory().wasOk(),
             "Cannot create Live bridge");
        privateFile(root, 0700);
        privateFile(dir, 0700);
        privateFile(requests, 0700);
        privateFile(responses, 0700);
        int ticks = 0;
        std::set<String> seen;
        std::map<String, var> pending;
        String reported;
        while (!stop.stop_requested()) {
            try {
                if (ticks++ % 10 == 0) {
                    auto view = companionRequest("read", {}, cancelled);
                    atomicJSON(dir.getChildFile("instance.json"),
                               object({{"id", companionId_},
                                       {"name", str(view, "name")},
                                       {"documentId", get(view, "documentId")},
                                       {"revision", get(view, "revision")},
                                       {"updatedAt", Time::currentTimeMillis()}}));
                }
                for (auto it = pending.begin(); it != pending.end();) {
                    if (requests.getChildFile(it->first + ".json").existsAsFile()) {
                        ++it;
                        continue;
                    }
                    seen.insert(it->first);
                    it = pending.erase(it);
                    if (seen.size() > 512) seen.erase(seen.begin());
                }
                auto files = requests.findChildFiles(File::findFiles, false, "*.json");
                int handled = 0;
                for (const auto& file : files) {
                    if (stop.stop_requested()) break;
                    auto id = file.getFileNameWithoutExtension();
                    if (id.length() < 16 || id.length() > 64 ||
                        id.containsOnly("0123456789abcdef-") == false || file.isSymbolicLink() ||
                        file.getSize() > 524288) {
                        file.deleteFile();
                        continue;
                    }
                    if (seen.contains(id)) {
                        file.deleteFile();
                        continue;
                    }
                    if (!pending.contains(id)) {
                        // Retries are publishes, not new work. Stop taking new requests once
                        // eight unpublished results are waiting, so a stuck folder cannot
                        // accumulate renders or apply edits the client never hears about.
                        if (handled >= 4 || pending.size() >= 8) continue;
                        ++handled;
                        try {
                            auto text = file.loadFileAsString();
                            if (text.getNumBytesAsUTF8() > 524288)
                                throw std::runtime_error("Request exceeds 512 KiB");
                            auto input = parseJSONBounded(text, 524288);
                            need(str(input, "id") == id, "Request identity mismatch");
                            need(Time::currentTimeMillis() -
                                         file.getLastModificationTime().toMilliseconds() <
                                     60000,
                                 "Request expired");
                            pending[id] = object(
                                {{"id", id},
                                 {"result", companionRequest(str(input, "method"),
                                                             get(input, "params"), cancelled)}});
                        } catch (const std::exception& e) {
                            pending[id] = object({{"id", id}, {"error", String(e.what())}});
                        } catch (...) {
                            pending[id] = object(
                                {{"id", id}, {"error", String("Live bridge request failed")}});
                        }
                    }
                    auto response = responses.getChildFile(id + ".json");
                    // Rename cannot replace a directory. Forget the request instead of
                    // retrying it on every tick.
                    if (response.exists() && !response.existsAsFile()) {
                        pending.erase(id);
                        seen.insert(id);
                        file.deleteFile();
                        if (seen.size() > 512) seen.erase(seen.begin());
                        continue;
                    }
                    try {
                        atomicJSON(response, pending[id]);
                        pending.erase(id);
                        seen.insert(id);
                        file.deleteFile();
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
                    for (const auto& f : responses.findChildFiles(File::findFiles, false, "*.json"))
                        if (Time::currentTimeMillis() -
                                f.getLastModificationTime().toMilliseconds() >
                            120000)
                            f.deleteFile();
            } catch (const std::exception& e) {
                if (reported != e.what()) {
                    reported = e.what();
                    report(reported);
                }
            } catch (...) {
                const String fallback = "unexpected failure";
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
    dir.deleteRecursively();
}
}  // namespace flows::app
