#pragma once
// JUCE implementations of the core's platform ports (macOS app and plugins).
#include "app/AIModels.hpp"
#include "app/AudioRuntime.hpp"
#include "app/Platform.hpp"
#include "app/SampleAssets.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>
#include <vector>

namespace flows::app {
// Keychain-backed secrets (AISecrets.mm).
AISecrets systemAISecrets();

class JuceHttpClient final : public HttpClient {
public:
    HttpResult post(const std::string& url, const std::string& body,
                    const std::vector<std::pair<std::string, std::string>>& headers, const std::function<bool()>& cancelled,
                    int timeoutMs, std::size_t responseLimit) override;
};

// Decodes every format JUCE's basic format manager registers (WAV, AIFF, FLAC,
// Ogg, MP3 and CoreAudio formats on macOS).
class JuceAudioDecoder final : public AudioDecoder {
public:
    std::shared_ptr<const SampleData> decode(const std::filesystem::path& file) override;
    std::string extensions() const override;
};

// Macro parameters that DAWs automate.
class JuceMacroParameters final : public MacroParameters {
public:
    explicit JuceMacroParameters(const std::array<juce::AudioParameterFloat*, 32>& parameters) : parameters_(parameters) {}
    float get(int slot) const noexcept override { return parameters_[static_cast<std::size_t>(slot)]->get(); }
    void set(int slot, float normalized) override {
        parameters_[static_cast<std::size_t>(slot)]->setValueNotifyingHost(normalized);
    }

private:
    const std::array<juce::AudioParameterFloat*, 32>& parameters_;
};

// Example instruments, samples and docs embedded in the binary (FlowsAssets).
std::vector<BundledFile> bundledResources();
// Built UI files embedded for the WebView: path ("/index.html", "/assets/x.js") to bytes.
struct UiResource {
    std::string path, mime;
    std::string_view bytes;
};
const std::vector<UiResource>& uiResources();
}  // namespace flows::app
