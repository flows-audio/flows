// The standalone app restores unsaved drafts from .session.flowsinst. A large
// recovery file must still load, and one that cannot be read must be kept
// aside and reported instead of being overwritten by the next checkpoint.
//
//   recovery_tests --repo <source dir>
#include "core_harness.hpp"

#include <iostream>

using namespace flows;
using namespace flows::testing;
namespace {
const std::string marker = "# Recovered draft marker";

bool reported(AppCore& core, const std::string& text) {
    for (const auto& event : core.takeAgentEvents())
        if (json::str(event, "error").find(text) != std::string::npos) return true;
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3 && std::string(argv[1]) == "--repo", "Usage: recovery_tests --repo <source dir>");
        const Bundle bundle(argv[2]);
        TemporaryFolder folder;
        const auto recoveryFile = folder.path / "Workspace" / ".session.flowsinst";
        {
            // An unsaved edit; closing the core writes the recovery file.
            auto core = makeCore(folder.path, bundle, true);
            waitReady(*core);
            (void)core->request("document.new", json::object({{"name", "Draft"}}));
            waitReady(*core);
            const auto document = core->snapshot()["document"];
            (void)core->request("document.apply", json::object({{"documentId", document["id"]},
                                                                {"baseRevision", document["revision"]},
                                                                {"text", json::str(document, "text") + "\n" + marker + "\n"}}));
        }
        check(files::isFile(recoveryFile), "Closing the standalone core wrote no recovery file");

        // More separators than the parser's default limits, in a field the restore ignores.
        json::ParseLimits limits;
        limits.maxBytes = 512u * 1024 * 1024;
        limits.maxSeparators = limits.maxSeparatorsPerContainer = 64u * 1024 * 1024;
        auto recovery = json::parse(files::read(recoveryFile, limits.maxBytes), limits);
        json::Array padding(40000, json::Value(0));
        recovery.set("padding", json::Value(std::move(padding)));
        files::writeAtomic(recoveryFile, json::stringify(recovery));
        {
            auto core = makeCore(folder.path, bundle, true);
            check(json::str(core->snapshot()["document"], "text").find(marker) != std::string::npos,
                  "A large recovery file did not restore the draft");
            check(!files::isFile(folder.path / "Workspace" / ".session.unreadable.flowsinst"),
                  "A readable recovery file was set aside");
        }

        // An unreadable file is kept for recovery by hand and reported; a
        // later one is kept beside it rather than replacing it.
        const auto workspace = folder.path / "Workspace";
        files::writeAtomic(recoveryFile, "{ not recovery");
        {
            auto core = makeCore(folder.path, bundle, true);
            check(files::tryRead(workspace / ".session.unreadable.flowsinst", 1024) == std::string("{ not recovery"),
                  "Unreadable drafts were not kept aside");
            check(reported(*core, "kept as .session.unreadable.flowsinst"), "Unreadable drafts were not reported");
        }
        files::writeAtomic(recoveryFile, "{ also not recovery");
        {
            auto core = makeCore(folder.path, bundle, true);
            check(files::tryRead(workspace / ".session.unreadable.flowsinst", 1024) == std::string("{ not recovery"),
                  "A second unreadable file replaced the first");
            check(files::tryRead(workspace / ".session.unreadable(2).flowsinst", 1024) == std::string("{ also not recovery"),
                  "A second unreadable file was not kept under a new name");
            check(reported(*core, "kept as .session.unreadable(2).flowsinst"), "The second file's name was not reported");
        }
        std::cout << "Recovery passed: large recovery restores drafts; unreadable recovery is kept and reported.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Recovery failed: " << error.what() << "\n";
        return 1;
    }
}
