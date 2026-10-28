#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <local_config.h>
#include <cloud_protocol.h>
#include <stt_root_ca.h>

namespace cloud {
inline void printSafe(const char* label, const char* text) {
    String value(text);
    for (const char* secret : {kWIFI_PASSWORD, kSTT_API_KEY, kLLM_API_KEY, kTTS_API_KEY}) {
        if (*secret) {
            value.replace(secret, "[REDACTED]");
        }
    }
    value.replace("\r", "\\r");
    value.replace("\n", "\\n");
    Serial.printf("%s %s\n", label, value.c_str());
}

inline Text copyText(const char* text) {
    if (!text) {
        return Text(nullptr);
    }
    const size_t size = strlen(text) + 1;
    Text copy(static_cast<char*>(ps_malloc(size)));
    if (copy) {
        memcpy(copy.get(), text, size);
    }
    return copy;
}

inline bool postChat(const char* label, const char* url, const char* key, const char* ca,
                     const cJSON* body, Text& reply) {
    reply.reset();
    JsonResponse sink;
    if (!sink.begin()) {
        Serial.println("ERROR response PSRAM allocation failed");
        return false;
    }
    Text payload(cJSON_PrintUnformatted(body));
    if (!payload) {
        Serial.println("ERROR request serialization failed");
        return false;
    }
    WiFiClientSecure tls;
    tls.setCACert(ca);
    tls.setHandshakeTimeout(12);
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(30000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setReuse(false);
    const char* responseHeaders[] = {"x-request-id"};
    http.collectHeaders(responseHeaders, 1);
    if (!http.begin(tls, url)) {
        Serial.println("ERROR HTTPS initialization failed");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + key);
    const unsigned long start = millis();
    Serial.printf("%s POST TLS_VERIFY=ON request_bytes=%u\n",
                  label, static_cast<unsigned>(strlen(payload.get())));
    const int code = http.POST(reinterpret_cast<uint8_t*>(payload.get()), strlen(payload.get()));
    Serial.printf("%s HTTP=%d headers_ms=%lu\n", label, code, millis() - start);
    if (code < 0) {
        Serial.printf("ERROR HTTP %s\n", HTTPClient::errorToString(code).c_str());
        http.end();
        return false;
    }
    const int received = http.writeToStream(&sink);
    String requestId = http.header("x-request-id");
    http.end();
    if (requestId.length()) {
        printSafe("REQUEST_ID", requestId.c_str());
    }
    if (received < 0 || sink.failed()) {
        Serial.printf("ERROR response transfer=%d limit_exceeded=%d\n", received, sink.failed());
        return false;
    }
    Json root = sink.parse();
    if (!root) {
        Serial.println("ERROR invalid/truncated response JSON");
        return false;
    }
    const cJSON* serviceError = cJSON_GetObjectItemCaseSensitive(root.get(), "error");
    if (serviceError) {
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(serviceError, "message");
        if (cJSON_IsString(message)) {
            printSafe("API_ERROR", message->valuestring);
        }
    }
    if (code != HTTP_CODE_OK) {
        Serial.println("ERROR HTTP rejected; no reply accepted");
        return false;
    }
    const char* error = nullptr;
    const char* text = chatContent(root.get(), error);
    if (!text) {
        Serial.printf("ERROR %s\n", error);
        return false;
    }
    reply = copyText(text);
    if (!reply) {
        Serial.println("ERROR reply allocation failed");
        return false;
    }
    printSafe(label, reply.get());
    Serial.printf("%s DONE request_ms=%lu response_bytes=%u\n",
                  label, millis() - start, static_cast<unsigned>(sink.size()));
    return true;
}

inline bool transcribe(const uint8_t* pcm, size_t bytes, Text& text) {
    Bytes wav = wav16k(pcm, bytes);
    if (!wav) {
        Serial.println("ERROR invalid PCM or WAV allocation failed");
        return false;
    }
    Text uri = dataUri("data:audio/wav;base64,", wav.get(), bytes + 44);
    wav.reset();
    Json request = chatRequest(kSTT_MODEL);
    if (!uri || !request || !addMediaMessage(request.get(), uri.get(), true, nullptr)) {
        Serial.println("ERROR STT request allocation failed");
        return false;
    }
    uri.reset();
    Serial.printf("STT AUDIO rate=16000 bytes=%u duration_ms=%u\n",
                  static_cast<unsigned>(bytes), static_cast<unsigned>(bytes / 32));
    return postChat("STT", kSTT_API_URL, kSTT_API_KEY, kSttRootCa, request.get(), text);
}
}  // namespace cloud
