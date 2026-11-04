#pragma once
#include "Json.hpp"
#include "StepSequencer.hpp"

#include <cmath>
#include <stdexcept>

namespace flows::app {
inline const char* directionName(StepDirection direction) {
    switch (direction) {
        case StepDirection::forward: return "forward";
        case StepDirection::reverse: return "reverse";
        case StepDirection::pingpong: return "pingpong";
        case StepDirection::random: return "random";
    }
    return "forward";
}
inline const char* scaleName(StepScale scale) {
    switch (scale) {
        case StepScale::chromatic: return "chromatic";
        case StepScale::major: return "major";
        case StepScale::minor: return "minor";
        case StepScale::pentatonic: return "pentatonic";
    }
    return "chromatic";
}
inline json::Value sequenceJSON(const SequencePattern& pattern) {
    json::Array rows;
    for (std::size_t i = 0; i < pattern.rowCount; ++i) {
        const auto& row = pattern.rows[i];
        json::Array steps;
        for (const auto& step : row.steps)
            steps.push_back(json::object({{"active", step.active}, {"velocity", step.velocity}, {"gate", step.gate}}));
        rows.push_back(json::object(
            {{"note", row.note}, {"mute", row.mute}, {"volume", row.volume}, {"steps", std::move(steps)}}));
    }
    return json::object({{"metronome", json::object({{"mute", pattern.metronome.mute},
                                                      {"division", pattern.metronome.division},
                                                      {"volume", pattern.metronome.volume}})},
                         {"length", pattern.length},
                         {"division", pattern.division},
                         {"swing", pattern.swing},
                         {"direction", directionName(pattern.direction)},
                         {"root", pattern.root},
                         {"scale", scaleName(pattern.scale)},
                         {"transpose", pattern.transpose},
                         {"rows", std::move(rows)}});
}
inline SequencePattern parseSequence(const json::Value& value) {
    const auto invalid = [](const std::string& message) {
        throw std::runtime_error("Invalid sequencer pattern: " + message);
    };
    const auto number = [&](const json::Value& item, const char* name, double low, double high,
                            bool integer = false) {
        const auto& v = item[name];
        const auto n = v.asDouble();
        if (!v.isNumber() || !std::isfinite(n) || n < low || n > high || (integer && n != std::floor(n)))
            invalid(name);
        return n;
    };
    const auto boolean = [&](const json::Value& item, const char* name) {
        const auto& v = item[name];
        if (!v.isBool()) invalid(name);
        return v.asBool();
    };
    SequencePattern result;
    const auto& metronome = value["metronome"];
    if (!metronome.isNull()) {
        result.metronome.mute = boolean(metronome, "mute");
        result.metronome.volume = number(metronome, "volume", -60, 6);
        result.metronome.division = static_cast<int>(number(metronome, "division", 1, 32, true));
        const auto d = result.metronome.division;
        if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32) invalid("metronome division");
    }
    result.length = static_cast<int>(number(value, "length", 16, 32, true));
    result.division = static_cast<int>(number(value, "division", 4, 32, true));
    if (result.length != 16 && result.length != 32) invalid("length must be 16 or 32");
    if (result.division != 4 && result.division != 8 && result.division != 16 && result.division != 32)
        invalid("division must be 4, 8, 16 or 32");
    result.swing = number(value, "swing", 0, .6);
    result.root = static_cast<int>(number(value, "root", 0, 11, true));
    result.transpose = static_cast<int>(number(value, "transpose", -24, 24, true));
    const auto direction = json::str(value, "direction"), scale = json::str(value, "scale");
    if (direction == "forward")
        result.direction = StepDirection::forward;
    else if (direction == "reverse")
        result.direction = StepDirection::reverse;
    else if (direction == "pingpong")
        result.direction = StepDirection::pingpong;
    else if (direction == "random")
        result.direction = StepDirection::random;
    else
        invalid("direction");
    if (scale == "chromatic")
        result.scale = StepScale::chromatic;
    else if (scale == "major")
        result.scale = StepScale::major;
    else if (scale == "minor")
        result.scale = StepScale::minor;
    else if (scale == "pentatonic")
        result.scale = StepScale::pentatonic;
    else
        invalid("scale");
    const auto* list = value["rows"].getArray();
    if (!list || list->empty() || list->size() > sequencerRows) invalid("use 1 to 8 note rows");
    result.rowCount = list->size();
    for (std::size_t r = 0; r < result.rowCount; ++r) {
        const auto& lane = (*list)[r];
        auto& row = result.rows[r];
        row.note = static_cast<int>(number(lane, "note", 0, 127, true));
        row.mute = boolean(lane, "mute");
        row.volume = lane.has("volume") ? number(lane, "volume", -60, 6) : 0;
        const auto* entries = lane["steps"].getArray();
        if (!entries || entries->size() != sequencerSteps) invalid("each row must contain 32 steps");
        for (std::size_t s = 0; s < sequencerSteps; ++s) {
            const auto& step = (*entries)[s];
            row.steps[s] = {boolean(step, "active"), number(step, "velocity", 0, 1), number(step, "gate", .05, 1)};
        }
    }
    return result;
}
}  // namespace flows::app
