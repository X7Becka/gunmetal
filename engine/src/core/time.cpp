#include "core/time.h"

#include <chrono>

namespace gm {

double now_seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

FrameClock::FrameClock() {
    reset();
}

void FrameClock::reset() {
    start_ = now_seconds();
    last_ = start_;
    started_ = false;
}

double FrameClock::tick() {
    const double now = now_seconds();
    if (!started_) {
        started_ = true;
        last_ = now;
        return 0.0;
    }
    const double dt = now - last_;
    last_ = now;
    return dt;
}

double FrameClock::elapsed() const {
    return now_seconds() - start_;
}

} // namespace gm
