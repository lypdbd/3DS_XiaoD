#pragma once

#include <Arduino.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <memory>
#include <cstring>
#include <atomic>
#include <audio_output.h>

// HTTPClient removes HTTP chunk framing before writing to this NDJSON sink.
class TtsResponse : public Stream, public speaker::PcmSource {
public:
    static constexpr size_t kLineCapacity = 128 * 1024;
    static constexpr size_t kAudioCapacity = 24000 * 2 * 30;

    bool begin() {
        line_.reset(static_cast<char*>(ps_malloc(kLineCapacity)));
        audio_.reset(static_cast<uint8_t*>(ps_malloc(kAudioCapacity)));
        started_ = millis();
        return (line_ && audio_) || reject("PSRAM allocation failed");
    }
    void markRequestStart(uint32_t started) { started_ = started; }

    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t* data, size_t size) override {
        CpuTimer timer(processingUs_);
        if (failed() && !error_) {
            reject("stream aborted");
            return 0;
        }
        if (error_ || !line_ || !audio_) {
            return 0;
        }
        if (size && !bodyBytes_) {
            firstBodyMs_ = millis() - started_;
        }
        bodyBytes_ += size;
        size_t offset = 0;
        while (offset < size) {
            if (millis() - started_ > 45000) {
                reject("response exceeded 45 seconds");
                return offset;
            }
            const auto* newline = static_cast<const uint8_t*>(memchr(data + offset, '\n', size - offset));
            const size_t count = newline ? static_cast<size_t>(newline - (data + offset)) : size - offset;
            if (count > kLineCapacity - 1 - lineSize_) {
                reject("JSON line exceeds 128 KiB");
                return offset;
            }
            memcpy(line_.get() + lineSize_, data + offset, count);
            lineSize_ += count;
            offset += count;
            if (newline) {
                if (!consumeLine()) {
                    return offset;
                }
                ++offset;
            }
        }
        return size;
    }

    bool finish() {
        CpuTimer timer(processingUs_);
        if (failed() && !error_) {
            return reject("stream playback failed");
        }
        if (error_ || (lineSize_ && !consumeLine())) {
            return false;
        }
        if (!ended_) {
            return reject("missing successful end marker");
        }
        if (!audioSize_ || audioSize_ % 2 != 0) {
            return reject("empty or odd-length PCM");
        }
        return true;
    }

    const uint8_t* audio() const { return audio_.get(); }
    size_t publishedBytes() const override { return published_.load(std::memory_order_acquire); }
    bool complete() const override { return complete_.load(std::memory_order_acquire); }
    bool failed() const override { return failed_.load(std::memory_order_acquire); }
    void cancel() { failed_.store(true, std::memory_order_release); }
    void closeStream(bool success) {
        if (!success) {
            failed_.store(true, std::memory_order_release);
        }
        complete_.store(true, std::memory_order_release);
    }
    size_t audioSize() const { return audioSize_; }
    size_t chunks() const { return chunks_; }
    size_t bodyBytes() const { return bodyBytes_; }
    uint32_t firstBodyMs() const { return firstBodyMs_; }
    uint32_t firstAudioMs() const { return firstAudioMs_; }
    uint32_t lastAudioMs() const { return lastAudioMs_; }
    uint32_t maxAudioGapMs() const { return maxAudioGapMs_; }
    uint32_t processingUs() const { return processingUs_; }
    uint32_t decodingUs() const { return decodingUs_; }
    uint32_t base64Us() const { return base64Us_; }
    const char* error() const { return error_ ? error_ : "none"; }
    int serviceCode() const { return serviceCode_; }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    struct CpuTimer {
        uint32_t& elapsed;
        uint32_t started = micros();
        explicit CpuTimer(uint32_t& value) : elapsed(value) {}
        ~CpuTimer() { elapsed += micros() - started; }
    };
    struct Free {
        void operator()(void* ptr) const { free(ptr); }
    };
    std::unique_ptr<char, Free> line_;
    std::unique_ptr<uint8_t, Free> audio_;
    size_t lineSize_ = 0;
    size_t audioSize_ = 0;
    size_t chunks_ = 0;
    unsigned long started_ = 0;
    bool ended_ = false;
    const char* error_ = nullptr;
    int serviceCode_ = 0;
    size_t bodyBytes_ = 0;
    uint32_t firstBodyMs_ = 0;
    uint32_t firstAudioMs_ = 0;
    uint32_t lastAudioMs_ = 0;
    uint32_t maxAudioGapMs_ = 0;
    uint32_t processingUs_ = 0;
    uint32_t decodingUs_ = 0;
    uint32_t base64Us_ = 0;
    std::atomic<size_t> published_{0};
    std::atomic<bool> complete_{false};
    std::atomic<bool> failed_{false};

    bool reject(const char* message) {
        error_ = message;
        failed_.store(true, std::memory_order_release);
        return false;
    }

    bool consumeLine() {
        line_.get()[lineSize_] = '\0';
        const size_t length = lineSize_;
        lineSize_ = 0;
        if (length == 0 || (length == 1 && line_.get()[0] == '\r')) {
            return true;
        }
        if (ended_) {
            return reject("data after end marker");
        }
        CpuTimer timer(decodingUs_);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
            cJSON_ParseWithLengthOpts(line_.get(), length + 1, nullptr, true),
            cJSON_Delete);
        if (!root || !cJSON_IsObject(root.get())) {
            return reject("invalid JSON response");
        }
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(root.get(), "code");
        if (!cJSON_IsNumber(code)) {
            return reject("missing numeric status code");
        }
        if (code->valuedouble != 0 && code->valuedouble != 20000000) {
            serviceCode_ = code->valueint;
            return reject("TTS service error");
        }
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
        if (data && !cJSON_IsString(data) && !cJSON_IsNull(data)) {
            return reject("audio data must be Base64 text");
        }
        if (cJSON_IsString(data) && data->valuestring[0]) {
            size_t bytes = 0;
            const auto* encoded = reinterpret_cast<const uint8_t*>(data->valuestring);
            const size_t length = strlen(data->valuestring);
            int result;
            {
                CpuTimer timer(base64Us_);
                result = mbedtls_base64_decode(audio_.get() + audioSize_,
                                               kAudioCapacity - audioSize_, &bytes, encoded, length);
            }
            if (result == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
                return reject("audio exceeds 30 seconds");
            }
            if (result != 0 || !bytes) {
                return reject("invalid Base64 audio");
            }
            audioSize_ += bytes;
            // Publish only complete PCM samples; the buffer is append-only and never moves.
            published_.store(audioSize_ & ~size_t(1), std::memory_order_release);
            const uint32_t now = millis() - started_;
            if (!chunks_) {
                firstAudioMs_ = now;
            } else if (now - lastAudioMs_ > maxAudioGapMs_) {
                maxAudioGapMs_ = now - lastAudioMs_;
            }
            lastAudioMs_ = now;
            ++chunks_;
        }
        ended_ = code->valuedouble == 20000000;
        return true;
    }
};
