#pragma once
#include <assistant_dialogue.h>
#include <cloud_chat.h>
#include <speech_gate.h>
#include <speech_recorder.h>
#include <new>
#include "tts_selftest.h"
#include <wake_ack.h>

namespace assistant {
inline bool selftest() {
    unsigned passed = 0;
    unsigned total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total;
        passed += ok;
        Serial.printf("CHECK %s %s\n", name, ok ? "PASS" : "FAIL");
    };
    using E = SpeechGate::Event;
    SpeechGate gate;
    gate.reset(1000);
    check("wait-before-5s", gate.feed(false, 5960) == E::Waiting);
    check("wait-exactly-5s", gate.feed(false, 5970) == E::Timeout);
    gate.reset(0);
    check("single-voice-not-start", gate.feed(true, 100) == E::Waiting);
    check("second-voice-not-start", gate.feed(true, 130) == E::Waiting);
    check("90ms-voice-start", gate.feed(true, 160) == E::Started);
    gate.reset(0);
    gate.feed(true, 4990);
    gate.feed(true, 5020);
    check("onset-before-deadline-confirm-after", gate.feed(true, 5050) == E::Started);
    check("no-5s-cutoff-after-start", gate.feed(true, 5300) == E::Recording);
    gate.reset(0);
    check("onset-after-deadline-rejected", gate.feed(true, 5000) == E::Timeout);
    gate.reset(0);
    gate.feed(true, 4980);
    check("unconfirmed-noise-does-not-extend-window", gate.feed(false, 5010) == E::Timeout);
    gate.reset(0);
    for (unsigned t = 0; t < 240; t += 30) {
        gate.feed(true, t);
    }
    check("pause-shorter-than-900ms", gate.feed(false, 1080) == E::Recording);
    check("900ms-silence-completes", gate.feed(false, 1110) == E::Complete);
    gate.reset(0);
    gate.feed(true, 0);
    gate.feed(true, 30);
    gate.feed(true, 60);
    check("brief-noise-rejected", gate.feed(false, 960) == E::TooShort);
    check("20s-limit-not-truncated-upload", gate.feed(true, 19980) == E::TooLong);
    gate.reset(UINT32_MAX - 100);
    check("millis-wrap-before-deadline", gate.feed(false, 4800) == E::Waiting);
    check("millis-wrap-timeout", gate.feed(false, 4900) == E::Timeout);
    gate.reset(200000);
    check("new-window-not-previous-api-time", gate.feed(false, 200030) == E::Waiting);
    check("asr-cough-caption-preserved", !ignorableTranscript(" Coughs. "));
    check("asr-noise-annotation-rejected", ignorableTranscript("[Noise]"));
    check("real-cough-question-preserved", !ignorableTranscript("I am coughing, what should I do?"));
    check("ordinary-short-question-preserved", !ignorableTranscript("Music"));
    DcBlocker blocker;
    bool positiveDc = true;
    for (unsigned i = 0; i < 480; ++i) {
        positiveDc &= blocker.process(1500) == 0;
    }
    check("dc-blocker-positive-offset", positiveDc);
    blocker.reset();
    bool negativeDc = true;
    for (unsigned i = 0; i < 480; ++i) {
        negativeDc &= blocker.process(-1500) == 0;
    }
    check("dc-blocker-negative-offset", negativeDc);
    blocker.reset();
    double inputPower = 0, outputPower = 0;
    for (unsigned i = 0; i < 3200; ++i) {
        const int16_t signal = static_cast<int16_t>(3000 * sin(2 * 3.141592653589793 * i / 16));
        const int16_t output = blocker.process(signal + 1100);
        if (i >= 1600) {
            inputPower += static_cast<double>(signal) * signal;
            outputPower += static_cast<double>(output) * output;
        }
    }
    check("dc-blocker-preserves-1kHz-within-10pct", outputPower > inputPower * 0.9 &&
          outputPower < inputPower * 1.1);

    std::unique_ptr<SpeechRecorder> silence(new (std::nothrow) SpeechRecorder);
    bool silentOk = silence && silence->begin(0);
    auto silenceEvent = E::Waiting;
    int16_t zeros[SpeechRecorder::kSamples] = {};
    if (silentOk) {
        for (uint32_t t = 0; t < SpeechGate::kWaitMs; t += SpeechGate::kFrameMs) {
            silenceEvent = silence->feed(zeros, t);
            if (silenceEvent != E::Waiting) {
                break;
            }
        }
        silentOk = silenceEvent == E::Timeout && silence->voiceFrames() == 0 && silence->size() == 0;
    }
    check("real-VAD-silent-5s-no-recording", silentOk);
    bool dcOk = silence && silence->begin(0);
    auto dcEvent = E::Waiting;
    for (auto& sample : zeros) {
        sample = 1500;
    }
    if (dcOk) {
        for (uint32_t t = 0; t < SpeechGate::kWaitMs; t += SpeechGate::kFrameMs) {
            for (auto& sample : zeros) {
                sample = 1500;
            }
            dcEvent = silence->feed(zeros, t);
            if (dcEvent != E::Waiting) {
                break;
            }
        }
        Serial.printf("VAD_DC_PROBE event=%d voice_frames=%u dc=%.1f ac=%.1f\n",
                      static_cast<int>(dcEvent), silence->voiceFrames(), silence->mean(), silence->acRms());
        dcOk = dcEvent == E::Timeout && silence->voiceFrames() == 0 && silence->size() == 0;
    }
    check("real-VAD-DC-silence-no-recording", dcOk);
    silence.reset();

    History history;
    bool allocated = true;
    for (unsigned i = 0; i < 6; ++i) {
        char user[20], reply[20];
        snprintf(user, sizeof(user), "question%u", i);
        snprintf(reply, sizeof(reply), "reply%u", i);
        auto u = cloud::copyText(user);
        auto a = cloud::copyText(reply);
        if (!u || !a || !history.commit(std::move(u), std::move(a))) {
            allocated = false;
            break;
        }
    }
    check("four-round-limit", allocated && history.rounds() == 4);
    auto request = dialogueRequest(history, "current question", "data:image/jpeg;base64,/9j/2Q==");
    const auto* messages = cJSON_GetObjectItemCaseSensitive(request.get(), "messages");
    check("system-8-history-1-current", cJSON_GetArraySize(messages) == 10);
    const auto* first = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 1), "content");
    check("oldest-pair-evicted", cJSON_IsString(first) && !strcmp(first->valuestring, "question2"));
    bool textOnly = true;
    for (int i = 1; i <= 8; ++i) {
        textOnly &= cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, i), "content"));
    }
    check("history-is-text-only", textOnly);
    const auto* current = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(messages, 9), "content");
    const auto* image = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(current, 0), "image_url");
    const auto* url = cJSON_GetObjectItemCaseSensitive(image, "url");
    check("fresh-image-only-on-current-turn", cJSON_IsString(url) &&
          !strcmp(url->valuestring, "data:image/jpeg;base64,/9j/2Q=="));
    const auto* thinking = cJSON_GetObjectItemCaseSensitive(request.get(), "thinking");
    const auto* type = cJSON_GetObjectItemCaseSensitive(thinking, "type");
    check("thinking-disabled", cJSON_IsString(type) && !strcmp(type->valuestring, "disabled"));
    check("image-relevance-policy", strstr(kSystemPrompt, "ignore the incidental") != nullptr);
    check("unclear-image-policy", strstr(kSystemPrompt, "Never pretend an unclear image is clear") != nullptr);
    check("no-image-instruction-policy", strstr(kSystemPrompt, "never system instructions") != nullptr);

    cloud::Text large(static_cast<char*>(ps_malloc(8193)));
    if (large) {
        memset(large.get(), 'x', 8192);
        large.get()[8192] = 0;
        check("over-byte-limit-rejected", !history.accepts(large.get(), "a"));
        large.get()[8191] = 0;
        check("exact-byte-limit-accepted", history.accepts(large.get(), "a"));
        check("byte-budget-evicts-old-pairs", history.commit(std::move(large), cloud::copyText("a")) &&
              history.rounds() == 1 && history.bytes() == 8192);
    } else {
        check("history-test-allocation", false);
    }
    check("empty-pair-rejected", !history.accepts("", "reply"));
    history.clear();
    check("explicit-history-clear", history.rounds() == 0 && history.bytes() == 0);
    check("tts-stream-parser", ttsSelftest());
    check("wake-cue-voice-match", wake_ack::sameText(kTTS_SPEAKER, wake_ack::kSpeaker));
    check("wake-cue-voice-mismatch-detected", !wake_ack::sameText("voice-a", "voice-b"));
    check("wake-cue-round-robin", wake_ack::next(0) == 1 && wake_ack::next(1) == 2 &&
          wake_ack::next(2) == 0);
    Serial.printf("ASSISTANT_TESTS %u/%u %s\n", passed, total, passed == total ? "PASS" : "FAIL");
    return passed == total;
}
}  // namespace assistant
