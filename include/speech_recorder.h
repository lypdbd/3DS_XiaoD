#pragma once
#include <Arduino.h>
#include <esp_vad.h>
#include <cloud_protocol.h>
#include <speech_gate.h>
#include <audio_conditioning.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace assistant {
class SpeechRecorder {
public:
    static constexpr size_t kSamples = 480;
    static constexpr size_t kPreFrames = 10;
    static constexpr size_t kCapacity = (SpeechGate::kMaximumMs + 600) * 32;
    ~SpeechRecorder() { stop(); }

    bool begin(uint32_t now) {
        stop();
        vad_ = vad_create(VAD_MODE_3);
        audio_.reset(static_cast<uint8_t*>(ps_malloc(kCapacity)));
        if (!vad_ || !audio_) {
            Serial.println("ERROR VAD/audio allocation failed");
            stop();
            return false;
        }
        next_ = filled_ = bytes_ = voiceFrames_ = frames_ = 0;
        gate_.reset(now);
        dcBlocker_.reset();
        return true;
    }
    SpeechGate::Event feed(int16_t* samples, uint32_t frameStart) {
        int32_t sum = 0;
        double squares = 0;
        for (size_t i = 0; i < kSamples; ++i) {
            sum += samples[i];
            squares += static_cast<double>(samples[i]) * samples[i];
        }
        mean_ = static_cast<float>(sum) / kSamples;
        acRms_ = sqrt(std::max(0.0, squares / kSamples - static_cast<double>(mean_) * mean_));
        for (size_t i = 0; i < kSamples; ++i) {
            samples[i] = dcBlocker_.process(samples[i]);
        }
        const bool voice = vad_process(vad_, samples, 16000, SpeechGate::kFrameMs) == VAD_SPEECH;
        ++frames_;
        voiceFrames_ += voice;
        if (!gate_.recording()) {
            memcpy(pre_[next_], samples, kSamples * 2);
            next_ = (next_ + 1) % kPreFrames;
            filled_ = std::min(kPreFrames, filled_ + 1);
        }
        const auto event = gate_.feed(voice, frameStart);
        if (event == SpeechGate::Event::Started) {
            const size_t first = filled_ == kPreFrames ? next_ : 0;
            for (size_t i = 0; i < filled_; ++i) {
                memcpy(audio_.get() + bytes_, pre_[(first + i) % kPreFrames], kSamples * 2);
                bytes_ += kSamples * 2;
            }
        } else if (event == SpeechGate::Event::Recording || event == SpeechGate::Event::Complete) {
            if (bytes_ + kSamples * 2 > kCapacity) {
                Serial.println("ERROR recording buffer limit; refusing truncated question");
                return SpeechGate::Event::TooLong;
            }
            memcpy(audio_.get() + bytes_, samples, kSamples * 2);
            bytes_ += kSamples * 2;
        }
        return event;
    }
    cloud::Bytes take() { return std::move(audio_); }
    size_t size() const { return bytes_; }
    unsigned voiceFrames() const { return voiceFrames_; }
    unsigned frames() const { return frames_; }
    float mean() const { return mean_; }
    float acRms() const { return acRms_; }
    uint32_t speechMs() const { return gate_.speechMs(); }
    void stop() {
        if (vad_) {
            vad_destroy(vad_);
            vad_ = nullptr;
        }
        audio_.reset();
        bytes_ = 0;
    }

private:
    vad_handle_t vad_ = nullptr;
    SpeechGate gate_;
    DcBlocker dcBlocker_;
    cloud::Bytes audio_;
    int16_t pre_[kPreFrames][kSamples] = {};
    size_t next_ = 0;
    size_t filled_ = 0;
    size_t bytes_ = 0;
    unsigned voiceFrames_ = 0;
    unsigned frames_ = 0;
    float mean_ = 0;
    float acRms_ = 0;
};
}  // namespace assistant
