#pragma once
#include <3ds.h>

struct TouchState {
    bool is_touching  = false;  // Currently touching
    bool is_dragging  = false;  // In drag mode
    int  start_x      = 0;     // Touch start X
    int  start_y      = 0;     // Touch start Y
    int  prev_y       = 0;     // Previous frame Y
    int  current_x    = 0;     // Current frame X
    int  current_y    = 0;     // Current frame Y

    static constexpr int DRAG_THRESHOLD = 4; // px

    // Call on touch start
    void begin(int x, int y) {
        is_touching = true;
        is_dragging = false;
        start_x = x;
        start_y = y;
        prev_y  = y;
        current_x = x;
        current_y = y;
    }

    // Call each frame while touching. Returns Y delta during drag
    int update(int x, int y) {
        current_x = x;
        current_y = y;
        int delta_y = 0;

        if (!is_dragging) {
            int dy = current_y - start_y;
            if (dy > DRAG_THRESHOLD || dy < -DRAG_THRESHOLD) {
                is_dragging = true;
            }
        }

        if (is_dragging) {
            delta_y = prev_y - current_y; // Swipe up = positive = scroll list down
        }

        prev_y = current_y;
        return delta_y;
    }

    // Call on touch end. Returns true if it was a tap
    bool end() {
        bool was_tap = is_touching && !is_dragging;
        is_touching = false;
        is_dragging = false;
        return was_tap;
    }

    void reset() {
        is_touching = false;
        is_dragging = false;
    }
};
