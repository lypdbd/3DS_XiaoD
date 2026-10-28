#pragma once
#include <Arduino.h>
#include <I2S.h>

namespace sense {
class Microphone {
public:
    ~Microphone() { stop(); }
    bool begin() {
        stop();
        I2S.setAllPins(-1, 42, 41, -1, -1);
        if (!I2S.begin(PDM_MONO_MODE, 16000, 16)) {
            Serial.println("ERROR PDM microphone initialization failed");
            return false;
        }
        active_ = true;
        int16_t warmup[512];
        for (unsigned i = 0; i < 4; ++i) {
            if (!read(warmup, 512)) {
                stop();
                return false;
            }
        }
        return true;
    }
    bool read(int16_t* samples, size_t count) {
        if (!active_ || !samples || !count || count > 2048) {
            Serial.println("ERROR invalid PDM read state/buffer");
            return false;
        }
        size_t received = 0;
        const auto result = esp_i2s::i2s_read(esp_i2s::I2S_NUM_0, samples, count * 2,
                                             &received, pdMS_TO_TICKS(1000));
        if (result != ESP_OK || received != count * 2) {
            Serial.printf("ERROR PDM read result=%s bytes=%u/%u\n", esp_err_to_name(result),
                          static_cast<unsigned>(received), static_cast<unsigned>(count * 2));
            return false;
        }
        return true;
    }
    void stop() {
        if (active_) {
            I2S.end();
            active_ = false;
        }
    }
    bool active() const { return active_; }

private:
    bool active_ = false;
};
}  // namespace sense
