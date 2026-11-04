#include "SampleAssets.hpp"

#include "Files.hpp"
#include "Text.hpp"
#include "WorkspaceLayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace flows::app {
namespace fs = std::filesystem;
namespace {
[[noreturn]] void error(const std::string& message) { throw std::runtime_error(message); }

bool safePath(const std::string& path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/' || text::containsAnyOf(path, "\\:")) return false;
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7f) return false;
    for (const auto& part : text::split(path, "/"))
        if (part.empty() || part == "." || part == "..") return false;
    return path.find("//") == std::string::npos && path.back() != '/';
}
struct Literal {
    std::size_t from = 0, to = 0;
    char quote = '"';
    std::string value;
};
std::vector<Literal> literals(const std::string& text) {
    std::vector<Literal> result;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = text[i];
        if (c == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (c != '"' && c != '\'') {
            ++i;
            continue;
        }
        const auto quote = c;
        const auto begin = ++i;
        std::string value;
        bool valid = true;
        while (i < text.size() && text[i] != quote && text[i] != '\n' && text[i] != '\r') {
            auto next = text[i++];
            if (next == '\\') {
                if (i == text.size()) {
                    valid = false;
                    break;
                }
                next = text[i++];
                if (next != '\\' && next != '"' && next != '\'') valid = false;
            }
            value += next;
        }
        if (i < text.size() && text[i] == quote) {
            const auto decoded = text::sanitizeUtf8(value);
            if (valid && safePath(decoded)) result.push_back({begin, i, quote, decoded});
            ++i;
        }
    }
    return result;
}
std::vector<std::string> orderedPaths(const std::vector<std::string>& sources) {
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const auto& source : sources)
        for (const auto& literal : literals(source))
            if (seen.insert(literal.value).second) result.push_back(literal.value);
    return result;
}
std::string boundedRead(const fs::path& file) {
    const auto size = files::size(file);
    if (size <= 0 || size > static_cast<std::int64_t>(maximumPortableSampleBytes))
        error("Sample file must be nonempty and at most 64 MiB: " + file.filename().string());
    auto bytes = files::tryRead(file, maximumPortableSampleBytes);
    if (!bytes) error("Could not read complete sample: " + file.filename().string());
    return *bytes;
}
void writeBytes(const fs::path& file, std::string_view bytes) {
    if (!files::createDirectories(file.parent_path())) error("Could not create the Samples folder");
    // Publish without replacing an existing directory entry, even if another
    // plugin instance imported the same name while this file was being written.
    if (!files::writeNew(file, bytes))
        error("Could not publish sample without overwriting an existing file: " + file.filename().string());
}
bool letterOrDigit(std::uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}
std::string safeName(const fs::path& source) {
    const auto stem = files::stem(source.filename().string());
    std::string result;
    std::size_t count = 0;
    for (std::size_t i = 0; i < stem.size() && count < 80;) {
        const auto c = static_cast<unsigned char>(stem[i]);
        std::size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + length > stem.size()) length = 1;
        if (length > 1 || letterOrDigit(c) || c == ' ' || c == '_' || c == '-' || c == '.')
            result.append(stem, i, length);
        else
            result += '_';
        i += length;
        ++count;
    }
    result = text::sanitizeUtf8(text::trim(result));
    while (!result.empty() && result.front() == '.') result.erase(result.begin());
    return result.empty() ? "Sample" : result;
}
json::Value description(const fs::path& file, const std::string& path) {
    SampleInfo info;
    try {
        info = probe_sample_file(file);
    } catch (const std::exception&) {
        error("Unsupported or empty mono/stereo sample: " + path);
    }
    if (!std::isfinite(info.sample_rate) || info.sample_rate <= 0 || info.channels < 1 || info.channels > 2 ||
        info.frames == 0)
        error("Unsupported or empty mono/stereo sample: " + path);
    return json::object({{"path", path},
                         {"name", file.filename().string()},
                         {"duration", static_cast<double>(info.frames) / info.sample_rate},
                         {"channels", static_cast<int>(info.channels)},
                         {"sampleRate", info.sample_rate}});
}
void putU16(std::string& out, std::uint16_t value) {
    out += static_cast<char>(value & 0xff);
    out += static_cast<char>(value >> 8);
}
void putU32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((value >> (8 * i)) & 0xff);
}
std::string wavHeader(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
                      std::uint32_t dataBytes) {
    std::string out;
    out += "RIFF";
    putU32(out, 36 + dataBytes + (dataBytes & 1));
    out += "WAVEfmt ";
    putU32(out, 16);
    putU16(out, format);
    putU16(out, channels);
    putU32(out, rate);
    putU32(out, rate * channels * (bits / 8));
    putU16(out, static_cast<std::uint16_t>(channels * (bits / 8)));
    putU16(out, bits);
    out += "data";
    putU32(out, dataBytes);
    return out;
}
}  // namespace

std::string encodeWavFloat32(double sampleRate, std::span<const float> left, std::span<const float> right) {
    const std::uint16_t channels = right.empty() ? 1 : 2;
    const auto frames = left.size();
    const auto dataBytes = static_cast<std::uint32_t>(frames * channels * 4);
    auto out = wavHeader(3, channels, static_cast<std::uint32_t>(std::llround(sampleRate)), 32, dataBytes);
    out.reserve(out.size() + dataBytes);
    for (std::size_t i = 0; i < frames; ++i) {
        putU32(out, std::bit_cast<std::uint32_t>(left[i]));
        if (channels == 2) putU32(out, std::bit_cast<std::uint32_t>(right[i]));
    }
    return out;
}
std::string encodeWavPcm24(double sampleRate, std::span<const float> left, std::span<const float> right) {
    const auto frames = left.size();
    const auto dataBytes = static_cast<std::uint32_t>(frames * 2 * 3);
    auto out = wavHeader(1, 2, static_cast<std::uint32_t>(std::llround(sampleRate)), 24, dataBytes);
    const auto put = [&](float value) {
        const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);
        auto sample = static_cast<std::int32_t>(std::lround(clamped * 8388607.0));
        const auto bits = static_cast<std::uint32_t>(sample);
        out += static_cast<char>(bits & 0xff);
        out += static_cast<char>((bits >> 8) & 0xff);
        out += static_cast<char>((bits >> 16) & 0xff);
    };
    for (std::size_t i = 0; i < frames; ++i) {
        put(left[i]);
        put(right.empty() ? left[i] : right[i]);
    }
    return out;
}

std::set<std::string> referencedSamplePaths(const std::vector<std::string>& sources) {
    std::set<std::string> result;
    for (const auto& source : sources)
        for (const auto& literal : literals(source)) result.insert(literal.value);
    return result;
}
std::string remapSamplePaths(const std::string& source, const SamplePathMap& paths) {
    auto result = source;
    const auto quoted = literals(source);
    for (auto it = quoted.rbegin(); it != quoted.rend(); ++it)
        if (const auto replacement = paths.find(it->value); replacement != paths.end()) {
            std::string escaped;
            for (auto c : replacement->second) {
                if (c == it->quote || c == '\\') escaped += '\\';
                escaped += c;
            }
            result.replace(it->from, it->to - it->from, escaped);
        }
    return result;
}

fs::path SampleAssets::resolve(const std::string& path) const {
    if (!safePath(path)) error("Sample paths must be portable relative WAV/AIFF paths: " + path);
    const auto file = root_ / path;
    if (!files::isInsideResolved(root_, file)) error("Sample path escapes the workspace: " + path);
    return file;
}
SampleAssets::Cached& SampleAssets::entry(const std::string& path) const {
    const auto file = resolve(path);
    const auto size = files::size(file), modified = files::modifiedStamp(file);
    if (size <= 0 || size > static_cast<std::int64_t>(maximumPortableSampleBytes))
        error("Sample is missing or exceeds 64 MiB: " + path);
    auto& cached = cache_[path];
    if (cached.size != size || cached.modified != modified || cached.description.isNull()) {
        if (!cached.encoded.empty()) encodedBytes_ -= static_cast<std::size_t>(cached.size);
        cached = {};
        cached.size = size;
        cached.modified = modified;
        cached.description = description(file, path);
    }
    return cached;
}
std::string SampleAssets::importExtensions() const {
    return decoder_ ? decoder_->extensions() : std::string("wav;aif;aiff;aifc");
}
void SampleAssets::installBundled(const std::string& path, std::string_view bytes) {
    std::lock_guard lock(mutex_);
    const auto file = resolve(path);
    if (files::exists(file) || !path.starts_with("Samples/") || bytes.size() > maximumPortableSampleBytes) return;
    writeBytes(file, bytes);
}
std::string SampleAssets::importAudio(const fs::path& source, const std::string& destination) {
    const auto sourceSize = files::size(source);
    if (sourceSize < 0 || sourceSize > static_cast<std::int64_t>(maximumPortableSampleBytes))
        error("Import an audio file of at most 64 MiB");
    std::shared_ptr<const SampleData> decoded;
    SampleLimits limits;
    limits.max_decoded_bytes = 1024ull * 1024 * 1024;
    if (decoder_) {
        decoded = decoder_->decode(source);
    } else if (files::hasExtension(source.string(), "wav;aif;aiff;aifc")) {
        try {
            decoded = load_sample_file(source, limits);
        } catch (const std::exception&) {
            error("This audio format could not be decoded");
        }
    } else
        error("This audio format could not be decoded");
    if (!decoded) error("This audio format could not be decoded");
    const auto frames = decoded->left.size();
    const unsigned channels = decoded->right.empty() ? 1 : 2;
    const auto rate = decoded->sample_rate;
    if (frames == 0 || !std::isfinite(rate) || rate < 8000 || rate > 384000 || frames / rate > 600)
        error("Samples must be mono or stereo, 8–384 kHz, and no longer than ten minutes");
    if (frames > ((maximumPortableSampleBytes - 4096) / 4) / channels)
        error("Decoded WAV exceeds the 64 MiB sample limit");
    for (const auto value : decoded->left)
        if (!std::isfinite(value)) error("Sample contains non-finite audio values");
    for (const auto value : decoded->right)
        if (!std::isfinite(value)) error("Sample contains non-finite audio values");
    const auto bytes = encodeWavFloat32(rate, decoded->left, decoded->right);
    // Validate the exact bytes the engine will load before publishing the asset.
    (void)load_sample_bytes(std::span(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
    std::lock_guard lock(mutex_);
    const auto folder = workspaceFolder(root_, destination);
    if (!files::isInsideResolved(root_, folder) || !files::createDirectories(folder))
        error("Could not create a managed Samples folder inside the workspace");
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto target = files::nonexistentSibling(folder / (safeName(source) + ".wav"));
        if (files::writeNew(target, bytes)) {
            const auto path = files::relative(root_, target);
            (void)entry(path);
            return path;
        }
    }
    error("Could not publish the sample without overwriting an existing file");
}
json::Array SampleAssets::catalogue() const {
    std::lock_guard lock(mutex_);
    struct Item {
        std::string path;
        json::Value description;
    };
    std::vector<Item> items;
    if (!files::isDirectory(root_)) return {};
    std::error_code status;
    std::size_t visited = 0;
    fs::recursive_directory_iterator iterator(root_, fs::directory_options::skip_permission_denied, status), end;
    for (; !status && iterator != end && visited++ < 8192 && items.size() < 4096; iterator.increment(status)) {
        const auto name = iterator->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            std::error_code ignored;
            if (iterator->is_directory(ignored)) iterator.disable_recursion_pending();
            continue;
        }
        std::error_code ignored;
        if (!iterator->is_regular_file(ignored) || !files::hasExtension(name, "wav;aif;aiff;aifc")) continue;
        const auto path = files::relative(root_, iterator->path());
        try {
            items.push_back({path, entry(path).description});
        } catch (...) {
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });
    json::Array result;
    result.reserve(items.size());
    for (auto& item : items) result.push_back(std::move(item.description));
    return result;
}
bool SampleAssets::isManaged(const std::string& path) const {
    if (!safePath(path)) return false;
    try {
        std::lock_guard lock(mutex_);
        (void)entry(path);
        return true;
    } catch (...) {
        return false;
    }
}
SampleBank SampleAssets::bank(const Program& program) const {
    std::lock_guard lock(mutex_);
    SampleBank result;
    std::size_t bytes = 0, fileBytes = 0;
    if (program.samples.size() > 64) error("An instrument may reference at most 64 samples");
    for (const auto& resource : program.samples) {
        const auto& path = resource.path;
        auto& cached = entry(path);
        auto decoded = cached.decoded.lock();
        if (static_cast<std::size_t>(cached.size) > maximumPortableSampleBytes - fileBytes)
            error("Native instrument sample files exceed the 64 MiB portable asset budget");
        fileBytes += static_cast<std::size_t>(cached.size);
        if (!decoded) {
            try {
                decoded = load_sample_file(resolve(path));
                cached.decoded = decoded;
            } catch (const std::exception& exception) {
                error("Could not load sample " + path + ": " + exception.what());
            }
        }
        const auto size = (decoded->left.size() + decoded->right.size()) * sizeof(float);
        if (size > SampleLimits{}.max_bank_bytes - bytes) error("Decoded sample bank exceeds 256 MiB");
        bytes += size;
        result.push_back(std::move(decoded));
    }
    return result;
}
json::Value SampleAssets::bundleLocked(const std::vector<std::string>& paths, bool strict) const {
    json::Array result;
    std::size_t total = 0;
    if (strict && paths.size() > 64) error("Portable instruments may include at most 64 samples");
    for (const auto& path : paths) {
        std::string encoded;
        std::size_t size = 0;
        try {
            const auto file = resolve(path);
            auto& cached = entry(path);
            size = static_cast<std::size_t>(cached.size);
            if (cached.encoded.empty()) {
                auto decoded = cached.decoded.lock();
                if (!decoded) {
                    decoded = load_sample_file(file);
                    cached.decoded = decoded;
                }
                if (encodedBytes_ + size > maximumPortableSampleBytes) {
                    for (auto& [_, item] : cache_) item.encoded.clear();
                    encodedBytes_ = 0;
                }
                const auto bytes = boundedRead(file);
                cached.encoded = text::base64Encode(bytes);
                encodedBytes_ += bytes.size();
            }
            encoded = cached.encoded;
            // Keep the last valid file bytes independently of current metadata.
            // Deleting or corrupting a file cannot erase a playable checkpoint.
            if (auto old = retained_.find(path); old != retained_.end()) {
                retainedBytes_ -= old->second.first;
                retained_.erase(old);
            }
            for (auto it = retained_.begin(); retainedBytes_ + size > maximumPortableSampleBytes && it != retained_.end();) {
                if (std::find(paths.begin(), paths.end(), it->first) == paths.end()) {
                    retainedBytes_ -= it->second.first;
                    it = retained_.erase(it);
                } else
                    ++it;
            }
            if (retainedBytes_ + size <= maximumPortableSampleBytes) {
                retained_[path] = {size, encoded};
                retainedBytes_ += size;
            }
        } catch (const std::exception&) {
            if (const auto retained = retained_.find(path); retained != retained_.end()) {
                size = retained->second.first;
                encoded = retained->second.second;
            } else if (strict)
                throw;
            else
                continue;
        }
        if (size > maximumPortableSampleBytes - total || result.size() >= 64) {
            if (strict) error("Portable sample assets exceed 64 MiB in total");
            continue;  // Keep the draft itself; unresolved paths stay diagnostic.
        }
        total += size;
        result.push_back(json::object({{"path", path}, {"encoding", "base64"}, {"data", std::move(encoded)}}));
    }
    return json::Value(std::move(result));
}
json::Value SampleAssets::bundle(const std::vector<std::string>& sources, bool strict) const {
    std::lock_guard lock(mutex_);
    return bundleLocked(orderedPaths(sources), strict);
}
std::shared_ptr<const std::string> SampleAssets::serializedBundle(const std::vector<std::string>& sources) const {
    std::lock_guard lock(mutex_);
    const auto paths = orderedPaths(sources);
    std::string key;
    for (const auto& path : paths) {
        key += path + ":";
        try {
            const auto file = resolve(path);
            key += std::to_string(files::size(file)) + ":" + std::to_string(files::modifiedStamp(file));
        } catch (...) {
            key += "unresolved";
        }
        key += "\n";
    }
    if (const auto found = serializedBundles_.find(key); found != serializedBundles_.end())
        if (auto cached = found->second.lock()) return cached;
    auto result = std::make_shared<const std::string>(json::stringify(bundleLocked(paths, false)));
    for (auto it = serializedBundles_.begin(); it != serializedBundles_.end();) {
        if (it->second.expired())
            it = serializedBundles_.erase(it);
        else
            ++it;
    }
    serializedBundles_[key] = result;
    return result;
}
SamplePathMap SampleAssets::restore(const json::Value& assets) {
    if (assets.isNull()) return {};
    const auto* list = assets.getArray();
    if (!list || list->size() > 64) error("Invalid embedded sample list (64 assets maximum)");
    struct Pending {
        std::string source, destination, bytes;
        bool write = false;
    };
    std::vector<Pending> pending;
    std::set<std::string> seen;
    std::map<std::string, std::string> reserved;
    std::size_t total = 0;
    std::lock_guard lock(mutex_);
    for (const auto& asset : *list) {
        const auto path = json::str(asset, "path");
        if (!safePath(path) || json::str(asset, "encoding") != "base64" || !asset["data"].isString() ||
            !seen.insert(path).second)
            error("Invalid or duplicate embedded sample path");
        const auto& encoded = asset["data"].string();
        if (encoded.size() > (maximumPortableSampleBytes * 4 / 3 + 8))
            error("Embedded sample exceeds its encoded byte budget");
        auto bytes = text::base64Decode(encoded);
        if (!bytes) error("Embedded sample has invalid base64 data");
        if (bytes->empty() || bytes->size() > maximumPortableSampleBytes - total)
            error("Embedded sample assets exceed 64 MiB in total");
        total += bytes->size();
        // Decode before writing anything into the workspace, with the same
        // loader the native and offline engines use.
        (void)load_sample_bytes(std::span(reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size()));
        auto target = resolve(path);
        bool needsWrite = true;
        const auto identity = text::sha256Hex(*bytes);
        const auto original = target;
        for (int index = 0;; ++index) {
            const auto destination = files::relative(root_, target);
            if (const auto earlier = reserved.find(destination); earlier != reserved.end()) {
                if (earlier->second == identity) {
                    needsWrite = false;
                    break;
                }
            } else if (!files::exists(target))
                break;
            else if (files::isFile(target) && files::size(target) == static_cast<std::int64_t>(bytes->size()) &&
                     text::sha256Hex(boundedRead(target)) == identity) {
                needsWrite = false;
                break;
            }
            const auto name = original.filename().string();
            const auto dot = name.rfind('.');
            const auto stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
            const auto extension = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
            const auto suffix = "-" + identity.substr(0, 12) + (index ? "-" + std::to_string(index + 1) : "");
            target = original.parent_path() / (stem + suffix + extension);
            if (index > 4096) error("Could not choose a unique embedded sample path");
        }
        const auto destination = files::relative(root_, target);
        (void)resolve(destination);
        reserved[destination] = identity;
        pending.push_back({path, destination, std::move(*bytes), needsWrite});
    }
    std::vector<fs::path> created;
    created.reserve(pending.size());
    try {
        for (const auto& asset : pending)
            if (asset.write) {
                const auto file = resolve(asset.destination);
                writeBytes(file, asset.bytes);
                created.push_back(file);
            }
    } catch (...) {
        for (const auto& file : created) files::remove(file);
        throw;
    }
    SamplePathMap result;
    for (const auto& asset : pending) result[asset.source] = asset.destination;
    cache_.clear();
    encodedBytes_ = 0;
    serializedBundles_.clear();
    return result;
}
}  // namespace flows::app
