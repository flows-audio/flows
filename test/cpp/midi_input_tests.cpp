#include "host/app/StandaloneMidi.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
#include <stdexcept>

using namespace juce;
using namespace flows::app;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Predicate> void waitFor(Predicate predicate, const char* message) {
    const auto end = Time::getMillisecondCounterHiRes() + 5000;
    while (!predicate()) {
        check(Time::getMillisecondCounterHiRes() < end, message);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, true);
    }
}
String deviceId(const String& name) {
    for (const auto& device : MidiInput::getAvailableDevices())
        if (device.name == name) return device.identifier;
    return {};
}
bool listed(const String& id) {
    for (const auto& device : MidiInput::getAvailableDevices())
        if (device.identifier == id) return true;
    return false;
}
int main() {
    ScopedJuceInitialiser_GUI gui;
    try {
        AudioDeviceManager manager;
        MidiMessageCollector collector;
        collector.reset(48000);
        int releases = 0;
        const auto name = "Flows MIDI test " + Uuid().toString();
        auto already = MidiOutput::createNewDevice(name);
        check(already != nullptr, "Cannot create virtual CoreMIDI source");
        String id;
        waitFor([&] { id = deviceId(name); return id.isNotEmpty(); }, "Virtual source was not listed");
        StandaloneMidi inputs(manager, collector, [&] { ++releases; });
        waitFor([&] { return manager.isMidiInputDeviceEnabled(id); }, "A keyboard attached before startup was not enabled");
        auto hot = MidiOutput::createNewDevice(name + " hot");
        check(hot != nullptr, "Cannot create hotplugged virtual source");
        String hotId;
        waitFor([&] { hotId = deviceId(name + " hot"); return hotId.isNotEmpty() && manager.isMidiInputDeviceEnabled(hotId); },
                "A keyboard attached after startup was not enabled automatically");
        hot->sendMessageNow(MidiMessage::noteOn(3, 60, static_cast<uint8>(91)));
        MidiBuffer received;
        waitFor([&] { collector.removeNextBlockOfMessages(received, 128); return !received.isEmpty(); },
                "No MIDI note reached the collector");
        auto message = (*received.begin()).getMessage();
        check(message.isNoteOn() && message.getChannel() == 3 && message.getNoteNumber() == 60
              && message.getVelocity() == 91, "MIDI channel, pitch or velocity changed in transit");
        received.clear();
        hot->sendMessageNow(MidiMessage::noteOff(3, 60));
        waitFor([&] { collector.removeNextBlockOfMessages(received, 128); return !received.isEmpty(); },
                "No MIDI release reached the collector");
        check((*received.begin()).getMessage().isNoteOff(), "MIDI release changed in transit");
        manager.setMidiInputDeviceEnabled(id, false);
        waitFor([&] { return releases == 1; }, "Disabling an input did not release held notes");
        auto extra = MidiOutput::createNewDevice(name + " extra");
        check(extra != nullptr, "Cannot create extra virtual source");
        waitFor([&] { auto extraId = deviceId(name + " extra"); return extraId.isNotEmpty() && manager.isMidiInputDeviceEnabled(extraId); },
                "Second source was not discovered");
        check(!manager.isMidiInputDeviceEnabled(id), "Device discovery re-enabled a disabled input");
        check(releases == 1, "Discovering another keyboard released notes");
        already.reset();
        waitFor([&] { return !listed(id) && releases == 1; }, "Unplugging a disabled keyboard released notes or did not disappear");
        manager.dispatchPendingMessages();
        check(releases == 1, "A manager notification after unplugging a disabled keyboard released notes");
        hot.reset();
        waitFor([&] { return releases == 2; }, "Unplugging an enabled keyboard did not release held notes");
        check(!manager.isMidiInputDeviceEnabled(hotId), "Unplugged keyboard stayed open in the device manager");
        extra.reset();
        std::cout << "CoreMIDI startup, hotplug, note/velocity routing, manual disable and unplug release passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
