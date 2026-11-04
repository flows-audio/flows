#include "engine/sample.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <iomanip>
#include <span>
#include <stdexcept>
#include <string>

namespace flows {
namespace {
using Bytes = std::span<const unsigned char>;
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }
void require(bool condition, const char* message) { if (!condition) fail(message); }
std::uint16_t u16(Bytes bytes, std::size_t at, bool little) {
    require(at <= bytes.size() && bytes.size() - at >= 2, "Truncated sample header");
    return little ? std::uint16_t(bytes[at] | std::uint16_t(bytes[at + 1]) << 8) :
        std::uint16_t(std::uint16_t(bytes[at]) << 8 | bytes[at + 1]);
}
std::uint32_t u32(Bytes bytes, std::size_t at, bool little) {
    require(at <= bytes.size() && bytes.size() - at >= 4, "Truncated sample header");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t(bytes[at + i]) << (8 * (little ? i : 3 - i));
    return value;
}
std::uint64_t u64(Bytes bytes, std::size_t at, bool little) {
    require(at <= bytes.size() && bytes.size() - at >= 8, "Truncated sample header");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) value |= std::uint64_t(bytes[at + i]) << (8 * (little ? i : 7 - i));
    return value;
}
bool tag(Bytes bytes, std::size_t at, const char* name) {
    return at <= bytes.size() && bytes.size() - at >= 4 && std::equal(name, name + 4, bytes.begin() + at);
}
struct Format {
    unsigned channels = 0, bits = 0;
    double rate = 0;
    bool little = true, floating = false, unsigned8 = false;
    std::size_t frames = 0;
    Bytes audio;
};

Format wave(Bytes bytes) {
    require(bytes.size() >= 12 && tag(bytes, 0, "RIFF") && tag(bytes, 8, "WAVE"), "Expected a RIFF WAVE sample");
    const auto declared = std::uint64_t(u32(bytes, 4, true)) + 8;
    require(declared >= 12 && declared <= bytes.size(), "Truncated WAVE container");
    bytes = bytes.first(static_cast<std::size_t>(declared));
    Bytes fmt, audio;
    bool have_fmt = false, have_audio = false;
    for (std::size_t at = 12; at < bytes.size();) {
        require(bytes.size() - at >= 8, "Truncated WAVE chunk header");
        const auto size = std::size_t(u32(bytes, at + 4, true));
        require(size <= bytes.size() - at - 8, "Truncated WAVE chunk");
        const auto contents = bytes.subspan(at + 8, size);
        if (tag(bytes, at, "fmt ")) { require(!have_fmt, "Duplicate WAVE format chunk"); fmt = contents; have_fmt = true; }
        if (tag(bytes, at, "data")) { require(!have_audio, "Duplicate WAVE data chunk"); audio = contents; have_audio = true; }
        at += 8 + size;
        if (size & 1) { require(at < bytes.size(), "Missing WAVE chunk padding"); ++at; }
    }
    require(have_fmt && have_audio && fmt.size() >= 16, "WAVE requires format and data chunks");
    auto encoding = u16(fmt, 0, true);
    Format result;
    result.channels = u16(fmt, 2, true); result.rate = u32(fmt, 4, true); result.bits = u16(fmt, 14, true);
    if (encoding == 0xfffe) {
        require(fmt.size() >= 40 && u16(fmt, 16, true) >= 22 && std::size_t(u16(fmt, 16, true)) <= fmt.size() - 18,
                "Truncated WAVE extensible format");
        require(u16(fmt, 18, true) == result.bits, "Packed WAVE valid bits are not supported");
        const auto mask = u32(fmt, 20, true);
        require(mask == 0 || (result.channels == 1 && (mask == 1 || mask == 4)) || (result.channels == 2 && mask == 3),
                "WAVE channel layout must be mono or left/right stereo");
        constexpr unsigned char guid_tail[] = {0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        require(std::equal(std::begin(guid_tail), std::end(guid_tail), fmt.begin() + 28), "Unsupported WAVE subformat");
        const auto subformat = u32(fmt, 24, true);
        require(subformat == 1 || subformat == 3, "Unsupported WAVE encoding");
        encoding = static_cast<std::uint16_t>(subformat);
    }
    require(encoding == 1 || encoding == 3, "WAVE must use PCM or IEEE floating-point audio");
    result.floating = encoding == 3; result.unsigned8 = !result.floating && result.bits == 8;
    require(result.channels == 1 || result.channels == 2, "Samples must have one or two channels");
    require(result.floating ? (result.bits == 32 || result.bits == 64) :
        (result.bits == 8 || result.bits == 16 || result.bits == 24 || result.bits == 32), "Unsupported WAVE bit depth");
    const auto stride = result.channels * (result.bits / 8);
    require(u16(fmt, 12, true) == stride && std::uint64_t(u32(fmt, 8, true)) == std::uint64_t(result.rate) * stride,
            "Inconsistent WAVE sample stride or byte rate");
    require(audio.size() % stride == 0, "Incomplete WAVE audio frame");
    result.frames = audio.size() / stride; result.audio = audio;
    return result;
}

double extended_rate(Bytes bytes) {
    const auto exponent = u16(bytes, 8, false);
    const auto mantissa = u64(bytes, 10, false);
    require((exponent & 0x8000) == 0 && exponent > 0 && exponent < 0x7fff && (mantissa >> 63) != 0,
            "Invalid AIFF sample rate");
    return std::ldexp(static_cast<double>(mantissa), int(exponent) - 16383 - 63);
}
Format aiff(Bytes bytes) {
    require(bytes.size() >= 12 && tag(bytes, 0, "FORM") && (tag(bytes, 8, "AIFF") || tag(bytes, 8, "AIFC")),
            "Expected an AIFF or AIFC sample");
    const bool compressed = tag(bytes, 8, "AIFC");
    const auto declared = std::uint64_t(u32(bytes, 4, false)) + 8;
    require(declared >= 12 && declared <= bytes.size(), "Truncated AIFF container");
    bytes = bytes.first(static_cast<std::size_t>(declared));
    Bytes comm, sound;
    bool have_comm = false, have_sound = false;
    for (std::size_t at = 12; at < bytes.size();) {
        require(bytes.size() - at >= 8, "Truncated AIFF chunk header");
        const auto size = std::size_t(u32(bytes, at + 4, false));
        require(size <= bytes.size() - at - 8, "Truncated AIFF chunk");
        if (tag(bytes, at, "COMM")) { require(!have_comm, "Duplicate AIFF common chunk"); comm = bytes.subspan(at + 8, size); have_comm = true; }
        if (tag(bytes, at, "SSND")) { require(!have_sound, "Duplicate AIFF sound chunk"); sound = bytes.subspan(at + 8, size); have_sound = true; }
        at += 8 + size;
        if (size & 1) { require(at < bytes.size(), "Missing AIFF chunk padding"); ++at; }
    }
    require(have_comm && have_sound && comm.size() >= (compressed ? 22U : 18U) && sound.size() >= 8,
            "AIFF requires complete common and sound chunks");
    Format result;
    result.channels = u16(comm, 0, false); result.frames = u32(comm, 2, false);
    result.bits = u16(comm, 6, false); result.rate = extended_rate(comm); result.little = false;
    if (compressed) {
        if (tag(comm, 18, "sowt")) result.little = true;
        else if (tag(comm, 18, "fl32") || tag(comm, 18, "FL32")) { result.floating = true; require(result.bits == 32, "Invalid AIFC float depth"); }
        else if (tag(comm, 18, "fl64") || tag(comm, 18, "FL64")) { result.floating = true; require(result.bits == 64, "Invalid AIFC float depth"); }
        else require(tag(comm, 18, "NONE") || tag(comm, 18, "twos"), "Unsupported compressed AIFC encoding");
    }
    require(result.channels == 1 || result.channels == 2, "Samples must have one or two channels");
    require(result.floating || result.bits == 8 || result.bits == 16 || result.bits == 24 || result.bits == 32,
            "Unsupported AIFF bit depth");
    const auto offset = std::size_t(u32(sound, 0, false));
    require(offset <= sound.size() - 8, "Invalid AIFF sound offset");
    result.audio = sound.subspan(8 + offset);
    const auto stride = result.channels * (result.bits / 8);
    require(result.frames <= result.audio.size() / stride, "Truncated AIFF audio frames");
    result.audio = result.audio.first(result.frames * stride);
    return result;
}

float decode(Bytes bytes, std::size_t at, const Format& format) {
    if (format.floating) {
        const double value = format.bits == 32 ? double(std::bit_cast<float>(u32(bytes, at, format.little))) :
            std::bit_cast<double>(u64(bytes, at, format.little));
        require(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(), "Sample contains a non-finite or overflowing value");
        return static_cast<float>(value);
    }
    if (format.unsigned8) return (int(bytes[at]) - 128) / 128.0f;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < format.bits / 8; ++i)
        value |= std::uint64_t(bytes[at + i]) << (8 * (format.little ? i : format.bits / 8 - 1 - i));
    const auto sign = std::uint64_t(1) << (format.bits - 1);
    const auto signed_value = value >= sign ? std::int64_t(value) - std::int64_t(sign * 2) : std::int64_t(value);
    return static_cast<float>(double(signed_value) / double(sign));
}
class ContentHash {
public:
    void number(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) { hash ^= (value >> (8 * i)) & 255U; hash *= 1099511628211ULL; }
    }
    void text(const std::string& value) { number(value.size()); for (unsigned char ch : value) number(ch); }
    std::string key() const {
        std::ostringstream stream; stream << std::hex << std::setfill('0') << std::setw(16) << hash; return stream.str();
    }
private:
    std::uint64_t hash = 14695981039346656037ULL;
};
std::string sample_key(const SampleData& sample) {
    ContentHash hash;
    hash.number(std::bit_cast<std::uint64_t>(sample.sample_rate));
    hash.number(sample.left.size()); hash.number(sample.right.size());
    for (const auto value : sample.left) hash.number(std::bit_cast<std::uint32_t>(value));
    for (const auto value : sample.right) hash.number(std::bit_cast<std::uint32_t>(value));
    return hash.key();
}
bool inside(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto part = child.begin();
    for (auto base = root.begin(); base != root.end(); ++base, ++part)
        if (part == child.end() || *part != *base) return false;
    return true;
}
} // namespace

namespace {
Format parse_format(Bytes bytes) {
    require(bytes.size() >= 12, "Sample file is empty or truncated");
    return tag(bytes, 0, "RIFF") ? wave(bytes) : aiff(bytes);
}
void check_limits(const SampleLimits& limits) {
    require(limits.max_file_bytes >= 12 && limits.max_decoded_bytes >= sizeof(float) &&
            std::isfinite(limits.max_seconds) && limits.max_seconds > 0, "Invalid sample loading limits");
}
void check_format(const Format& format, const SampleLimits& limits) {
    require(std::isfinite(format.rate) && format.rate >= 100 && format.rate <= 768000, "Sample rate must be between 100 and 768000 Hz");
    require(format.frames > 0 && format.frames <= limits.max_decoded_bytes / (format.channels * sizeof(float)) &&
            double(format.frames) / format.rate <= limits.max_seconds, "Decoded sample is empty or exceeds the memory/duration limit");
}
std::vector<unsigned char> read_sample_file(const std::filesystem::path& path, const SampleLimits& limits) {
    std::error_code error;
    require(std::filesystem::is_regular_file(path, error) && !error, "Sample is not a regular file");
    const auto length = std::filesystem::file_size(path, error);
    require(!error && length >= 12 && length <= limits.max_file_bytes &&
            length <= std::uintmax_t(std::numeric_limits<std::streamsize>::max()), "Sample file is empty, truncated or exceeds the size limit");
    std::vector<unsigned char> storage(static_cast<std::size_t>(length));
    std::ifstream file(path, std::ios::binary);
    require(bool(file) && bool(file.read(reinterpret_cast<char*>(storage.data()), static_cast<std::streamsize>(length))), "Could not read the complete sample file");
    return storage;
}
} // namespace

SampleInfo probe_sample(std::span<const unsigned char> bytes, SampleLimits limits) {
    check_limits(limits);
    require(bytes.size() <= limits.max_file_bytes, "Sample file exceeds the size limit");
    const auto format = parse_format(bytes);
    check_format(format, limits);
    return {format.rate, format.channels, format.frames};
}

SampleInfo probe_sample_file(const std::filesystem::path& path, SampleLimits limits) {
    check_limits(limits);
    const auto storage = read_sample_file(path, limits);
    return probe_sample(storage, limits);
}

std::shared_ptr<const SampleData> load_sample_bytes(std::span<const unsigned char> bytes, SampleLimits limits) {
    check_limits(limits);
    require(bytes.size() >= 12 && bytes.size() <= limits.max_file_bytes, "Sample file is empty, truncated or exceeds the size limit");
    const auto format = parse_format(bytes);
    check_format(format, limits);
    auto decoded = std::make_shared<SampleData>();
    decoded->sample_rate = format.rate; decoded->left.resize(format.frames);
    if (format.channels == 2) decoded->right.resize(format.frames);
    const auto sample_bytes = format.bits / 8;
    for (std::size_t i = 0; i < format.frames; ++i) {
        decoded->left[i] = decode(format.audio, i * sample_bytes * format.channels, format);
        if (format.channels == 2) decoded->right[i] = decode(format.audio, (i * 2 + 1) * sample_bytes, format);
    }
    decoded->content_key = sample_key(*decoded);
    return decoded;
}

std::shared_ptr<const SampleData> load_sample_file(const std::filesystem::path& path, SampleLimits limits) {
    check_limits(limits);
    const auto storage = read_sample_file(path, limits);
    return load_sample_bytes(storage, limits);
}

SampleBank load_sample_bank(const Program& program, const std::filesystem::path& asset_root, SampleLimits limits) {
    SampleBank bank;
    if (program.samples.empty()) return bank;
    require(program.samples.size() <= 64, "An instrument may reference at most 64 sample resources");
    std::error_code error;
    std::filesystem::path base = asset_root;
    if (base.empty()) {
        require(!program.source_path.empty(), "Samples require a saved source path or asset root");
        const auto source = std::filesystem::absolute(std::filesystem::path(program.source_path), error);
        require(!error, "Could not resolve the instrument path");
        base = source.parent_path();
    }
    const auto root = std::filesystem::canonical(base, error);
    require(!error && std::filesystem::is_directory(root), "Could not resolve the sample root directory");
    std::map<std::filesystem::path, std::shared_ptr<const SampleData>> decoded;
    std::size_t total = 0;
    bank.reserve(program.samples.size());
    for (const auto& resource : program.samples) {
        try {
            require(!resource.path.empty() && resource.path.size() <= 4096 &&
                    resource.path.find('\0') == std::string::npos && resource.path.find('\\') == std::string::npos &&
                    resource.path.find(':') == std::string::npos, "Sample paths must be relative file paths using forward slashes");
            const std::filesystem::path relative(resource.path);
            require(!relative.is_absolute() && !relative.has_root_name(), "Sample paths must be relative to the asset root");
            for (const auto& component : relative)
                require(!component.empty() && component != "." && component != "..", "Sample paths cannot contain empty or dot components");
            require(resource.path.find("//") == std::string::npos, "Sample paths cannot contain empty components");
            const auto target = std::filesystem::canonical(root / relative, error);
            require(!error, "Sample file does not exist or cannot be resolved");
            require(inside(target, root), "Sample path escapes the instrument's allowed directory");
            auto found = decoded.find(target);
            if (found == decoded.end()) {
                auto item = load_sample_file(target, limits);
                const auto size = (item->left.size() + item->right.size()) * sizeof(float);
                require(size <= limits.max_bank_bytes && total <= limits.max_bank_bytes - size, "Sample bank exceeds the memory limit");
                total += size;
                found = decoded.emplace(target, std::move(item)).first;
            }
            bank.push_back(found->second);
        } catch (const std::exception& exception) {
            fail("Sample '" + resource.path + "': " + exception.what());
        }
    }
    return bank;
}
std::string sample_bank_key(const SampleBank& bank) {
    ContentHash hash;
    hash.number(bank.size());
    for (const auto& sample : bank) {
        require(bool(sample), "Sample bank contains a missing resource");
        hash.text(sample->content_key.empty() ? sample_key(*sample) : sample->content_key);
    }
    return hash.key();
}
} // namespace flows
