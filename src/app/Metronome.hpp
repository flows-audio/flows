#pragma once
#include "StepSequencer.hpp"
#include <numbers>

namespace flows::app {
// A transport lane independent of note gates, swing and instrument preparation.
class Metronome {
    bool running_ = false;
    int division_ = 4;
    double origin_ = 0, expected_ = 0, tick_ = 0, phase_ = 0, age_ = 1, frequency_ = 1320;
    float gain_ = 0;
public:
    void process(bool enabled,const MetronomeSettings& settings,double rate,Transport transport,
                 std::span<float> left,std::span<float> right) noexcept {
        const bool playing=enabled&&transport.playing;
        const double increment=transport.bpm/(60*rate);
        if(playing && (!running_ || division_!=settings.division || std::abs(transport.ppq-expected_)>2*increment)) {
            origin_=transport.ppq;tick_=0;division_=settings.division;
        }
        running_=playing;expected_=transport.ppq+(playing?left.size()*increment:0);
        const float target=playing&&!settings.mute?static_cast<float>(std::pow(10.,settings.volume/20)):0;
        const float slew=static_cast<float>(1/(.005*rate));
        for(std::size_t i=0;i<left.size();++i) {
            const double beat=transport.ppq+i*increment;
            if(playing && beat+increment*.5>=origin_+tick_*4/division_) {
                frequency_=std::fmod(tick_*4/division_,4)<1e-8?1760:1320;
                ++tick_;age_=0;phase_=0;
            }
            gain_+=std::clamp(target-gain_,-slew,slew);
            float click=0;
            if(age_<.025) {
                const double attack=std::min(1.,age_/.0003),release=std::pow(1-age_/.025,3);
                click=static_cast<float>(std::sin(phase_)*attack*release)*gain_;
                phase_+=2*std::numbers::pi*frequency_/rate;age_+=1/rate;
            }
            left[i]+=click;right[i]+=click;
        }
    }
};
}
