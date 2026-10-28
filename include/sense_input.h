#pragma once

#include <Arduino.h>
#include <esp_camera.h>

namespace sense {
inline bool check(const char* operation, esp_err_t result) {
    if (result == ESP_OK) {
        return true;
    }
    Serial.printf("ERROR %s: %s\n", operation, esp_err_to_name(result));
    return false;
}

template <typename Consumer>
bool captureJpeg(Consumer consume) {
    if (!psramFound()) {
        Serial.println("ERROR camera requires PSRAM");
        return false;
    }
    camera_config_t config = {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = 10;
    config.pin_sccb_sda = 40;
    config.pin_sccb_scl = 39;
    config.pin_d0 = 15;
    config.pin_d1 = 17;
    config.pin_d2 = 18;
    config.pin_d3 = 16;
    config.pin_d4 = 14;
    config.pin_d5 = 12;
    config.pin_d6 = 11;
    config.pin_d7 = 48;
    config.pin_vsync = 38;
    config.pin_href = 47;
    config.pin_pclk = 13;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    if (!check("camera init", esp_camera_init(&config))) {
        return false;
    }
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
        Serial.printf("CAMERA PID=0x%x\n", sensor->id.PID);
    }
    delay(500);
    bool ok = true;
    for (unsigned i = 0; i < 5; ++i) {
        camera_fb_t* frame = esp_camera_fb_get();
        if (!frame) {
            Serial.println("ERROR camera returned no frame");
            ok = false;
            break;
        }
        if (i == 4) {
            if (frame->format != PIXFORMAT_JPEG || frame->len < 4 ||
                frame->buf[0] != 0xff || frame->buf[1] != 0xd8 ||
                frame->buf[frame->len - 2] != 0xff || frame->buf[frame->len - 1] != 0xd9) {
                Serial.println("ERROR invalid camera JPEG");
                ok = false;
            } else {
                Serial.printf("PHOTO %ux%u JPEG bytes=%u\n",
                              static_cast<unsigned>(frame->width),
                              static_cast<unsigned>(frame->height),
                              static_cast<unsigned>(frame->len));
                ok = consume(frame->buf, frame->len);
            }
        }
        esp_camera_fb_return(frame);
        delay(100);
    }
    const bool stopped = check("camera deinit", esp_camera_deinit());
    return ok && stopped;
}
}  // namespace sense
