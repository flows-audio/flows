#include "JuceServices.hpp"

#include "FlowsAssets.h"
#include "FlowsUi.hpp"
#include "app/Text.hpp"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace flows::app {
HttpResult JuceHttpClient::post(const std::string& url, const std::string& body,
                                const std::vector<std::pair<std::string, std::string>>& headers,
                                const std::function<bool()>& cancelled, int timeoutMs, std::size_t responseLimit) {
    using Clock = std::chrono::steady_clock;
    juce::WebInputStream stream(juce::URL(juce::String(url)).withPOSTData(juce::MemoryBlock(body.data(), body.size())), true);
    juce::String headerText;
    for (const auto& [name, value] : headers) headerText += juce::String(name) + ": " + juce::String(value) + "\r\n";
    stream.withExtraHeaders(headerText)
        .withCustomRequestCommand("POST")
        .withConnectionTimeout(timeoutMs)
        .withNumRedirectsToFollow(0);
    const auto started = Clock::now();
    std::atomic<bool> finished{false}, timedOut{false};
    std::jthread watchdog([&](std::stop_token stop) {
        while (!stop.stop_requested() && !finished.load()) {
            if (cancelled()) {
                stream.cancel();
                return;
            }
            if (Clock::now() - started >= std::chrono::milliseconds(timeoutMs)) {
                timedOut.store(true);
                stream.cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    const auto finish = [&] {
        finished.store(true);
        watchdog.request_stop();
    };
    HttpResult result;
    if (!stream.connect(nullptr)) {
        finish();
        result.error = cancelled() ? HttpResult::Error::cancelled
                                   : timedOut.load() ? HttpResult::Error::timeout : HttpResult::Error::connect;
        return result;
    }
    result.status = stream.getStatusCode();
    std::array<char, 8192> bytes{};
    while (!stream.isExhausted()) {
        if (cancelled()) {
            stream.cancel();
            finish();
            result.error = HttpResult::Error::cancelled;
            return result;
        }
        const int count = stream.read(bytes.data(), static_cast<int>(bytes.size()));
        if (count <= 0) break;
        if (result.body.size() + static_cast<std::size_t>(count) > responseLimit) {
            stream.cancel();
            finish();
            result.error = HttpResult::Error::tooLarge;
            return result;
        }
        result.body.append(bytes.data(), static_cast<std::size_t>(count));
    }
    finish();
    if (cancelled()) result.error = HttpResult::Error::cancelled;
    else if (timedOut.load()) result.error = HttpResult::Error::timeout;
    return result;
}

std::shared_ptr<const SampleData> JuceAudioDecoder::decode(const std::filesystem::path& file) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(juce::String(file.string()))));
    if (!reader) throw std::runtime_error("This audio format could not be decoded");
    const auto frames = reader->lengthInSamples;
    const auto channels = reader->numChannels;
    const auto rate = reader->sampleRate;
    if (channels < 1 || channels > 2 || frames <= 0 || !std::isfinite(rate) || rate < 8000 || rate > 384000 ||
        frames / rate > 600)
        throw std::runtime_error("Samples must be mono or stereo, 8–384 kHz, and no longer than ten minutes");
    if (static_cast<std::uint64_t>(frames) > ((maximumPortableSampleBytes - 4096) / 4) / channels)
        throw std::runtime_error("Decoded WAV exceeds the 64 MiB sample limit");
    auto decoded = std::make_shared<SampleData>();
    decoded->sample_rate = rate;
    decoded->left.resize(static_cast<std::size_t>(frames));
    if (channels == 2) decoded->right.resize(static_cast<std::size_t>(frames));
    juce::AudioBuffer<float> block(static_cast<int>(channels), 4096);
    for (juce::int64 start = 0; start < frames; start += 4096) {
        const auto count = static_cast<int>(std::min<juce::int64>(4096, frames - start));
        if (!reader->read(&block, 0, count, start, true, channels == 2))
            throw std::runtime_error("Audio decoding failed before the end of the sample");
        for (int sample = 0; sample < count; ++sample) {
            decoded->left[static_cast<std::size_t>(start + sample)] = block.getSample(0, sample);
            if (channels == 2) decoded->right[static_cast<std::size_t>(start + sample)] = block.getSample(1, sample);
        }
    }
    return decoded;
}
std::string JuceAudioDecoder::extensions() const {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::string result;
    for (const auto& pattern : juce::StringArray::fromTokens(formats.getWildcardForAllFormats(), ";", {})) {
        auto extension = pattern.trim().fromLastOccurrenceOf(".", false, false).toStdString();
        if (extension.empty() || extension == "*") continue;
        if (!result.empty()) result += ';';
        result += extension;
    }
    return result;
}

std::vector<BundledFile> bundledResources() {
    std::vector<BundledFile> result;
    for (int i = 0; i < FlowsAssets::namedResourceListSize; ++i) {
        int size = 0;
        const auto* bytes = FlowsAssets::getNamedResource(FlowsAssets::namedResourceList[i], size);
        if (!bytes) continue;
        const auto name = juce::File(juce::String(FlowsAssets::originalFilenames[i])).getFileName().toStdString();
        result.push_back({name, std::string_view(bytes, static_cast<std::size_t>(size))});
    }
    return result;
}
const std::vector<UiResource>& uiResources() {
    static const std::vector<UiResource> resources = [] {
        std::vector<UiResource> result;
        for (std::size_t i = 0; i < FlowsUi::count; ++i)
            result.push_back({FlowsUi::files[i].path, FlowsUi::files[i].mime,
                              std::string_view(reinterpret_cast<const char*>(FlowsUi::files[i].data), FlowsUi::files[i].size)});
        return result;
    }();
    return resources;
}
}  // namespace flows::app
