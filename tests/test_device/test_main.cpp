#include <Arduino.h>
#include <unity.h>
#include <audio_output.h>
#include <microphone.h>
#include <sense_input.h>
#include <wake_engine.h>
#include <hilexin.h>
#include <algorithm>
#include "assistant_selftest.h"

void setUp() {}
void tearDown() { digitalWrite(44, LOW); }

void test_assistant_logic() {
    TEST_ASSERT_TRUE(psramFound());
    TEST_ASSERT_TRUE(assistant::selftest());
}

void test_wake_model() {
    sense::WakeEngine wake;
    TEST_ASSERT_TRUE(wake.begin());
    static int16_t samples[2048] = {};
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(samples) / sizeof(samples[0]), wake.frameSamples());
    const size_t bytes = wake.frameSamples() * 2;
    unsigned silent = 0, positive = 0;
    for (unsigned i = 0; i < 16000U * 3 / wake.frameSamples(); ++i) {
        silent += wake.detect(samples) > 0;
        delay(1);
    }
    TEST_ASSERT_TRUE(wake.reset());
    for (size_t offset = 0; offset < sizeof(hilexin); offset += bytes) {
        memset(samples, 0, bytes);
        memcpy(samples, hilexin + offset, std::min(bytes, sizeof(hilexin) - offset));
        positive += wake.detect(samples) > 0;
        delay(1);
    }
    Serial.printf("WAKE_FIXTURE source=official_PCM silence=%u positive=%u\n", silent, positive);
    TEST_ASSERT_EQUAL_UINT(0, silent);
    TEST_ASSERT_GREATER_THAN(0, positive);
}

void test_local_audio_playback() {
    for (unsigned i = 0; i < wake_ack::kCount; ++i) {
        const auto& clip = wake_ack::kClips[i];
        TEST_ASSERT_TRUE(speaker::playPcm(clip.audio, clip.bytes));
        TEST_ASSERT_EQUAL_INT(LOW, digitalRead(44));
    }
}

void test_microphone_after_playback() {
    sense::Microphone microphone;
    TEST_ASSERT_TRUE(microphone.begin());
    int16_t samples[512] = {};
    const bool captured = microphone.read(samples, 512);
    microphone.stop();
    TEST_ASSERT_TRUE(captured);
    TEST_ASSERT_EQUAL_INT(LOW, digitalRead(44));
}

class TimedPcm : public speaker::PcmSource {
public:
    TimedPcm(uint32_t finishAfter, uint32_t failAfter = 0, unsigned copies = 3)
        : finishAfter_(finishAfter), failAfter_(failAfter) {
        const auto& clip = wake_ack::kClips[0];
        bytes_ = clip.bytes * copies;
        data_.reset(static_cast<uint8_t*>(ps_malloc(bytes_)));
        if (data_) {
            for (unsigned i = 0; i < copies; ++i) {
                memcpy(data_.get() + i * clip.bytes, clip.audio, clip.bytes);
            }
        }
    }
    const uint8_t* audio() const override { return data_.get(); }
    size_t publishedBytes() const override { return complete() ? bytes_ : 48000; }
    bool complete() const override { return millis() - started_ >= finishAfter_; }
    bool failed() const override { return failAfter_ && millis() - started_ >= failAfter_; }
    size_t bytes() const { return bytes_; }
    uint32_t started() const { return started_; }
private:
    std::unique_ptr<uint8_t, decltype(&free)> data_{nullptr, free};
    size_t bytes_ = 0;
    uint32_t started_ = millis();
    uint32_t finishAfter_;
    uint32_t failAfter_;
};

void test_streaming_overlap() {
    TimedPcm source(500);
    TEST_ASSERT_NOT_NULL(source.audio());
    speaker::StreamStats stats;
    TEST_ASSERT_TRUE(speaker::playStream(source, stats));
    TEST_ASSERT_LESS_THAN(500, stats.firstWriteMs - source.started());
    TEST_ASSERT_EQUAL_UINT(source.bytes(), stats.playedBytes);
    TEST_ASSERT_EQUAL_UINT(0, stats.underruns);
    TEST_ASSERT_EQUAL_INT(LOW, digitalRead(44));
}

void test_streaming_short_clip() {
    TimedPcm source(0, 0, 1);
    TEST_ASSERT_NOT_NULL(source.audio());
    speaker::StreamStats stats;
    TEST_ASSERT_TRUE(speaker::playStream(source, stats));
    TEST_ASSERT_EQUAL_UINT(source.bytes(), stats.playedBytes);
}

void test_streaming_error_and_starvation() {
    for (const uint32_t failAfter : {uint32_t(300), uint32_t(0)}) {
        TimedPcm source(3000, failAfter);
        TEST_ASSERT_NOT_NULL(source.audio());
        speaker::StreamStats stats;
        TEST_ASSERT_FALSE(speaker::playStream(source, stats));
        TEST_ASSERT_GREATER_THAN(0, stats.playedBytes);
        TEST_ASSERT_LESS_THAN(source.bytes(), stats.playedBytes);
        TEST_ASSERT_EQUAL_UINT(failAfter ? 0 : 1, stats.underruns);
        TEST_ASSERT_EQUAL_INT(LOW, digitalRead(44));
    }
    TtsResponse rejected;
    rejected.closeStream(false);
    speaker::StreamStats stats;
    TEST_ASSERT_FALSE(speaker::playStream(rejected, stats));
    TEST_ASSERT_EQUAL_UINT(0, stats.playedBytes);
}

void test_camera_after_audio() {
    TEST_ASSERT_TRUE(sense::captureJpeg([](const uint8_t*, size_t bytes) { return bytes > 0; }));
}

void setup() {
    digitalWrite(44, LOW);
    pinMode(44, OUTPUT);
    Serial.begin(115200);
    delay(2000);
    UNITY_BEGIN();
    cJSON_Hooks hooks = {ps_malloc, free};
    cJSON_InitHooks(&hooks);
    RUN_TEST(test_assistant_logic);
    RUN_TEST(test_wake_model);
    RUN_TEST(test_local_audio_playback);
    RUN_TEST(test_streaming_overlap);
    RUN_TEST(test_streaming_short_clip);
    RUN_TEST(test_streaming_error_and_starvation);
    RUN_TEST(test_microphone_after_playback);
    RUN_TEST(test_camera_after_audio);
    UNITY_END();
}

void loop() { delay(1000); }
