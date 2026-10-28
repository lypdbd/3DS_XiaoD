#pragma once

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <local_config.h>
#include <tts_response.h>
#include <tts_root_ca.h>

inline bool fetchTts(const char* text, uint32_t sampleRate, TtsResponse& response) {
    if (!response.begin()) {
        Serial.printf("ERROR TTS %s\n", response.error());
        return false;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> body(cJSON_CreateObject(), cJSON_Delete);
    cJSON* params = body ? cJSON_AddObjectToObject(body.get(), "req_params") : nullptr;
    cJSON* format = params ? cJSON_AddObjectToObject(params, "audio_params") : nullptr;
    if (!format || !cJSON_AddStringToObject(params, "text", text) ||
        !cJSON_AddStringToObject(params, "speaker", kTTS_SPEAKER) ||
        !cJSON_AddStringToObject(format, "format", "pcm") ||
        !cJSON_AddNumberToObject(format, "sample_rate", sampleRate)) {
        Serial.println("ERROR TTS request JSON allocation failed");
        return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> payload(
        cJSON_PrintUnformatted(body.get()), cJSON_free);
    if (!payload) {
        Serial.println("ERROR TTS request JSON serialization failed");
        return false;
    }
    WiFiClientSecure tls;
    tls.setCACert(kTtsRootCa);
    tls.setHandshakeTimeout(12);
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(15000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(tls, kTTS_API_URL)) {
        Serial.println("ERROR TTS HTTPS initialization failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Api-Key", kTTS_API_KEY);
    http.addHeader("X-Api-Resource-Id", kTTS_RESOURCE_ID);
    Serial.printf("TTS POST TLS_VERIFY=ON voice=%s resource=%s text_bytes=%u\n",
                  kTTS_SPEAKER, kTTS_RESOURCE_ID, static_cast<unsigned>(strlen(text)));
    const unsigned long start = millis();
    response.markRequestStart(start);
    const int status = http.POST(reinterpret_cast<uint8_t*>(payload.get()),
                                 strlen(payload.get()));
    Serial.printf("HTTP status=%d headers_ms=%lu\n", status, millis() - start);
    if (status != HTTP_CODE_OK) {
        if (status < 0) {
            Serial.printf("HTTP error: %s\n", HTTPClient::errorToString(status).c_str());
        }
        http.end();
        Serial.println("ERROR TTS HTTP rejected; check status, TLS, key and resource");
        return false;
    }
    const int transferred = http.writeToStream(&response);
    http.end();
    if (transferred < 0 || !response.finish()) {
        Serial.printf("ERROR TTS %s code=%d transfer=%d\n",
                      response.error(), response.serviceCode(), transferred);
        return false;
    }
    Serial.printf("TTS COMPLETE code=20000000 bytes=%u chunks=%u request_ms=%lu\n",
                  static_cast<unsigned>(response.audioSize()),
                  static_cast<unsigned>(response.chunks()), millis() - start);
    Serial.printf("TTS_TIMING first_body_ms=%u first_pcm_ms=%u last_pcm_ms=%u "
                  "max_pcm_gap_ms=%u receive_cpu_us=%u decode_cpu_us=%u base64_cpu_us=%u "
                  "body_bytes=%u audio_ms=%u\n",
                  response.firstBodyMs(), response.firstAudioMs(), response.lastAudioMs(),
                  response.maxAudioGapMs(), response.processingUs(), response.decodingUs(), response.base64Us(),
                  static_cast<unsigned>(response.bodyBytes()),
                  static_cast<unsigned>(response.audioSize() / 2 * 1000 / sampleRate));
    return true;
}
