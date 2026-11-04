#pragma once

#include "compiler/program.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace flows {

// Decoded once on a worker thread; shared read-only by every voice and engine.
// An empty right channel denotes mono. The sampler duplicates mono to stereo.
struct SampleData {
    double sample_rate = 48000.0;
    std::vector<float> left, right;
    // Set only by the decoder after immutable PCM preparation. Manually built
    // resources leave this empty and their identity is computed when requested.
    std::string content_key;
};
using SampleBank = std::vector<std::shared_ptr<const SampleData>>;

struct SampleLimits {
    std::size_t max_file_bytes = 64 * 1024 * 1024;
    std::size_t max_decoded_bytes = 128 * 1024 * 1024;
    std::size_t max_bank_bytes = 256 * 1024 * 1024;
    double max_seconds = 600.0;
};

// Header facts used to list samples without decoding their audio.
struct SampleInfo {
    double sample_rate = 0;
    unsigned channels = 0;
    std::size_t frames = 0;
};

// WAV PCM / IEEE float and AIFF / AIFC PCM / IEEE float; one or two channels.
// Throws std::runtime_error for unreadable, unsupported or malformed resources.
// Never call these loading functions from the audio callback.
std::shared_ptr<const SampleData> load_sample_file(const std::filesystem::path& path,
                                                  SampleLimits limits = {});
// Decodes the same formats from memory, for hosts without a native file system
// path (browser uploads, embedded packages).
std::shared_ptr<const SampleData> load_sample_bytes(std::span<const unsigned char> bytes,
                                                   SampleLimits limits = {});
// Validates a sample's container and limits without decoding its audio.
SampleInfo probe_sample(std::span<const unsigned char> bytes, SampleLimits limits = {});
SampleInfo probe_sample_file(const std::filesystem::path& path, SampleLimits limits = {});
// Paths resolve against asset_root, or beside the source file when no root is
// supplied. Dot components and targets escaping that root are rejected.
SampleBank load_sample_bank(const Program& program,
                            const std::filesystem::path& asset_root = {},
                            SampleLimits limits = {});

// Stable PCM identity for off-thread structural comparison, not a security hash.
std::string sample_bank_key(const SampleBank& bank);

} // namespace flows
