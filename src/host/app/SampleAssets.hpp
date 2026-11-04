#pragma once
#include <juce_audio_formats/juce_audio_formats.h>

#include <map>
#include <mutex>
#include <set>

#include "Json.hpp"
#include "engine/sample.hpp"

namespace flows::app {
inline constexpr std::size_t maximumPortableSampleBytes = 64 * 1024 * 1024;
inline constexpr std::size_t maximumInstrumentPackageBytes = 96 * 1024 * 1024;
using SamplePathMap = std::map<juce::String, juce::String>;
// These helpers understand quoted literals and comments, including incomplete
// drafts, so a recoverable sampler remains portable while its source is edited.
std::set<juce::String> referencedSamplePaths(const std::vector<juce::String>& sources);
juce::String remapSamplePaths(const juce::String& source, const SamplePathMap& paths);

class SampleAssets {
    struct Cached {
        juce::int64 size = 0, modified = 0;
        juce::var description;
        juce::String encoded;
        std::weak_ptr<const SampleData> decoded;
    };
    juce::File root_;
    mutable std::mutex mutex_;
    mutable std::map<juce::String, Cached> cache_;
    mutable std::size_t encodedBytes_ = 0, retainedBytes_ = 0;
    mutable std::map<juce::String, std::pair<std::size_t, juce::String>> retained_;
    mutable std::map<juce::String, std::weak_ptr<const juce::MemoryBlock>> serializedBundles_;
    juce::File resolve(const juce::String&) const;
    Cached& entry(const juce::String&) const;
    juce::var bundleLocked(const std::vector<juce::String>&, bool strict) const;

public:
    void setRoot(const juce::File& root) { root_ = root; }
    static juce::String importWildcard();
    juce::String importAudio(const juce::File& source, const juce::String& folder = "Samples");
    void installBundled(const juce::String& path, const void* bytes, std::size_t size);
    juce::Array<juce::var> catalogue() const;
    bool isManaged(const juce::String& path) const;
    SampleBank bank(const Program&) const;
    juce::var bundle(const std::vector<juce::String>& sources, bool strict = false) const;
    std::shared_ptr<const juce::MemoryBlock> serializedBundle(
        const std::vector<juce::String>& sources) const;
    SamplePathMap restore(const juce::var& assets);
};
}  // namespace flows::app
