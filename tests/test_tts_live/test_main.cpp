#include <Arduino.h>
#include <unity.h>
#include <cloud_connection.h>
#include <tts_client.h>
#include <audio_output.h>

void setUp() {}
void tearDown() { digitalWrite(44, LOW); }

void test_tts_download_and_play() {
    const int networks = WiFi.scanNetworks();
    bool visible = false;
    for (int i = 0; i < networks; ++i) {
        if (WiFi.SSID(i) == kWIFI_SSID) {
            visible = true;
            Serial.printf("TARGET_WIFI RSSI=%d channel=%d\n", WiFi.RSSI(i), WiFi.channel(i));
        }
    }
    WiFi.scanDelete();
    Serial.printf("WIFI_SCAN networks=%d configured_network_visible=%d\n", networks, visible);
    TEST_ASSERT_TRUE(cloud::connectWifi());
    TEST_ASSERT_TRUE(cloud::syncTime());
    const uint32_t started = millis();
    TtsPlaybackMetrics metrics;
    TEST_ASSERT_TRUE(speakTts("你好，我是小D。现在测试语音合成和播放速度。", &metrics));
    TEST_ASSERT_GREATER_THAN(0, metrics.firstPlayMs);
    TEST_ASSERT_LESS_THAN(metrics.receiveMs, metrics.firstPlayMs);
    TEST_ASSERT_EQUAL_UINT(metrics.audioBytes, metrics.playback.playedBytes);
    TEST_ASSERT_EQUAL_UINT(0, metrics.playback.underruns);
    TEST_ASSERT_EQUAL_INT(LOW, digitalRead(44));
    Serial.printf("TTS_LIVE total_ms=%u\n", millis() - started);
}

void setup() {
    digitalWrite(44, LOW);
    pinMode(44, OUTPUT);
    Serial.begin(115200);
    delay(2000);
    cJSON_Hooks hooks = {ps_malloc, free};
    cJSON_InitHooks(&hooks);
    WiFi.mode(WIFI_STA);
    UNITY_BEGIN();
    RUN_TEST(test_tts_download_and_play);
    UNITY_END();
}
void loop() { delay(1000); }
