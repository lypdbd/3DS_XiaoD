#pragma once
#include <tts_response.h>
#include <algorithm>

inline bool ttsSelftest() {
    struct Test { const char* name; const char* input; bool valid; };
    const Test tests[] = {
        {"split-lines", "{\"code\":0,\"data\":\"AQACAA==\"}\r\n{\"code\":20000000}\n", true},
        {"final-no-newline", "{\"code\":20000000,\"data\":\"AQACAA==\"}", true},
        {"odd-chunk-boundaries", "{\"code\":0,\"data\":\"AQ==\"}\n{\"code\":20000000,\"data\":\"AA==\"}", true},
        {"missing-end", "{\"code\":0,\"data\":\"AQACAA==\"}\n", false},
        {"empty-audio", "{\"code\":20000000}\n", false},
        {"service-error", "{\"code\":45000030}\n", false},
        {"invalid-base64", "{\"code\":0,\"data\":\"!!!\"}\n", false},
        {"missing-code", "{\"data\":\"AQACAA==\"}\n", false},
        {"odd-pcm", "{\"code\":20000000,\"data\":\"AQ==\"}\n", false},
        {"invalid-json", "{bad}\n", false},
        {"after-end", "{\"code\":20000000,\"data\":\"AQACAA==\"}\n{\"code\":0}\n", false},
    };
    unsigned passed = 0, total = 0;
    for (const auto& test : tests) {
        for (const size_t chunk : {size_t(1), size_t(7), size_t(1024)}) {
            ++total;
            TtsResponse response;
            if (!response.begin()) {
                Serial.println("ERROR TTS selftest allocation");
                return false;
            }
            const size_t length = strlen(test.input);
            for (size_t offset = 0; offset < length;) {
                const size_t count = std::min(chunk, length - offset);
                if (response.write(reinterpret_cast<const uint8_t*>(test.input + offset), count) != count) {
                    break;
                }
                offset += count;
            }
            const bool valid = response.finish();
            response.closeStream(valid);
            const bool bytesOk = !valid || (response.audioSize() >= 2 &&
                                            response.audio()[0] == 1 && response.audio()[1] == 0);
            const bool metricsOk = !valid || (response.bodyBytes() == length &&
                response.firstBodyMs() <= response.firstAudioMs() &&
                response.firstAudioMs() <= response.lastAudioMs());
            const bool streamOk = response.complete() && response.failed() == !valid &&
                response.publishedBytes() == (response.audioSize() & ~size_t(1));
            const bool ok = valid == test.valid && bytesOk && metricsOk && streamOk;
            passed += ok;
            Serial.printf("TTS_PARSER %s chunk=%u %s\n", test.name, static_cast<unsigned>(chunk),
                          ok ? "PASS" : "FAIL");
        }
    }
    std::unique_ptr<uint8_t, decltype(&free)> large(
        static_cast<uint8_t*>(ps_malloc(TtsResponse::kLineCapacity)), free);
    if (!large) {
        Serial.println("ERROR TTS boundary test allocation");
        return false;
    }
    memset(large.get(), ' ', TtsResponse::kLineCapacity);
    constexpr char json[] = "{\"code\":20000000,\"data\":\"AQACAA==\"}";
    memcpy(large.get(), json, strlen(json));
    for (const size_t length : {TtsResponse::kLineCapacity - 1, TtsResponse::kLineCapacity}) {
        ++total;
        TtsResponse response;
        const bool initialized = response.begin();
        const bool accepted = initialized && response.write(large.get(), length) == length && response.finish();
        const bool ok = initialized && accepted == (length < TtsResponse::kLineCapacity);
        passed += ok;
        Serial.printf("TTS_PARSER line_bytes=%u %s\n", static_cast<unsigned>(length), ok ? "PASS" : "FAIL");
    }
    constexpr char prefix[] = "{\"code\":0,\"data\":\"";
    constexpr char suffix[] = "\"}\n";
    memcpy(large.get(), prefix, strlen(prefix));
    memset(large.get() + strlen(prefix), 'A', 96000);
    memcpy(large.get() + strlen(prefix) + 96000, suffix, strlen(suffix));
    const size_t lineBytes = strlen(prefix) + 96000 + strlen(suffix);
    constexpr char end[] = "{\"code\":20000000}\n";
    for (const bool overflow : {false, true}) {
        ++total;
        TtsResponse response;
        bool filled = response.begin();
        for (unsigned i = 0; filled && i < 20; ++i) {
            filled = response.write(large.get(), lineBytes) == lineBytes;
        }
        bool ok = filled && response.audioSize() == TtsResponse::kAudioCapacity;
        if (overflow) {
            ok = ok && response.write(large.get(), lineBytes) != lineBytes &&
                 !response.finish() && response.audioSize() == TtsResponse::kAudioCapacity;
        } else {
            ok = ok && response.write(reinterpret_cast<const uint8_t*>(end), strlen(end)) == strlen(end) &&
                 response.finish();
        }
        passed += ok;
        Serial.printf("TTS_PARSER audio_capacity overflow=%d %s\n", overflow, ok ? "PASS" : "FAIL");
    }
    Serial.printf("TTS_PARSER_TESTS %u/%u\n", passed, total);
    return passed == total;
}
