#pragma once
#include "Json.hpp"
#include "engine/sample.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace flows::app {
inline constexpr std::size_t maximumPortableSampleBytes = 64 * 1024 * 1024;
inline constexpr std::size_t maximumInstrumentPackageBytes = 96 * 1024 * 1024;
using SamplePathMap = std::map<std::string, std::string>;
// These helpers understand quoted literals and comments, including incomplete
// drafts, so a recoverable sampler remains portable while its source is edited.
std::set<std::string> referencedSamplePaths(const std::vector<std::string>& sources);
std::string remapSamplePaths(const std::string& source, const SamplePathMap& paths);

// Mono or stereo float WAV (IEEE float, 32-bit). An empty right channel is mono.
std::string encodeWavFloat32(double sampleRate, std::span<const float> left, std::span<const float> right);
// Stereo 24-bit PCM WAV for short previews.
std::string encodeWavPcm24(double sampleRate, std::span<const float> left, std::span<const float> right);

// Host decoder for formats beyond WAV/AIFF (MP3, FLAC, M4A...). The core reads
// WAV and AIFF itself; the browser host converts other formats before import.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // Planar decode; throws std::runtime_error with a user-facing message.
    virtual std::shared_ptr<const SampleData> decode(const std::filesystem::path& file) = 0;
    // ';'-separated extensions for file pickers, e.g. "wav;aif;mp3".
    virtual std::string extensions() const = 0;
};

class SampleAssets {
    struct Cached {
        std::int64_t size = 0, modified = 0;
        json::Value description;
        std::string encoded;
        std::weak_ptr<const SampleData> decoded;
    };
    std::filesystem::path root_;
    std::shared_ptr<AudioDecoder> decoder_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, Cached> cache_;
    mutable std::size_t encodedBytes_ = 0, retainedBytes_ = 0;
    mutable std::map<std::string, std::pair<std::size_t, std::string>> retained_;
    mutable std::map<std::string, std::weak_ptr<const std::string>> serializedBundles_;
    std::filesystem::path resolve(const std::string&) const;
    Cached& entry(const std::string&) const;
    json::Value bundleLocked(const std::vector<std::string>&, bool strict) const;

public:
    void setRoot(const std::filesystem::path& root) { root_ = root; }
    void setDecoder(std::shared_ptr<AudioDecoder> decoder) { decoder_ = std::move(decoder); }
    std::string importExtensions() const;
    // Converts a mono/stereo audio file to a managed float WAV inside `folder`.
    std::string importAudio(const std::filesystem::path& source, const std::string& folder = "Samples");
    void installBundled(const std::string& path, std::string_view bytes);
    json::Array catalogue() const;
    bool isManaged(const std::string& path) const;
    SampleBank bank(const Program&) const;
    json::Value bundle(const std::vector<std::string>& sources, bool strict = false) const;
    std::shared_ptr<const std::string> serializedBundle(const std::vector<std::string>& sources) const;
    SamplePathMap restore(const json::Value& assets);
};
}  // namespace flows::app
