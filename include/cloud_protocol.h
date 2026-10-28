#pragma once

#include <Arduino.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <memory>
#include <cstring>

namespace cloud {
struct Free {
    void operator()(void* ptr) const { free(ptr); }
};
using Bytes = std::unique_ptr<uint8_t, Free>;
using Text = std::unique_ptr<char, Free>;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

inline Json json(cJSON* value) { return Json(value, cJSON_Delete); }

inline cJSON* appendObject(cJSON* array) {
    cJSON* item = cJSON_CreateObject();
    if (!item) {
        return nullptr;
    }
    if (!cJSON_AddItemToArray(array, item)) {
        cJSON_Delete(item);
        return nullptr;
    }
    return item;
}

inline bool addTextMessage(cJSON* root, const char* role, const char* text) {
    auto* message = appendObject(cJSON_GetObjectItemCaseSensitive(root, "messages"));
    return message && cJSON_AddStringToObject(message, "role", role) &&
           cJSON_AddStringToObject(message, "content", text);
}

inline Json chatRequest(const char* model) {
    Json root = json(cJSON_CreateObject());
    if (!root || !cJSON_AddStringToObject(root.get(), "model", model) ||
        !cJSON_AddFalseToObject(root.get(), "stream") ||
        !cJSON_AddArrayToObject(root.get(), "messages")) {
        return json(nullptr);
    }
    return root;
}

inline bool addMediaMessage(cJSON* root, const char* uri, bool audio, const char* prompt) {
    auto* message = appendObject(cJSON_GetObjectItemCaseSensitive(root, "messages"));
    if (!message || !cJSON_AddStringToObject(message, "role", "user")) {
        return false;
    }
    auto* content = cJSON_AddArrayToObject(message, "content");
    auto* part = content ? appendObject(content) : nullptr;
    if (!part || !cJSON_AddStringToObject(part, "type", audio ? "input_audio" : "image_url")) {
        return false;
    }
    auto* media = cJSON_AddObjectToObject(part, audio ? "input_audio" : "image_url");
    if (!media || !cJSON_AddStringToObject(media, audio ? "data" : "url", uri)) {
        return false;
    }
    if (prompt) {
        auto* text = appendObject(content);
        if (!text || !cJSON_AddStringToObject(text, "type", "text") ||
            !cJSON_AddStringToObject(text, "text", prompt)) {
            return false;
        }
    }
    return true;
}

inline void le32(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

inline Bytes wav16k(const uint8_t* pcm, size_t bytes) {
    if (!pcm || !bytes || bytes % 2 || bytes > 32000 * 30) {
        return Bytes(nullptr);
    }
    Bytes wav(static_cast<uint8_t*>(ps_malloc(bytes + 44)));
    if (!wav) {
        return wav;
    }
    uint8_t* p = wav.get();
    memset(p, 0, 44);
    memcpy(p, "RIFF", 4);
    le32(p + 4, bytes + 36);
    memcpy(p + 8, "WAVEfmt ", 8);
    le32(p + 16, 16);
    p[20] = 1;
    p[22] = 1;
    le32(p + 24, 16000);
    le32(p + 28, 32000);
    p[32] = 2;
    p[34] = 16;
    memcpy(p + 36, "data", 4);
    le32(p + 40, bytes);
    memcpy(p + 44, pcm, bytes);
    return wav;
}

inline Text dataUri(const char* prefix, const uint8_t* data, size_t bytes) {
    if (!data || !bytes || bytes > 1024 * 1024) {
        return Text(nullptr);
    }
    const size_t offset = strlen(prefix);
    const size_t capacity = 4 * ((bytes + 2) / 3) + 1;
    Text text(static_cast<char*>(ps_malloc(offset + capacity)));
    if (!text) {
        return text;
    }
    memcpy(text.get(), prefix, offset);
    size_t encoded = 0;
    if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(text.get() + offset),
                              capacity, &encoded, data, bytes) != 0) {
        return Text(nullptr);
    }
    text.get()[offset + encoded] = '\0';
    return text;
}

// Receives de-chunked HTTP body, not raw WiFiClient bytes.
class JsonResponse : public Stream {
public:
    static constexpr size_t kCapacity = 64 * 1024;
    bool begin() {
        data_.reset(static_cast<char*>(ps_malloc(kCapacity + 1)));
        started_ = millis();
        return data_ != nullptr;
    }
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* data, size_t size) override {
        if (!data_ || failed_ || millis() - started_ > 60000 || size > kCapacity - size_) {
            failed_ = true;
            return 0;
        }
        memcpy(data_.get() + size_, data, size);
        size_ += size;
        data_.get()[size_] = '\0';
        return size;
    }
    Json parse() const {
        if (failed_ || !size_) {
            return json(nullptr);
        }
        return json(cJSON_ParseWithLengthOpts(data_.get(), size_ + 1, nullptr, true));
    }
    size_t size() const { return size_; }
    bool failed() const { return failed_; }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    Text data_;
    size_t size_ = 0;
    unsigned long started_ = 0;
    bool failed_ = false;
};

inline const char* chatContent(const cJSON* root, const char*& error) {
    error = nullptr;
    if (!cJSON_IsObject(root) || cJSON_HasObjectItem(root, "error")) {
        error = "invalid or error response";
        return nullptr;
    }
    const cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON* choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    const cJSON* finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    if (!cJSON_IsString(finish) || strcmp(finish->valuestring, "stop")) {
        error = "missing/abnormal finish_reason (truncated, filtered or tool response)";
        return nullptr;
    }
    const cJSON* message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    const cJSON* content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsString(content)) {
        error = "missing string message.content";
        return nullptr;
    }
    const char* text = content->valuestring;
    size_t size = strlen(text);
    if (size > 8192) {
        error = "message.content exceeds 8 KiB";
        return nullptr;
    }
    bool visible = false;
    for (size_t i = 0; i < size; ++i) {
        visible |= static_cast<unsigned char>(text[i]) > 32;
    }
    if (!visible) {
        error = "empty recognition or reply; no usable text";
        return nullptr;
    }
    return text;
}
}  // namespace cloud
