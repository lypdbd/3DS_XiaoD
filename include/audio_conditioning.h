#pragma once
#include <algorithm>
#include <stdint.h>

namespace assistant {
class DcBlocker {
public:
    void reset() { initialized_ = false; previousInput_ = previousOutput_ = 0; }
    int16_t process(int16_t sample) {
        if (!initialized_) {
            previousInput_ = sample;
            initialized_ = true;
        }
        // A ~13 Hz high-pass at 16 kHz, initialized without a DC step impulse.
        const float output = sample - previousInput_ + 0.995f * previousOutput_;
        previousInput_ = sample;
        previousOutput_ = output;
        return static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, output)));
    }

private:
    bool initialized_ = false;
    float previousInput_ = 0;
    float previousOutput_ = 0;
};
}  // namespace assistant
