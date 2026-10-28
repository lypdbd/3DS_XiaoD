#include <Arduino.h>
#include <driver/i2s.h>
#include <audio_output.h>
#include <algorithm>

namespace speaker {
namespace {
constexpr size_t kBlockFrames = 240;
constexpr int kVolumePercent = 80;

bool check(const char* operation, esp_err_t result) {
    if (result == ESP_OK) {
        return true;
    }
    Serial.printf("ERROR speaker %s: %s\n", operation, esp_err_to_name(result));
    return false;
}

class Driver {
public:
    ~Driver() { close(); }
    bool begin(uint32_t rate) {
        digitalWrite(44, LOW);
        i2s_config_t config = {};
        config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
        config.sample_rate = rate;
        config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
        config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        config.dma_buf_count = 4;
        config.dma_buf_len = 120;
        config.tx_desc_auto_clear = true;
        if (!check("install", i2s_driver_install(I2S_NUM_0, &config, 0, nullptr))) {
            return false;
        }
        active_ = true;
        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = 2;
        pins.ws_io_num = 4;
        pins.data_out_num = 1;
        pins.data_in_num = I2S_PIN_NO_CHANGE;
        return check("pins", i2s_set_pin(I2S_NUM_0, &pins)) &&
               check("zero DMA", i2s_zero_dma_buffer(I2S_NUM_0));
    }
    bool write(const int16_t* stereo, size_t frames) {
        size_t written = 0;
        const size_t bytes = frames * 4;
        if (!check("write", i2s_write(I2S_NUM_0, stereo, bytes, &written, pdMS_TO_TICKS(1000)))) {
            return false;
        }
        if (written != bytes) {
            Serial.println("ERROR incomplete speaker I2S write");
            return false;
        }
        return true;
    }
    bool silence() {
        const int16_t zeros[kBlockFrames * 2] = {};
        for (unsigned i = 0; i < 6; ++i) {
            if (!write(zeros, kBlockFrames)) {
                return false;
            }
        }
        // More silence than the DMA capacity; drain the tail before turning the amp off.
        delay(30);
        return true;
    }
    bool close() {
        digitalWrite(44, LOW);
        if (!active_) {
            return true;
        }
        active_ = false;
        return check("uninstall", i2s_driver_uninstall(I2S_NUM_0));
    }

private:
    bool active_ = false;
};

bool output(Driver& driver, const uint8_t* audio, size_t frames, uint32_t rate) {
    if (!driver.begin(rate) || !driver.silence()) {
        return false;
    }
    digitalWrite(44, HIGH);
    delay(10);
    const size_t fadeFrames = rate / 100;
    int16_t stereo[kBlockFrames * 2];
    for (size_t start = 0; start < frames; start += kBlockFrames) {
        const size_t count = std::min(kBlockFrames, frames - start);
        for (size_t i = 0; i < count; ++i) {
            int16_t raw;
            memcpy(&raw, audio + (start + i) * 2, 2);
            int32_t sample = static_cast<int32_t>(raw) * kVolumePercent / 100;
            const size_t edge = std::min(start + i, frames - 1 - start - i);
            if (edge < fadeFrames) {
                sample = sample * static_cast<int32_t>(edge) / static_cast<int32_t>(fadeFrames);
            }
            stereo[i * 2] = static_cast<int16_t>(sample);
            stereo[i * 2 + 1] = static_cast<int16_t>(sample);
        }
        if (!driver.write(stereo, count)) {
            return false;
        }
    }
    return driver.silence();
}
}  // namespace

bool playPcm(const uint8_t* audio, size_t bytes, uint32_t sampleRate) {
    digitalWrite(44, LOW);
    if (!audio || !bytes || bytes % 2 || sampleRate != 24000 ||
        (bytes >= 4 && (!memcmp(audio, "RIFF", 4) || !memcmp(audio, "ID3", 3)))) {
        Serial.println("ERROR speaker expects raw mono PCM16 at 24000 Hz");
        return false;
    }
    unsigned peak = 0;
    for (size_t i = 0; i < bytes; i += 2) {
        int16_t value;
        memcpy(&value, audio + i, 2);
        peak = std::max(peak, static_cast<unsigned>(abs(static_cast<int>(value))));
    }
    if (!peak) {
        Serial.println("ERROR received only silent PCM");
        return false;
    }
    Serial.printf("PLAY frames=%u duration_ms=%u raw_peak=%u volume=%d%%\n",
                  static_cast<unsigned>(bytes / 2),
                  static_cast<unsigned>(bytes / 2 * 1000 / sampleRate), peak, kVolumePercent);
    Driver driver;
    const bool played = output(driver, audio, bytes / 2, sampleRate);
    const bool closed = driver.close();
    Serial.printf("PLAY_END result=%s AMP=%d I2S_RELEASED=%d ms=%lu\n",
                  played && closed ? "PASS" : "FAIL", digitalRead(44), closed, millis());
    return played && closed;
}
}  // namespace speaker
