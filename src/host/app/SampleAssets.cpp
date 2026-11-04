#include "SampleAssets.hpp"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "WorkspaceLayout.hpp"

namespace flows::app {
namespace {
using namespace juce;
[[noreturn]] void error(const String& text) {
    throw std::runtime_error(text.toStdString());
}
bool safePath(const String& path) {
    if (path.isEmpty() || path.getNumBytesAsUTF8() > 4096 || File::isAbsolutePath(path) ||
        path.containsAnyOf("\\:"))
        return false;
    for (auto c : path)
        if (c < 0x20 || c == 0x7f) return false;
    for (const auto& part : StringArray::fromTokens(path, "/", {}))
        if (part.isEmpty() || part == "." || part == "..") return false;
    return !path.contains("//") && !path.endsWithChar('/');
}
bool contained(const File& root, const File& file) {
    std::error_code status;
    auto canonicalRoot =
        std::filesystem::weakly_canonical(root.getFullPathName().toStdString(), status);
    if (status) return false;
    auto canonicalFile =
        std::filesystem::weakly_canonical(file.getFullPathName().toStdString(), status);
    if (status) return false;
    auto relative = canonicalFile.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& part : relative)
        if (part == "..") return false;
    return true;
}
struct Literal {
    std::size_t from = 0, to = 0;
    char quote = '"';
    String value;
};
std::vector<Literal> literals(const String& source) {
    const auto text = source.toStdString();
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
            const auto decoded = String::fromUTF8(value.data(), static_cast<int>(value.size()));
            if (valid && safePath(decoded)) result.push_back({begin, i, quote, decoded});
            ++i;
        }
    }
    return result;
}
std::vector<String> orderedPaths(const std::vector<String>& sources) {
    std::set<String> seen;
    std::vector<String> result;
    for (const auto& source : sources)
        for (const auto& literal : literals(source))
            if (seen.insert(literal.value).second) result.push_back(literal.value);
    return result;
}
int64 modifiedStamp(const File& file) {
    std::error_code status;
    const auto stamp =
        std::filesystem::last_write_time(file.getFullPathName().toStdString(), status);
    return status ? 0 : static_cast<int64>(stamp.time_since_epoch().count());
}
MemoryBlock boundedRead(const File& file) {
    if (!file.existsAsFile() || file.getSize() <= 0 ||
        file.getSize() > static_cast<int64>(maximumPortableSampleBytes))
        error("Sample file must be nonempty and at most 64 MiB: " + file.getFileName());
    auto stream = file.createInputStream();
    if (!stream) error("Could not read sample: " + file.getFileName());
    MemoryBlock bytes;
    stream->readIntoMemoryBlock(bytes, maximumPortableSampleBytes + 1);
    if (stream->getStatus().failed() || bytes.getSize() > maximumPortableSampleBytes)
        error("Could not read complete sample: " + file.getFileName());
    return bytes;
}
void writeBytes(const File& file, const MemoryBlock& bytes) {
    if (!file.getParentDirectory().createDirectory()) error("Could not create the Samples folder");
    TemporaryFile temporary(file);
    auto stream = temporary.getFile().createOutputStream();
    if (!stream || !stream->write(bytes.getData(), bytes.getSize()))
        error("Could not write sample: " + file.getFileName());
    stream->flush();
    if (stream->getStatus().failed()) error("Could not flush sample: " + file.getFileName());
    stream.reset();
    // Publish without replacing an existing directory entry, even if another
    // plugin instance imported the same name while this file was being written.
    std::error_code status;
    std::filesystem::create_hard_link(temporary.getFile().getFullPathName().toStdString(),
                                      file.getFullPathName().toStdString(), status);
    if (status)
        error("Could not publish sample without overwriting an existing file: " +
              file.getFileName());
}
String hash(const MemoryBlock& bytes) {
    return SHA256(bytes.getData(), bytes.getSize()).toHexString();
}
String safeName(const File& source) {
    String result;
    for (auto c : source.getFileNameWithoutExtension()) {
        if (CharacterFunctions::isLetterOrDigit(c) || c == ' ' || c == '_' || c == '-' || c == '.')
            result += String::charToString(c);
        else
            result += "_";
        if (result.length() >= 80) break;
    }
    result = result.trim().trimCharactersAtStart(".");
    return result.isEmpty() ? "Sample" : result;
}
var description(const File& file, const String& path) {
    AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0 ||
        reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0)
        error("Unsupported or empty mono/stereo sample: " + path);
    return object({{"path", path},
                   {"name", file.getFileName()},
                   {"duration", reader->lengthInSamples / reader->sampleRate},
                   {"channels", static_cast<int>(reader->numChannels)},
                   {"sampleRate", reader->sampleRate}});
}
}  // namespace
std::set<juce::String> referencedSamplePaths(const std::vector<juce::String>& sources) {
    std::set<juce::String> result;
    for (const auto& source : sources)
        for (const auto& literal : literals(source)) result.insert(literal.value);
    return result;
}
juce::String remapSamplePaths(const juce::String& source, const SamplePathMap& paths) {
    auto result = source.toStdString();
    const auto quoted = literals(source);
    for (auto it = quoted.rbegin(); it != quoted.rend(); ++it)
        if (const auto replacement = paths.find(it->value); replacement != paths.end()) {
            std::string escaped;
            for (auto c : replacement->second.toStdString()) {
                if (c == it->quote || c == '\\') escaped += '\\';
                escaped += c;
            }
            result.replace(it->from, it->to - it->from, escaped);
        }
    return juce::String::fromUTF8(result.data(), static_cast<int>(result.size()));
}
juce::File SampleAssets::resolve(const juce::String& path) const {
    if (!safePath(path)) error("Sample paths must be portable relative WAV/AIFF paths: " + path);
    auto file = root_.getChildFile(path);
    if (!contained(root_, file)) error("Sample path escapes the workspace: " + path);
    return file;
}
SampleAssets::Cached& SampleAssets::entry(const juce::String& path) const {
    const auto file = resolve(path);
    const auto size = file.getSize(), modified = modifiedStamp(file);
    if (!file.existsAsFile() || size <= 0 || size > static_cast<int64>(maximumPortableSampleBytes))
        error("Sample is missing or exceeds 64 MiB: " + path);
    auto& cached = cache_[path];
    if (cached.size != size || cached.modified != modified || cached.description.isVoid()) {
        if (cached.encoded.isNotEmpty()) encodedBytes_ -= static_cast<std::size_t>(cached.size);
        cached = {};
        cached.size = size;
        cached.modified = modified;
        cached.description = description(file, path);
    }
    return cached;
}
juce::String SampleAssets::importWildcard() {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.getWildcardForAllFormats();
}
void SampleAssets::installBundled(const juce::String& path, const void* bytes, std::size_t size) {
    std::lock_guard lock(mutex_);
    const auto file = resolve(path);
    if (file.exists() || !path.startsWith("Samples/") || size > maximumPortableSampleBytes) return;
    writeBytes(file, MemoryBlock(bytes, size));
}
juce::String SampleAssets::importAudio(const juce::File& source, const juce::String& destination) {
    if (!source.existsAsFile() || source.getSize() > static_cast<int64>(maximumPortableSampleBytes))
        error("Import an audio file of at most 64 MiB");
    AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader(formats.createReaderFor(source));
    if (!reader) error("This audio format could not be decoded");
    const auto frames = reader->lengthInSamples;
    const auto channels = reader->numChannels;
    const auto rate = reader->sampleRate;
    if (channels < 1 || channels > 2 || frames <= 0 || !std::isfinite(rate) || rate < 8000 ||
        rate > 384000 || frames / rate > 600)
        error("Samples must be mono or stereo, 8–384 kHz, and no longer than ten minutes");
    if (static_cast<std::uint64_t>(frames) > ((maximumPortableSampleBytes - 4096) / 4) / channels)
        error("Decoded WAV exceeds the 64 MiB sample limit");
    std::lock_guard lock(mutex_);
    const auto folder = workspaceFolder(root_, destination);
    if (!contained(root_, folder) || !folder.createDirectory())
        error("Could not create a managed Samples folder inside the workspace");
    auto target = folder.getChildFile(safeName(source) + ".wav").getNonexistentSibling();
    TemporaryFile temporary(target);
    auto stream = temporary.getFile().createOutputStream();
    if (!stream) error("Could not create the imported sample");
    auto* output = stream.get();
    WavAudioFormat format;
    std::unique_ptr<AudioFormatWriter> writer(
        format.createWriterFor(stream.get(), rate, channels, 32, {}, 0));
    if (!writer) error("Could not create a WAV writer");
    stream.release();
    AudioBuffer<float> block(static_cast<int>(channels), 4096);
    for (int64 start = 0; start < frames; start += 4096) {
        const auto count = static_cast<int>(std::min<int64>(4096, frames - start));
        if (!reader->read(&block, 0, count, start, true, channels == 2))
            error("Audio decoding failed before the end of the sample");
        for (unsigned int channel = 0; channel < channels; ++channel)
            for (int sample = 0; sample < count; ++sample)
                if (!std::isfinite(block.getSample(static_cast<int>(channel), sample)))
                    error("Sample contains non-finite audio values");
        if (!writer->writeFromAudioSampleBuffer(block, 0, count))
            error("Could not write imported audio");
    }
    if (!writer->flush()) error("Could not finalize the imported WAV");
    output->flush();
    if (output->getStatus().failed()) error("Could not flush the imported WAV");
    writer.reset();
    // Validate the exact file the engine will load before publishing the asset.
    (void)load_sample_file(temporary.getFile().getFullPathName().toStdString());
    std::error_code publishStatus;
    std::filesystem::create_hard_link(temporary.getFile().getFullPathName().toStdString(),
                                      target.getFullPathName().toStdString(), publishStatus);
    if (publishStatus) error("Could not publish the sample without overwriting an existing file");
    const auto path = target.getRelativePathFrom(root_);
    (void)entry(path);
    return path;
}
juce::Array<juce::var> SampleAssets::catalogue() const {
    std::lock_guard lock(mutex_);
    Array<var> result;
    const auto folder = root_;
    if (!folder.exists() || !contained(root_, folder)) return result;
    std::error_code status;
    std::size_t visited = 0;
    std::filesystem::recursive_directory_iterator iterator(
        folder.getFullPathName().toStdString(),
        std::filesystem::directory_options::skip_permission_denied, status),
        end;
    for (; !status && iterator != end && visited++ < 8192 && result.size() < 4096;
         iterator.increment(status)) {
        const auto name = iterator->path().filename().string();
        if (!name.empty() && name.front() == '.') {
            if (iterator->is_directory(status)) iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file(status)) continue;
        const File file(String(iterator->path().string()));
        if (!file.hasFileExtension("wav;aif;aiff;aifc")) continue;
        try {
            result.add(entry(file.getRelativePathFrom(root_)).description);
        } catch (...) {
        }
    }
    return result;
}
bool SampleAssets::isManaged(const juce::String& path) const {
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
        const auto path = String(resource.path);
        auto& cached = entry(path);
        auto decoded = cached.decoded.lock();
        if (static_cast<std::size_t>(cached.size) > maximumPortableSampleBytes - fileBytes)
            error("Native instrument sample files exceed the 64 MiB portable asset budget");
        fileBytes += static_cast<std::size_t>(cached.size);
        if (!decoded) {
            try {
                decoded = load_sample_file(resolve(path).getFullPathName().toStdString());
                cached.decoded = decoded;
            } catch (const std::exception& exception) {
                error("Could not load sample " + path + ": " + exception.what());
            }
        }
        const auto size = (decoded->left.size() + decoded->right.size()) * sizeof(float);
        if (size > SampleLimits{}.max_bank_bytes - bytes)
            error("Decoded sample bank exceeds 256 MiB");
        bytes += size;
        result.push_back(std::move(decoded));
    }
    return result;
}
juce::var SampleAssets::bundleLocked(const std::vector<juce::String>& paths, bool strict) const {
    Array<var> result;
    std::size_t total = 0;
    if (strict && paths.size() > 64) error("Portable instruments may include at most 64 samples");
    for (const auto& path : paths) {
        String encoded;
        std::size_t size = 0;
        try {
            const auto file = resolve(path);
            auto& cached = entry(path);
            size = static_cast<std::size_t>(cached.size);
            if (cached.encoded.isEmpty()) {
                auto decoded = cached.decoded.lock();
                if (!decoded) {
                    decoded = load_sample_file(file.getFullPathName().toStdString());
                    cached.decoded = decoded;
                }
                if (encodedBytes_ + size > maximumPortableSampleBytes) {
                    for (auto& [_, item] : cache_) item.encoded.clear();
                    encodedBytes_ = 0;
                }
                const auto bytes = boundedRead(file);
                cached.encoded = Base64::toBase64(bytes.getData(), bytes.getSize());
                encodedBytes_ += bytes.getSize();
            }
            encoded = cached.encoded;
            // Keep the last valid file bytes independently of current metadata.
            // Deleting or corrupting a file cannot erase a playable checkpoint.
            auto old = retained_.find(path);
            if (old != retained_.end()) {
                retainedBytes_ -= old->second.first;
                retained_.erase(old);
            }
            for (auto it = retained_.begin();
                 retainedBytes_ + size > maximumPortableSampleBytes && it != retained_.end();) {
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
        result.add(object({{"path", path}, {"encoding", "base64"}, {"data", encoded}}));
    }
    return result;
}
juce::var SampleAssets::bundle(const std::vector<juce::String>& sources, bool strict) const {
    std::lock_guard lock(mutex_);
    return bundleLocked(orderedPaths(sources), strict);
}
std::shared_ptr<const juce::MemoryBlock> SampleAssets::serializedBundle(
    const std::vector<juce::String>& sources) const {
    std::lock_guard lock(mutex_);
    const auto paths = orderedPaths(sources);
    String key;
    for (const auto& path : paths) {
        key += path + ":";
        try {
            const auto file = resolve(path);
            key += String(file.getSize()) + ":" + String(modifiedStamp(file));
        } catch (...) {
            key += "unresolved";
        }
        key += "\n";
    }
    if (const auto found = serializedBundles_.find(key); found != serializedBundles_.end())
        if (auto cached = found->second.lock()) return cached;
    const auto text = JSON::toString(bundleLocked(paths, false), true);
    auto result = std::make_shared<const MemoryBlock>(text.toRawUTF8(), text.getNumBytesAsUTF8());
    for (auto it = serializedBundles_.begin(); it != serializedBundles_.end();) {
        if (it->second.expired())
            it = serializedBundles_.erase(it);
        else
            ++it;
    }
    serializedBundles_[key] = result;
    return result;
}
SamplePathMap SampleAssets::restore(const juce::var& assets) {
    if (assets.isVoid()) return {};
    const auto* list = assets.getArray();
    if (!list || list->size() > 64) error("Invalid embedded sample list (64 assets maximum)");
    struct Pending {
        String source, destination;
        MemoryBlock bytes;
        bool write = false;
    };
    std::vector<Pending> pending;
    std::set<String> seen;
    std::map<String, String> reserved;
    std::size_t total = 0;
    std::lock_guard lock(mutex_);
    for (const auto& asset : *list) {
        const auto path = str(asset, "path"), encoded = str(asset, "data");
        if (!safePath(path) || str(asset, "encoding") != "base64" ||
            !get(asset, "data").isString() || !seen.insert(path).second)
            error("Invalid or duplicate embedded sample path");
        if (static_cast<std::size_t>(encoded.length()) > (maximumPortableSampleBytes * 4 / 3 + 8))
            error("Embedded sample exceeds its encoded byte budget");
        MemoryOutputStream stream;
        if (!Base64::convertFromBase64(stream, encoded))
            error("Embedded sample has invalid base64 data");
        auto bytes = stream.getMemoryBlock();
        if (bytes.getSize() == 0 || bytes.getSize() > maximumPortableSampleBytes - total)
            error("Embedded sample assets exceed 64 MiB in total");
        total += bytes.getSize();
        // Decode before writing anything into the workspace. A temporary file
        // also lets the native and offline loaders use the same validation.
        TemporaryFile validation(path.endsWithIgnoreCase(".wav") ? ".wav" : ".aiff");
        writeBytes(validation.getFile(), bytes);
        (void)load_sample_file(validation.getFile().getFullPathName().toStdString());
        auto target = resolve(path);
        bool needsWrite = true;
        const auto identity = hash(bytes);
        const auto original = target;
        for (int index = 0;; ++index) {
            const auto destination = target.getRelativePathFrom(root_);
            if (const auto earlier = reserved.find(destination); earlier != reserved.end()) {
                if (earlier->second == identity) {
                    needsWrite = false;
                    break;
                }
            } else if (!target.exists())
                break;
            else if (target.existsAsFile() &&
                     target.getSize() == static_cast<int64>(bytes.getSize()) &&
                     hash(boundedRead(target)) == identity) {
                needsWrite = false;
                break;
            }
            const auto suffix =
                "-" + identity.substring(0, 12) + (index ? "-" + String(index + 1) : String());
            target = original.getSiblingFile(original.getFileNameWithoutExtension() + suffix +
                                             original.getFileExtension());
            if (index > 4096) error("Could not choose a unique embedded sample path");
        }
        const auto destination = target.getRelativePathFrom(root_);
        (void)resolve(destination);
        reserved[destination] = identity;
        pending.push_back({path, destination, std::move(bytes), needsWrite});
    }
    std::vector<File> created;
    created.reserve(pending.size());
    try {
        for (const auto& asset : pending)
            if (asset.write) {
                const auto file = resolve(asset.destination);
                writeBytes(file, asset.bytes);
                created.push_back(file);
            }
    } catch (...) {
        for (const auto& file : created) file.deleteFile();
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
