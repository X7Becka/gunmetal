#pragma once

namespace gm {

/// Monotonic wall clock in seconds.
double now_seconds();

/// Frame delta with a first-tick guard (the first delta is 0).
class FrameClock {
public:
    FrameClock();

    void reset();
    double tick();              // seconds since the previous tick
    double elapsed() const;     // seconds since reset()

private:
    double start_ = 0.0;
    double last_ = 0.0;
    bool started_ = false;
};

} // namespace gm
