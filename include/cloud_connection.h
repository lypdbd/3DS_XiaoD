#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <local_config.h>
#include <ctime>

namespace cloud {
inline bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    Serial.println("WIFI connecting (20s timeout)");
    WiFi.begin(kWIFI_SSID, kWIFI_PASSWORD);
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(100);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("ERROR Wi-Fi connection timed out");
        return false;
    }
    Serial.printf("WIFI connected RSSI=%d\n", WiFi.RSSI());
    return true;
}

inline bool syncTime() {
    if (time(nullptr) >= 1767225600) {
        return true;
    }
    Serial.println("TIME synchronizing for TLS certificate validation (15s timeout)");
    configTime(0, 0, "ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org");
    const unsigned long start = millis();
    while (time(nullptr) < 1767225600 && millis() - start < 15000) {
        delay(100);
    }
    if (time(nullptr) < 1767225600) {
        Serial.println("ERROR NTP failed; refusing unverified TLS");
        return false;
    }
    Serial.println("TIME synchronized");
    return true;
}
}  // namespace cloud
