// SPDX-License-Identifier: MIT
//
// What the operator display remembers of the stream, and how it reduces it to
// the pixels it has.
//
// No Qt in this file, on purpose: it is arithmetic on samples, and it is
// compiled as it ships into a host test (tests/hmi) that needs no display and
// no toolkit. What draws it is WaveformItem; what feeds it is EegClient.
//
// Everything here is DISPLAY. The filters below shape what is drawn and
// nothing else: the record is written by the acquisition service from the
// frames it produced, before this process ever sees them, and this process can
// open neither the record volume nor the front-end. A filter switched on here
// cannot reach a clinical file, and that is not a promise - it is the sandbox.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hmi {

/// Min and max of one channel across one screen column. An empty column - no
/// data there yet, or the gap the sweep leaves ahead of itself - has lo > hi.
struct Column {
    float lo = 1.0F;
    float hi = -1.0F;
    bool empty() const noexcept { return lo > hi; }
};

/// A fixed window of the most recent samples per channel, written as a sweep:
/// sample n lands at position n mod capacity, so a new sample replaces the one
/// that was a whole window older and nothing ever scrolls. That is how a
/// bedside monitor draws, and it means appending costs the same at 250 samples
/// a second and at 16000.
///
/// Bounded by construction. An HMI whose buffer grows with the session takes
/// the device down eventually; this one allocates once per configuration.
class TraceStore {
public:
    /// Drops everything held and sizes the window. Rates and windows that would
    /// hold fewer than two samples are raised to two.
    void configure(int channels, double sampleRateHz, double windowSeconds);

    /// First order high-pass at @p highPassHz and a notch at @p notchHz, either
    /// one off when zero. Applies from the next sample on - what is already on
    /// screen stays as it was drawn, and the sweep overwrites it.
    void setFilters(double highPassHz, double notchHz);

    /// @p interleaved is sample-major: interleaved[i * channels + c], the
    /// layout of a decoded frame. A channel count other than the configured
    /// one is ignored rather than guessed at.
    void append(const double* interleaved, int channels, int samplesPerChannel);

    int channels() const noexcept { return channels_; }
    int capacity() const noexcept { return capacity_; }
    double sampleRateHz() const noexcept { return rateHz_; }
    double windowSeconds() const noexcept { return windowSeconds_; }
    /// Samples per channel appended since configure().
    std::uint64_t total() const noexcept { return total_; }
    /// Where the next sample will be written, 0..capacity-1.
    int head() const noexcept {
        return capacity_ > 0 ? static_cast<int>(total_ % static_cast<std::uint64_t>(capacity_)) : 0;
    }
    /// Samples left blank ahead of the sweep so the eye can find it.
    int gap() const noexcept { return gap_; }

    /// Reduces one channel to @p columns columns (at most capacity()). Every
    /// sample that falls in a column contributes to its min and max, so a spike
    /// one sample wide is still one column tall however many samples share it.
    void envelope(int channel, int columns, std::vector<Column>& out) const;

    /// Root mean square about the mean, over the most recent @p samples that
    /// exist. Zero with fewer than two.
    double rms(int channel, int samples) const;

    /// Largest excursion from the mean over the whole window held.
    double peak(int channel) const;

    /// What the channel carried BEFORE any display filter, over the last whole
    /// second: its mean level and its span, maximum minus minimum.
    ///
    /// These exist because a high-pass makes a lie easy. A channel sitting at
    /// the converter's rail does not vary at all, the filter removes the level,
    /// and what is left on screen is a calm line at zero with an rms of nothing
    /// - the picture of a perfect electrode. Measured on the board 2026-10-11:
    /// eight open inputs, every one at +187.5 mV with a span of exactly zero,
    /// drawn as eight quiet traces. A real input always moves by at least the
    /// converter's own noise, so a span of zero is never a quiet signal.
    struct RawLevel {
        bool measured = false;
        double mean = 0.0;
        double span = 0.0;
    };
    RawLevel rawLevel(int channel) const;

private:
    struct FilterState {
        bool primed = false;
        // high-pass
        double previousIn = 0.0;
        double previousOut = 0.0;
        // notch, direct form II transposed
        double z1 = 0.0;
        double z2 = 0.0;
    };

    void design();
    bool valid(int index) const noexcept;

    int channels_ = 0;
    int capacity_ = 0;
    int gap_ = 0;
    double rateHz_ = 0.0;
    double windowSeconds_ = 0.0;
    std::uint64_t total_ = 0;
    /// Channel-major: data_[c * capacity_ + i]. One channel is one contiguous
    /// run, which is the order envelope() reads in.
    std::vector<float> data_;

    double highPassHz_ = 0.0;
    double notchHz_ = 0.0;
    bool highPassOn_ = false;
    bool notchOn_ = false;
    double highPassAlpha_ = 0.0;
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    std::vector<FilterState> state_;

    struct RawBlock {
        int count = 0;
        double sum = 0.0;
        double lo = 0.0;
        double hi = 0.0;
        RawLevel last;
    };
    int blockSamples_ = 0;
    std::vector<RawBlock> raw_;
};

/// The rate the stream actually arrives at, as opposed to the one its header
/// declares. The acquisition service once delivered 94 of a declared 250
/// samples a second while perfectly active; a display that prints the declared
/// number repeats that mistake on the operator's screen.
///
/// Time is an argument and not read here, so the test drives it.
class RateMeter {
public:
    explicit RateMeter(double windowSeconds = 2.0) : windowNs_(static_cast<std::int64_t>(windowSeconds * 1e9)) {}

    void reset() noexcept {
        started_ = false;
        samples_ = 0;
        rateHz_ = 0.0;
        measured_ = false;
    }

    /// @p nowNs is any monotonic clock.
    void add(std::int64_t nowNs, std::uint32_t samplesPerChannel) noexcept;

    /// Also to be called when nothing arrives: a stream that stops must read
    /// zero, and add() alone would leave the last good number on screen.
    void tick(std::int64_t nowNs) noexcept;

    /// False until one whole window has been observed.
    bool measured() const noexcept { return measured_; }
    double rateHz() const noexcept { return rateHz_; }

private:
    std::int64_t windowNs_;
    std::int64_t windowStart_ = 0;
    std::uint64_t samples_ = 0;
    double rateHz_ = 0.0;
    bool started_ = false;
    bool measured_ = false;
};

}  // namespace hmi
