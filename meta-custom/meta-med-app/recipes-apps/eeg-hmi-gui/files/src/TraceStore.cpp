// SPDX-License-Identifier: MIT

#include "TraceStore.h"

#include <algorithm>
#include <cmath>

namespace hmi {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Narrow enough to leave 10 Hz either side of the mains line alone.
constexpr double kNotchQ = 30.0;
}  // namespace

void TraceStore::configure(int channels, double sampleRateHz, double windowSeconds) {
    channels_ = std::max(channels, 0);
    rateHz_ = sampleRateHz > 0.0 ? sampleRateHz : 0.0;
    windowSeconds_ = windowSeconds > 0.0 ? windowSeconds : 0.0;

    const double wanted = std::round(rateHz_ * windowSeconds_);
    // Bounded above as well: 60 s at the fastest rate a converter of this class
    // offers is under a million samples a channel. A header claiming a rate far
    // beyond that is a corrupt header, not a reason to allocate for it.
    capacity_ = static_cast<int>(std::clamp(wanted, 2.0, 2.0e6));
    gap_ = std::max(1, capacity_ / 50);

    total_ = 0;
    data_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity_), 0.0F);
    state_.assign(static_cast<std::size_t>(channels_), FilterState{});
    blockSamples_ = std::max(2, static_cast<int>(std::lround(rateHz_)));
    raw_.assign(static_cast<std::size_t>(channels_), RawBlock{});
    design();
}

void TraceStore::setFilters(double highPassHz, double notchHz) {
    highPassHz_ = highPassHz > 0.0 ? highPassHz : 0.0;
    notchHz_ = notchHz > 0.0 ? notchHz : 0.0;
    design();
    // A filter that keeps the memory of another filter's output rings on the
    // screen for seconds. Start clean.
    std::fill(state_.begin(), state_.end(), FilterState{});
}

void TraceStore::design() {
    highPassOn_ = highPassHz_ > 0.0 && rateHz_ > 0.0 && highPassHz_ < rateHz_ / 2.0;
    if (highPassOn_) {
        const double rc = 1.0 / (2.0 * kPi * highPassHz_);
        const double dt = 1.0 / rateHz_;
        highPassAlpha_ = rc / (rc + dt);
    }

    // A notch at or above half the sample rate does not exist; asking for one
    // is asking for nothing, and the display says "off" rather than aliasing.
    notchOn_ = notchHz_ > 0.0 && rateHz_ > 0.0 && notchHz_ < rateHz_ / 2.0;
    if (notchOn_) {
        const double w0 = 2.0 * kPi * notchHz_ / rateHz_;
        const double alpha = std::sin(w0) / (2.0 * kNotchQ);
        const double a0 = 1.0 + alpha;
        b0_ = 1.0 / a0;
        b1_ = -2.0 * std::cos(w0) / a0;
        b2_ = 1.0 / a0;
        a1_ = -2.0 * std::cos(w0) / a0;
        a2_ = (1.0 - alpha) / a0;
    }
}

void TraceStore::append(const double* interleaved, int channels, int samplesPerChannel) {
    if (interleaved == nullptr || channels != channels_ || capacity_ <= 0 || samplesPerChannel <= 0) {
        return;
    }

    for (int sample = 0; sample < samplesPerChannel; ++sample) {
        const int index = head();
        for (int channel = 0; channel < channels_; ++channel) {
            double value = interleaved[static_cast<std::size_t>(sample) * static_cast<std::size_t>(channels_) +
                                       static_cast<std::size_t>(channel)];
            FilterState& s = state_[static_cast<std::size_t>(channel)];

            // The level and the span of what arrived, before anything below
            // reshapes it for the screen.
            RawBlock& block = raw_[static_cast<std::size_t>(channel)];
            if (block.count == 0) {
                block.lo = value;
                block.hi = value;
                block.sum = 0.0;
            } else {
                block.lo = std::min(block.lo, value);
                block.hi = std::max(block.hi, value);
            }
            block.sum += value;
            if (++block.count >= blockSamples_) {
                block.last.measured = true;
                block.last.mean = block.sum / block.count;
                block.last.span = block.hi - block.lo;
                block.count = 0;
            }

            if (highPassOn_) {
                if (!s.primed) {
                    // Start at the input's own level: the alternative is a
                    // step the size of the electrode offset, which takes
                    // seconds to leave the screen.
                    s.previousIn = value;
                    s.previousOut = 0.0;
                }
                const double out = highPassAlpha_ * (s.previousOut + value - s.previousIn);
                s.previousIn = value;
                s.previousOut = out;
                value = out;
            }
            if (notchOn_) {
                if (!s.primed) {
                    // Steady state for a constant input, for the same reason.
                    const double gain = b0_ + b1_ + b2_;
                    const double denominator = 1.0 + a1_ + a2_;
                    const double settled = denominator != 0.0 ? value * gain / denominator : value;
                    s.z2 = b2_ * value - a2_ * settled;
                    s.z1 = b1_ * value - a1_ * settled + s.z2;
                }
                const double out = b0_ * value + s.z1;
                s.z1 = b1_ * value - a1_ * out + s.z2;
                s.z2 = b2_ * value - a2_ * out;
                value = out;
            }
            s.primed = true;

            data_[static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_) +
                  static_cast<std::size_t>(index)] = static_cast<float>(value);
        }
        ++total_;
    }
}

bool TraceStore::valid(int index) const noexcept {
    const std::uint64_t capacity = static_cast<std::uint64_t>(capacity_);
    if (total_ < capacity) {
        return static_cast<std::uint64_t>(index) < total_;
    }
    // Wrapped: everything holds data, and the run just ahead of the sweep is
    // left blank.
    const int ahead = (index - head() + capacity_) % capacity_;
    return ahead >= gap_;
}

void TraceStore::envelope(int channel, int columns, std::vector<Column>& out) const {
    out.clear();
    if (channel < 0 || channel >= channels_ || capacity_ <= 0 || columns <= 0) {
        return;
    }
    columns = std::min(columns, capacity_);
    out.assign(static_cast<std::size_t>(columns), Column{});

    const float* trace = data_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    for (int column = 0; column < columns; ++column) {
        const int first = static_cast<int>(static_cast<std::int64_t>(column) * capacity_ / columns);
        const int last = static_cast<int>(static_cast<std::int64_t>(column + 1) * capacity_ / columns);
        Column& reduced = out[static_cast<std::size_t>(column)];
        for (int index = first; index < last; ++index) {
            if (!valid(index)) {
                continue;
            }
            const float value = trace[index];
            if (reduced.empty()) {
                reduced.lo = value;
                reduced.hi = value;
            } else {
                reduced.lo = std::min(reduced.lo, value);
                reduced.hi = std::max(reduced.hi, value);
            }
        }
    }
}

double TraceStore::rms(int channel, int samples) const {
    if (channel < 0 || channel >= channels_ || capacity_ <= 0) {
        return 0.0;
    }
    const std::uint64_t held = std::min<std::uint64_t>(total_, static_cast<std::uint64_t>(capacity_));
    const int count = static_cast<int>(std::min<std::uint64_t>(held, static_cast<std::uint64_t>(std::max(samples, 0))));
    if (count < 2) {
        return 0.0;
    }

    const float* trace = data_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    double sum = 0.0;
    double squares = 0.0;
    int index = head();
    for (int taken = 0; taken < count; ++taken) {
        index = (index - 1 + capacity_) % capacity_;
        const double value = static_cast<double>(trace[index]);
        sum += value;
        squares += value * value;
    }
    const double mean = sum / count;
    const double variance = squares / count - mean * mean;
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

double TraceStore::peak(int channel) const {
    if (channel < 0 || channel >= channels_ || capacity_ <= 0) {
        return 0.0;
    }
    const int held = static_cast<int>(std::min<std::uint64_t>(total_, static_cast<std::uint64_t>(capacity_)));
    if (held < 1) {
        return 0.0;
    }
    const float* trace = data_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    double sum = 0.0;
    for (int index = 0; index < held; ++index) {
        sum += static_cast<double>(trace[index]);
    }
    const double mean = sum / held;
    double largest = 0.0;
    for (int index = 0; index < held; ++index) {
        largest = std::max(largest, std::fabs(static_cast<double>(trace[index]) - mean));
    }
    return largest;
}

TraceStore::RawLevel TraceStore::rawLevel(int channel) const {
    if (channel < 0 || channel >= channels_) {
        return RawLevel{};
    }
    return raw_[static_cast<std::size_t>(channel)].last;
}

void RateMeter::add(std::int64_t nowNs, std::uint32_t samplesPerChannel) noexcept {
    lastFrame_ = nowNs;
    if (!started_) {
        // The first frame opens the window and is not counted in it: its
        // samples were converted before the window began.
        started_ = true;
        windowStart_ = nowNs;
        samples_ = 0;
        return;
    }
    samples_ += samplesPerChannel;
    const std::int64_t elapsed = nowNs - windowStart_;
    if (elapsed >= windowNs_) {
        // From one frame's arrival to another's: every frame between them is
        // counted whole, and none is cut by the edge.
        rateHz_ = static_cast<double>(samples_) * 1e9 / static_cast<double>(elapsed);
        measured_ = true;
        windowStart_ = nowNs;
        samples_ = 0;
    }
}

void RateMeter::tick(std::int64_t nowNs) noexcept {
    if (!started_ || nowNs - lastFrame_ < windowNs_) {
        return;
    }
    // Nothing for a whole window: the stream has stalled, and no frame is
    // coming to close this window. Close it here, so the number falls.
    const std::int64_t elapsed = nowNs - windowStart_;
    if (elapsed <= 0) {
        return;
    }
    rateHz_ = static_cast<double>(samples_) * 1e9 / static_cast<double>(elapsed);
    measured_ = true;
    windowStart_ = nowNs;
    samples_ = 0;
}

}  // namespace hmi
