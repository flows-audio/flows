#include "FlowsEditor.hpp"

#include "MacAppearance.hpp"

#include <cstring>

namespace flows::app {
namespace {
using Browser = juce::WebBrowserComponent;
// Serves every file of the built editor (see tools/embed_ui.py).
std::optional<Browser::Resource> resource(const juce::String& path) {
    const auto wanted = path == "/" ? juce::String("/index.html") : path.upToFirstOccurrenceOf("?", false, false);
    for (const auto& file : uiResources()) {
        if (wanted != juce::String(file.path)) continue;
        std::vector<std::byte> data(file.bytes.size());
        std::memcpy(data.data(), file.bytes.data(), file.bytes.size());
        return Browser::Resource{std::move(data), juce::String(file.mime)};
    }
    return {};
}
class LocalBrowser final : public Browser {
public:
    explicit LocalBrowser(const Options& o) : Browser(o) {}
    bool pageAboutToLoad(const juce::String& url) override {
        return url.startsWith(Browser::getResourceProviderRoot()) || url == "about:blank";
    }
    void newWindowAttemptingToLoad(const juce::String&) override {}
};
juce::var failure(const juce::var& id, const char* code, const std::string& message) {
    return object({{"id", id}, {"ok", false}, {"error", object({{"code", code}, {"message", juce::String(message)}})}});
}
}  // namespace
FlowsEditor::FlowsEditor(FlowsProcessor& processor) : AudioProcessorEditor(processor), processor_(processor) {
    setOpaque(!processor_.standalone);
    setResizable(true, true);
    setResizeLimits(960, 620, 2560, 1800);
    setSize(1440, 920);
    browser_ = std::make_unique<LocalBrowser>(
        Browser::Options()
            .withNativeIntegrationEnabled()
            .withKeepPageLoadedWhenBrowserIsHidden()
            .withResourceProvider(resource)
            .withNativeFunction("flowsRequest", [this](const auto& args, auto complete) { handle(args, std::move(complete)); }));
    addAndMakeVisible(*browser_);
    browser_->setBounds(getLocalBounds());
    browser_->goToURL(Browser::getResourceProviderRoot());
    startTimerHz(30);
}
FlowsEditor::~FlowsEditor() { stopTimer(); }
void FlowsEditor::resized() {
    if (browser_) browser_->setBounds(getLocalBounds());
}
void FlowsEditor::timerCallback() {
    if (!isShowing()) return;
    if (processor_.standalone && !processor_.windowTransparency.load())
        processor_.windowTransparency.store(prepareTransparentWindow(*this));
    if (const auto telemetry = processor_.telemetry(); telemetry.isObject())
        browser_->emitEventIfBrowserIsVisible("flowsTelemetry", telemetry);
    for (const auto& event : processor_.takeAgentEvents()) browser_->emitEventIfBrowserIsVisible("flowsAgent", event);
    if ((timerCount_++ % 6) == 0)
        if (const auto delta = stream_.next(processor_.snapshotValue()))
            browser_->emitEventIfBrowserIsVisible("flowsState", toJuce(*delta));
}
juce::var FlowsEditor::respond(const juce::var& id, const json::Value& result) {
    return object({{"id", id}, {"ok", true}, {"result", toJuce(result)}});
}
void FlowsEditor::handle(const juce::Array<juce::var>& args, Browser::NativeFunctionCompletion complete) {
    const auto request = args.isEmpty() ? juce::var() : args[0];
    const auto id = get(request, "id"), params = get(request, "params");
    const auto method = str(request, "method");
    if (method == "clipboard.write" || method == "browser.open") {
        try {
            if (method == "clipboard.write") {
                const auto text = get(params, "text");
                if (!text.isString() || text.toString().getNumBytesAsUTF8() > 2 * 1024 * 1024)
                    throw std::runtime_error("Copy requires text of at most 2 MiB.");
                juce::SystemClipboard::copyTextToClipboard(text.toString());
            } else {
                const auto address = str(params, "url");
                const juce::URL url(address);
                bool invalidCharacter = false;
                for (const auto character : address)
                    if (character <= 32 || character == 127 || character == '\\') invalidCharacter = true;
                const auto authority = address.fromFirstOccurrenceOf("://", false, false)
                                           .upToFirstOccurrenceOf("/", false, false)
                                           .upToFirstOccurrenceOf("?", false, false)
                                           .upToFirstOccurrenceOf("#", false, false);
                if (address.length() > 8192 || invalidCharacter || authority.containsChar('@') ||
                    !(address.startsWithIgnoreCase("https://") || address.startsWithIgnoreCase("http://")) ||
                    !url.isWellFormed() || url.getDomain().isEmpty())
                    throw std::runtime_error("Open a valid HTTP or HTTPS link without credentials.");
                if (!url.launchInDefaultBrowser()) throw std::runtime_error("Could not open the default browser.");
            }
            complete(object({{"id", id}, {"ok", true}, {"result", object({{"ok", true}})}}));
        } catch (const std::exception& error) {
            complete(failure(id, "assistant_action_failed", error.what()));
        }
        return;
    }
    if (method == "sample.import") {
        chooser_ = std::make_unique<juce::FileChooser>("Import an audio sample", juce::File(), processor_.importWildcard());
        auto safe = juce::Component::SafePointer<FlowsEditor>(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe, id, complete = std::move(complete)](const juce::FileChooser& chooser) mutable {
                                  if (!safe) return;
                                  try {
                                      const auto file = chooser.getResult();
                                      const auto result = file == juce::File()
                                                              ? safe->processor_.snapshotValue()
                                                              : fromJuce(safe->processor_.importSample(file));
                                      complete(safe->respond(id, result));
                                  } catch (const std::exception& error) {
                                      complete(failure(id, "sample_import_failed", error.what()));
                                  }
                              });
        return;
    }
    if (method == "project.export" || method == "project.import") {
        const bool exporting = method == "project.export";
        chooser_ = std::make_unique<juce::FileChooser>(exporting ? "Export instrument" : "Open instrument package",
                                                       juce::File(), "*.flowsinst");
        auto safe = juce::Component::SafePointer<FlowsEditor>(this);
        chooser_->launchAsync(
            (exporting ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                       : juce::FileBrowserComponent::openMode) |
                juce::FileBrowserComponent::canSelectFiles,
            [safe, id, exporting, complete = std::move(complete)](const juce::FileChooser& chooser) mutable {
                if (!safe) return;
                try {
                    auto file = chooser.getResult();
                    if (file == juce::File()) {
                        complete(safe->respond(id, safe->processor_.snapshotValue()));
                        return;
                    }
                    if (exporting) {
                        file = file.withFileExtension("flowsinst");
                        const auto text = safe->processor_.requestValue("project.exportText", {}).string();
                        juce::TemporaryFile temporary(file);
                        auto output = temporary.getFile().createOutputStream();
                        if (!output || !output->write(text.data(), text.size()))
                            throw std::runtime_error("Could not write instrument package");
                        output->flush();
                        if (output->getStatus().failed()) throw std::runtime_error("Could not flush instrument package");
                        output.reset();
                        if (!temporary.overwriteTargetFileWithTemporary())
                            throw std::runtime_error("Could not finish instrument export");
                    } else {
                        if (file.getSize() > static_cast<juce::int64>(maximumInstrumentPackageBytes))
                            throw std::runtime_error("Package exceeds 96 MiB limit");
                        auto input = file.createInputStream();
                        if (!input) throw std::runtime_error("Could not read instrument package");
                        juce::MemoryBlock bytes;
                        input->readIntoMemoryBlock(bytes, maximumInstrumentPackageBytes + 1);
                        if (input->getStatus().failed() || bytes.getSize() > maximumInstrumentPackageBytes)
                            throw std::runtime_error("Instrument package exceeds its read budget");
                        const auto text =
                            juce::String::createStringFromData(bytes.getData(), static_cast<int>(bytes.getSize())).toStdString();
                        safe->processor_.requestValue("project.importText", json::object({{"text", text}}));
                    }
                    complete(safe->respond(id, safe->processor_.snapshotValue()));
                } catch (const std::exception& e) {
                    complete(failure(id, "request_failed", e.what()));
                }
            });
        return;
    }
    try {
        complete(respond(id, processor_.requestValue(method.toStdString(), fromJuce(params))));
    } catch (const std::exception& e) {
        complete(failure(id, "request_failed", e.what()));
    }
}
}  // namespace flows::app
