#pragma once
#include <stdint.h>

namespace assistant {
class SpeechGate {
public:
    static constexpr uint32_t kFrameMs = 30;
    static constexpr uint32_t kWaitMs = 5000;
    static constexpr uint32_t kAttackMs = 90;
    static constexpr uint32_t kMinimumSpeechMs = 240;
    static constexpr uint32_t kSilenceMs = 900;
    static constexpr uint32_t kMaximumMs = 20000;
    enum class Event { Waiting, Started, Recording, Complete, Timeout, TooShort, TooLong };

    void reset(uint32_t now) {
        opened_ = now;
        started_ = lastVoiceEnd_ = 0;
        consecutive_ = speechMs_ = 0;
        recording_ = false;
    }

    Event feed(bool speech, uint32_t frameStart) {
        if (!recording_) {
            if (speech) {
                if (!consecutive_) {
                    if (frameStart - opened_ >= kWaitMs) {
                        return Event::Timeout;
                    }
                    started_ = frameStart;
                }
                consecutive_ += kFrameMs;
                if (consecutive_ >= kAttackMs) {
                    recording_ = true;
                    speechMs_ = consecutive_;
                    lastVoiceEnd_ = frameStart + kFrameMs;
                    return Event::Started;
                }
            } else {
                consecutive_ = 0;
                if (frameStart + kFrameMs - opened_ >= kWaitMs) {
                    return Event::Timeout;
                }
            }
            return Event::Waiting;
        }
        if (frameStart + kFrameMs - started_ >= kMaximumMs) {
            return Event::TooLong;
        }
        if (speech) {
            speechMs_ += kFrameMs;
            lastVoiceEnd_ = frameStart + kFrameMs;
        } else if (frameStart + kFrameMs - lastVoiceEnd_ >= kSilenceMs) {
            return speechMs_ >= kMinimumSpeechMs ? Event::Complete : Event::TooShort;
        }
        return Event::Recording;
    }
    uint32_t speechMs() const { return speechMs_; }
    bool recording() const { return recording_; }

private:
    uint32_t opened_ = 0;
    uint32_t started_ = 0;
    uint32_t lastVoiceEnd_ = 0;
    uint32_t consecutive_ = 0;
    uint32_t speechMs_ = 0;
    bool recording_ = false;
};
}  // namespace assistant
