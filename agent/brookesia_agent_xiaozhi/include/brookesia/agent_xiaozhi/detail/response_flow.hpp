#pragma once
#include <cstdint>

namespace esp_brookesia::agent::acos {

// Access under receive_mutex_. Binary ACOS messages have no response ID, so
// assign a local turn at the wire boundary, before buffering for playback.
struct ResponseFlow {
    struct Cancellation {
        uint32_t through;
        bool send;
    };
    uint32_t current = 0;
    uint32_t discarded = 0;
    uint32_t cancel_requested = 0;
    bool open = false;

    constexpr uint32_t audio()
    {
        if (!open) {
            ++current;
            open = true;
        }
        return current;
    }
    constexpr uint32_t done()
    {
        open = false;
        return current;
    }
    constexpr Cancellation interrupt()
    {
        discarded = current;
        const bool send = open && cancel_requested != current;
        if (send) cancel_requested = current;
        return {current, send};
    }
    constexpr bool accepts(uint32_t turn) const
    {
        return turn != 0 && turn > discarded;
    }
};

} // namespace esp_brookesia::agent::acos
