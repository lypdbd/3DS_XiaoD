#pragma once
#include <stddef.h>
#include <stdint.h>

namespace speaker {
bool playPcm(const uint8_t* audio, size_t bytes, uint32_t sampleRate = 24000);

class PcmSource {
public:
    virtual ~PcmSource() = default;
    virtual const uint8_t* audio() const = 0;
    virtual size_t publishedBytes() const = 0;
    virtual bool complete() const = 0;
    virtual bool failed() const = 0;
};

struct StreamStats {
    uint32_t firstWriteMs = 0;
    size_t playedBytes = 0;
    unsigned underruns = 0;
};
bool playStream(const PcmSource& source, StreamStats& stats);
}
