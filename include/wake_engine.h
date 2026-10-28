#pragma once
#include <Arduino.h>
#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>
#include <mbedtls/sha256.h>
#include <wake_model_metadata.h>
#include <algorithm>

namespace sense {
inline bool verifyWakeModel() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!part || kWakeModelSize > part->size) {
        Serial.println("ERROR missing/undersized model partition; upload firmware AND model");
        return false;
    }
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
    uint8_t block[1024];
    for (size_t offset = 0; ok && offset < kWakeModelSize; offset += sizeof(block)) {
        const size_t bytes = std::min(sizeof(block), kWakeModelSize - offset);
        ok = esp_partition_read(part, offset, block, bytes) == ESP_OK &&
             mbedtls_sha256_update_ret(&context, block, bytes) == 0;
    }
    uint8_t digest[32] = {};
    ok = ok && mbedtls_sha256_finish_ret(&context, digest) == 0;
    mbedtls_sha256_free(&context);
    if (!ok || memcmp(digest, kWakeModelSha256, sizeof(digest))) {
        Serial.println("ERROR model SHA256 mismatch; upload firmware AND model");
        return false;
    }
    Serial.printf("MODEL_SHA256 PASS bytes=%u\n", static_cast<unsigned>(kWakeModelSize));
    return true;
}

class WakeEngine {
public:
    ~WakeEngine() {
        if (detector_) {
            api_->destroy(detector_);
        }
        if (models_) {
            esp_srmodel_deinit(models_);
        }
    }
    bool begin() {
        if (!verifyWakeModel()) {
            return false;
        }
        models_ = esp_srmodel_init("model");
        if (!models_ || models_->num != 1 || strcmp(models_->model_name[0], kWakeModelName)) {
            Serial.println("ERROR WakeNet model initialization failed");
            return false;
        }
        api_ = esp_wn_handle_from_name(kWakeModelName);
        return reset();
    }
    bool reset() {
        if (!api_) {
            Serial.println("ERROR WakeNet interface unavailable");
            return false;
        }
        // This model's clean() faults in ESP-SR 1.7.1; recreate for discontinuous input.
        if (detector_) {
            api_->destroy(detector_);
        }
        detector_ = api_->create(kWakeModelName, DET_MODE_90);
        if (!detector_ || frameSamples() <= 0 || frameSamples() > 2048 ||
            api_->get_samp_rate(detector_) != 16000 || api_->get_channel_num(detector_) != 1) {
            Serial.println("ERROR WakeNet allocation/audio layout");
            return false;
        }
        return true;
    }
    int frameSamples() const { return api_->get_samp_chunksize(detector_); }
    int detect(int16_t* samples) { return api_->detect(detector_, samples); }
    const char* word(int index = 1) const { return api_->get_word_name(detector_, index); }
    float threshold() const { return api_->get_det_threshold(detector_, 1); }

private:
    const esp_wn_iface_t* api_ = nullptr;
    model_iface_data_t* detector_ = nullptr;
    srmodel_list_t* models_ = nullptr;
};
}  // namespace sense
