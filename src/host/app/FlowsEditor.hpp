#pragma once
#include <juce_gui_extra/juce_gui_extra.h>

#include "FlowsProcessor.hpp"
#include "app/Protocol.hpp"

namespace flows::app {
class FlowsEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit FlowsEditor(FlowsProcessor&);
    ~FlowsEditor() override;
    void resized() override;

private:
    FlowsProcessor& processor_;
    std::unique_ptr<juce::WebBrowserComponent> browser_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::uint64_t timerCount_ = 0;
    // Tracks what the page has, so state events carry only changed slices.
    StateStream stream_;
    void timerCallback() override;
    void handle(const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion);
    juce::var respond(const juce::var& id, const json::Value& result);
};
}  // namespace flows::app
