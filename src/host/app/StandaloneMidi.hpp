#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <map>
#include <set>
#include <functional>

namespace flows::app {
// Device ownership stays on the message thread. Audio receives only MIDI
// messages and a release request; it never enumerates or opens hardware.
class StandaloneMidi final : private juce::ChangeListener {
public:
    StandaloneMidi(juce::AudioDeviceManager& manager, juce::MidiInputCallback& receiver,
                   std::function<void()> release)
        : devices(manager), callback(receiver), releaseNotes(std::move(release)) {
        devices.addMidiInputDeviceCallback({}, &callback);
        devices.addChangeListener(this);
        connection = juce::MidiDeviceListConnection::make([this] { refresh(true); });
        refresh(true);
    }

    ~StandaloneMidi() override {
        connection.reset();
        devices.removeChangeListener(this);
        devices.removeMidiInputDeviceCallback({}, &callback);
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(false); }

    // `disabled` means the user turned a still-listed device off. A device that
    // disappears is closed and forgotten, so the same CoreMIDI id can be opened
    // again. An open that fails is omitted from `inputs` and retried on the next
    // device-list change, not on the change message that failure itself posts.
    void refresh(bool retryFailed) {
        std::map<juce::String, bool> current;
        bool lostInput = false;
        juce::StringArray closeIds;
        for (const auto& input : juce::MidiInput::getAvailableDevices()) {
            const auto id = input.identifier;
            const auto previous = inputs.find(id);
            bool enabled = devices.isMidiInputDeviceEnabled(id);
            if (previous != inputs.end() && previous->second && !enabled) {
                disabled.insert(id);
                lostInput = true;
            }
            if (enabled) {
                disabled.erase(id);
                openFailed.erase(id);
            }
            if (!disabled.contains(id) && !enabled && (retryFailed || !openFailed.contains(id))) {
                devices.setMidiInputDeviceEnabled(id, true);
                enabled = devices.isMidiInputDeviceEnabled(id);
                if (enabled) openFailed.erase(id);
                else openFailed.insert(id);
            }
            if (enabled || disabled.contains(id)) current.emplace(id, enabled);
        }
        for (const auto& [id, wasEnabled] : inputs) {
            if (current.contains(id) || !wasEnabled) continue;
            lostInput = true;
            closeIds.add(id);
        }
        inputs = std::move(current);
        for (const auto& id : closeIds) devices.setMidiInputDeviceEnabled(id, false);
        if (lostInput) releaseNotes();
    }

    juce::AudioDeviceManager& devices;
    juce::MidiInputCallback& callback;
    std::function<void()> releaseNotes;
    std::map<juce::String, bool> inputs;
    std::set<juce::String> disabled, openFailed;
    juce::MidiDeviceListConnection connection;
};
}
