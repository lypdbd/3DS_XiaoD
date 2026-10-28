#pragma once
#include <stddef.h>
#include <stdint.h>

namespace speaker {
bool playPcm(const uint8_t* audio, size_t bytes, uint32_t sampleRate = 24000);
}
