#pragma once
#include <stddef.h>
#include <stdint.h>
#include <local_config.h>
#include <wake_ack_metadata.h>

namespace wake_ack {
constexpr bool sameText(const char* a, const char* b) {
    return *a == *b && (!*a || sameText(a + 1, b + 1));
}
static_assert(sameText(kTTS_SPEAKER, kSpeaker),
              "Wake cue voice mismatch: run python tools\\prepare_wake_audio.py --generate");

struct Clip {
    const uint8_t* audio;
    size_t bytes;
    const char* text;
};
extern const Clip kClips[kCount];
constexpr unsigned next(unsigned index) { return (index + 1) % kCount; }
}  // namespace wake_ack
