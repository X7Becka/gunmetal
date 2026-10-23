#include "audio/music.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace gm::audio {
namespace {

constexpr double kInt16Scale = 1.0 / 32768.0;

bool valid_out_rate(int rate) {
    return rate >= MusicTrack::kMinRate && rate <= MusicTrack::kMaxRate;
}

bool valid_source_rate(int rate) {
    return rate >= MusicTrack::kMinSourceRate && rate <= MusicTrack::kMaxSourceRate;
}

/// Frames in a linear ramp.  The header promise is
/// `N = max(1, round(seconds * rate))`; a non-positive duration is an
/// immediate jump, handled by the caller.
std::int64_t ramp_frames(double seconds, int rate) {
    const double frames = std::round(seconds * static_cast<double>(rate));
    if (!(frames >= 1.0)) {
        return 1;
    }
    if (frames > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2)) {
        return std::numeric_limits<std::int64_t>::max() / 2;
    }
    return static_cast<std::int64_t>(frames);
}

} // namespace

bool MusicTrack::fill_slot(Slot& slot, const PcmClip& clip, int source_rate,
                           std::string& error) const {
    if (clip.samples.empty()) {
        error = "music: clip has no frames";
        return false;
    }
    if (clip.channels != 1 && clip.channels != 2) {
        error = "music: channels must be 1 or 2";
        return false;
    }
    if (!valid_source_rate(source_rate)) {
        error = "music: bad source rate " + std::to_string(source_rate);
        return false;
    }
    if (out_rate_ <= 0 || !valid_out_rate(out_rate_)) {
        error = "music: no valid output rate";
        return false;
    }
    if (clip.samples.size() % static_cast<std::size_t>(clip.channels) != 0) {
        error = "music: payload is not a whole number of frames";
        return false;
    }

    Slot loaded;
    loaded.samples = clip.samples;
    loaded.channels = clip.channels;
    loaded.source_rate = source_rate;
    loaded.step = static_cast<double>(source_rate) / static_cast<double>(out_rate_);
    loaded.pos = 0.0;
    loaded.env = 1.0;
    loaded.active = false;
    slot = std::move(loaded);
    return true;
}

bool MusicTrack::fail(const std::string& message, std::string* error) {
    last_error_ = message;
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool MusicTrack::load(const PcmClip& clip, int sample_rate, std::string* error) {
    return load_at(clip, clip.sample_rate, sample_rate, error);
}

bool MusicTrack::load_at(const PcmClip& clip, int source_rate, int sample_rate,
                         std::string* error) {
    if (!valid_out_rate(sample_rate)) {
        return fail("music: output rate " + std::to_string(sample_rate) +
                        " is outside 1..192000",
                    error);
    }

    const int saved_rate = out_rate_;
    out_rate_ = sample_rate;
    Slot loaded;
    std::string detail;
    if (!fill_slot(loaded, clip, source_rate, detail)) {
        out_rate_ = saved_rate;
        return fail(detail, error);
    }

    current_ = std::move(loaded);
    outgoing_ = Slot{};
    playing_ = false;
    finished_ = false;
    crossfading_ = false;
    pending_fade_in_ = -1.0;
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool MusicTrack::play() {
    if (current_.samples.empty()) {
        return fail("music: play() without a loaded track", nullptr);
    }
    if (out_rate_ <= 0) {
        return fail("music: play() with no output rate", nullptr);
    }
    current_.pos = 0.0;
    current_.env = 1.0;
    current_.ramping = false;
    current_.ramp_done = 0;
    current_.ramp_total = 0;
    current_.active = true;
    outgoing_ = Slot{};
    crossfading_ = false;
    playing_ = true;
    finished_ = false;
    last_error_.clear();
    if (pending_fade_in_ >= 0.0) {
        const double seconds = pending_fade_in_;
        pending_fade_in_ = -1.0;
        fade_in(seconds);
    }
    return true;
}

void MusicTrack::stop() {
    playing_ = false;
    finished_ = false;
    crossfading_ = false;
    current_.active = false;
    current_.ramping = false;
    current_.env = 1.0;
    outgoing_ = Slot{};
    pending_fade_in_ = -1.0;
    last_error_.clear();
}

void MusicTrack::set_gain(double gain) {
    if (!std::isfinite(gain)) {
        fail("music: set_gain(NaN/inf) ignored", nullptr);
        return;
    }
    master_gain_ = std::clamp(gain, 0.0, 1.0);
    last_error_.clear();
}

double MusicTrack::gain() const {
    if (!playing_ || !current_.active) {
        return 0.0;
    }
    return master_gain_ * current_.current_env();
}

void MusicTrack::begin_ramp(Slot& slot, double from, double to, double seconds) {
    slot.env = from;
    slot.env_from = from;
    slot.env_to = to;
    slot.ramp_done = 0;
    slot.ramp_total = 0;
    if (seconds <= 0.0) {
        slot.env = to;
        slot.ramping = false;
        return;
    }
    slot.ramp_total = ramp_frames(seconds, out_rate_);
    slot.ramping = true;
}

void MusicTrack::fade_in(double seconds) {
    if (current_.samples.empty()) {
        fail("music: fade_in() without a loaded track", nullptr);
        return;
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        fail("music: fade_in() wants a finite non-negative duration", nullptr);
        return;
    }
    if (!playing_) {
        // Applied by play(): lets the CLI call `load -> fade_in -> play`.
        pending_fade_in_ = seconds;
        last_error_.clear();
        return;
    }
    begin_ramp(current_, 0.0, 1.0, seconds);
    last_error_.clear();
}

void MusicTrack::fade_out(double seconds) {
    if (current_.samples.empty()) {
        fail("music: fade_out() without a loaded track", nullptr);
        return;
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        fail("music: fade_out() wants a finite non-negative duration", nullptr);
        return;
    }
    if (!playing_) {
        fail("music: fade_out() while playback is not active", nullptr);
        return;
    }
    begin_ramp(current_, current_.current_env(), 0.0, seconds);
    last_error_.clear();
}

void MusicTrack::advance(int frames) {
    if (frames < 0) {
        fail("music: advance() with a negative frame count", nullptr);
        return;
    }
    if (!playing_ || frames == 0) {
        return;
    }
    for (int i = 0; i < frames; ++i) {
        step_one();
        if (!playing_) {
            break;
        }
    }
}

void MusicTrack::step_slot(Slot& slot) {
    if (!slot.active) {
        return;
    }
    if (slot.ramping) {
        ++slot.ramp_done;
        if (slot.ramp_done >= slot.ramp_total) {
            slot.ramp_done = slot.ramp_total;
            slot.env = slot.env_to;
            slot.ramping = false;
            if (slot.env <= 0.0) {
                slot.env = 0.0;
                slot.active = false;
                return;
            }
        }
    }
    slot.pos += slot.step;
    if (slot.pos >= static_cast<double>(slot.frames())) {
        slot.active = false;
    }
}

void MusicTrack::step_one() {
    if (current_.active) {
        step_slot(current_);
    }
    if (outgoing_.active) {
        step_slot(outgoing_);
    }
    if (!outgoing_.active) {
        crossfading_ = false;
    }
    if (!current_.active && !outgoing_.active) {
        playing_ = false;
        finished_ = true;
        crossfading_ = false;
    }
}

float MusicTrack::sample_at(const Slot& slot, std::uint64_t frame, int channel) const {
    const std::uint64_t count = slot.frames();
    if (count == 0) {
        return 0.0f;
    }
    if (frame >= count) {
        frame = count - 1;
    }
    const std::size_t channels = static_cast<std::size_t>(slot.channels);
    const std::size_t base =
        static_cast<std::size_t>(frame) * channels + static_cast<std::size_t>(channel);
    const float first = static_cast<float>(slot.samples[base]) *
                        static_cast<float>(kInt16Scale);
    return first;
}

int MusicTrack::mix_into(std::span<float> out, int channels) {
    if (channels != 1 && channels != 2) {
        last_error_ = "music: mix_into() wants 1 or 2 channels";
        return 0;
    }
    if (!playing_ || out.empty()) {
        if (!playing_) {
            last_error_ = current_.samples.empty()
                              ? "music: mix_into() without a loaded track"
                              : "music: mix_into() while playback is not active";
        }
        return 0;
    }
    const std::size_t frames = out.size() / static_cast<std::size_t>(channels);
    if (frames == 0) {
        return 0;
    }

    for (std::size_t n = 0; n < frames; ++n) {
        double left = 0.0;
        double right = 0.0;
        const auto add = [&](const Slot& slot) {
            if (!slot.active) {
                return;
            }
            const double envelope = master_gain_ * slot.current_env();
            if (envelope == 0.0) {
                return;
            }
            const std::uint64_t index = static_cast<std::uint64_t>(slot.pos);
            const double frac = slot.pos - static_cast<double>(index);
            const auto tap = [&](int channel) {
                const float a = sample_at(slot, index, channel);
                const float b = sample_at(slot, index + 1, channel);
                return static_cast<double>(a) +
                       (static_cast<double>(b) - static_cast<double>(a)) * frac;
            };
            if (slot.channels == 1) {
                const double value = tap(0) * envelope;
                left += value;
                right += value;
            } else {
                left += tap(0) * envelope;
                right += tap(1) * envelope;
            }
        };
        add(current_);
        add(outgoing_);

        if (channels == 2) {
            out[2 * n] += static_cast<float>(left);
            out[2 * n + 1] += static_cast<float>(right);
        } else {
            out[n] += static_cast<float>((left + right) * 0.5);
        }

        step_one();
        if (!playing_) {
            return static_cast<int>(n + 1);
        }
    }
    return static_cast<int>(frames);
}

void MusicTrack::crossfade_to(const PcmClip& next, double seconds) {
    crossfade_to_at(next, next.sample_rate, seconds);
}

bool MusicTrack::crossfade_to_at(const PcmClip& next, int source_rate, double seconds) {
    if (!playing_ || !current_.active) {
        return fail("music: crossfade_to() needs an active track", nullptr);
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return fail("music: crossfade_to() wants a finite non-negative duration", nullptr);
    }

    Slot incoming;
    std::string detail;
    if (!fill_slot(incoming, next, source_rate, detail)) {
        return fail(detail, nullptr);
    }

    // A second crossfade during a fade cuts the previous outgoing track: the
    // audible state is "current", so that is what ramps down now.
    outgoing_ = std::move(current_);
    current_ = std::move(incoming);
    current_.active = true;

    if (seconds <= 0.0) {
        outgoing_.active = false;
        current_.env = 1.0;
        current_.ramping = false;
        crossfading_ = false;
    } else {
        begin_ramp(outgoing_, outgoing_.current_env(), 0.0, seconds);
        begin_ramp(current_, 0.0, 1.0, seconds);
        crossfading_ = true;
    }
    finished_ = false;
    last_error_.clear();
    return true;
}

MusicTrack::CrossfadeInfo MusicTrack::crossfade() const {
    CrossfadeInfo info;
    info.active = crossfading_ && outgoing_.active && current_.active;
    info.progress = current_.active ? current_.current_env() : 0.0;
    info.old_gain = outgoing_.active ? outgoing_.current_env() * master_gain_ : 0.0;
    info.new_gain = current_.active ? current_.current_env() * master_gain_ : 0.0;
    return info;
}

std::uint64_t MusicTrack::frames() const { return current_.frames(); }

double MusicTrack::duration_seconds() const {
    if (current_.source_rate <= 0) {
        return 0.0;
    }
    return static_cast<double>(current_.frames()) /
           static_cast<double>(current_.source_rate);
}

double MusicTrack::position_frames() const { return current_.pos; }

double MusicTrack::position_seconds() const {
    if (current_.source_rate <= 0) {
        return 0.0;
    }
    return current_.pos / static_cast<double>(current_.source_rate);
}

} // namespace gm::audio
