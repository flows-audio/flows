#pragma once

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <vector>

namespace flows::app {
struct RetiredBundledExample {
    const char* name;
    const char* sha256;
};
inline constexpr std::array<RetiredBundledExample, 3> retiredBundledExamples{{
    {"filtered-saw.flows", "5c5b824e1ee46bd4e853a5afd2996342b27b5de3ae93481e878db747a72db42d"},
    {"poly-adsr.flows", "a878ad184e5e6bbf6c6cb714f00f3f72e5d0716d8b542489cc0a03c6745485b6"},
    {"mono-retrigger.flows", "15fb8d7fcb618571fd11f982d467f448e2aab0480ab87df9ce59726e8619c6d3"},
}};

// Only retire byte-identical factory copies. Open instruments, edited files and
// symlinks remain untouched. Backups keep their bytes and use a non-.flows suffix
// so the instrument explorer does not list them as additional examples.
inline std::size_t retireBundledExamples(const juce::File& workspace,
                                         const std::vector<juce::String>& openDocuments) {
    std::size_t moved = 0;
    for (const auto& example : retiredBundledExamples) {
        if (std::find(openDocuments.begin(), openDocuments.end(), example.name) !=
            openDocuments.end())
            continue;
        const auto file = workspace.getChildFile(example.name);
        if (!file.existsAsFile() || file.isSymbolicLink() || file.getSize() > 1024 * 1024) continue;
        auto input = file.createInputStream();
        if (!input) continue;
        juce::MemoryBlock bytes;
        input->readIntoMemoryBlock(bytes, 1024 * 1024 + 1);
        input.reset();
        if (bytes.getSize() > 1024 * 1024 || juce::SHA256(bytes).toHexString() != example.sha256)
            continue;
        const auto backups = workspace.getChildFile(".example-backups");
        if (backups.isSymbolicLink() || !backups.createDirectory().wasOk()) continue;
        const auto destination =
            backups.getChildFile(juce::String(example.name) + ".bak").getNonexistentSibling();
        if (file.moveFileTo(destination)) ++moved;
    }
    return moved;
}
}  // namespace flows::app
