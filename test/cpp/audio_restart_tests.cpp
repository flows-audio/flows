// When audio (re)starts, the runtime resets voices but keeps the notes sent
// after the restart, such as the key press that starts browser audio. A panic
// still drops notes that have not played yet.
//
//   audio_restart_tests --repo <source dir>
#include "core_harness.hpp"

#include <iostream>

using namespace flows;
using namespace flows::testing;
namespace {
float loudest(AppCore& core, int blocks) {
    float peak = 0;
    for (int i = 0; i < blocks; ++i) {
        render(core, 1);
        peak = std::max(peak, core.audio().mixedPeak.load());
    }
    return peak;
}
void key(AppCore& core, const char* method, int id) {
    const auto document = core.snapshot()["document"]["id"];
    (void)core.request(method, json::object({{"noteId", id}, {"pitch", 60}, {"velocity", 0.9}, {"documentId", document}}));
}
}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3 && std::string(argv[1]) == "--repo", "Usage: audio_restart_tests --repo <source dir>");
        TemporaryFolder folder;
        const Bundle bundle(argv[2]);
        auto core = makeCore(folder.path, bundle);
        waitReady(*core);
        (void)core->request("document.new", json::object({{"name", "Restart"}}));
        waitReady(*core);
        check(loudest(*core, 8) < 1e-6f, "The new instrument sounded before any note");

        // The host restarts audio, then a note arrives before the first block.
        core->audio().prepare(48000, 128);
        key(*core, "keyboard.noteOn", 1);
        check(loudest(*core, 40) > 0.01f, "A note sent right after audio restarted was dropped");
        key(*core, "keyboard.noteOff", 1);
        (void)loudest(*core, 400);

        // Stopping and starting again (a suspended browser context) behaves the same.
        core->audio().release();
        core->audio().prepare(48000, 128);
        key(*core, "keyboard.noteOn", 2);
        check(loudest(*core, 40) > 0.01f, "A note sent right after audio resumed was dropped");
        key(*core, "keyboard.noteOff", 2);
        (void)loudest(*core, 400);

        // A panic drops a note that is still queued.
        key(*core, "keyboard.noteOn", 3);
        (void)core->request("keyboard.panic", json::Value::object());
        (void)loudest(*core, 4);
        check(loudest(*core, 36) < 1e-3f, "A panic did not drop a queued note");
        std::cout << "Audio restart passed: notes sent after a start or resume play; a panic drops queued notes.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Audio restart failed: " << error.what() << "\n";
        return 1;
    }
}
