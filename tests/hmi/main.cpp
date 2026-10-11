// SPDX-License-Identifier: MIT
//
// The operator display's arithmetic, without a display.

#include <cmath>
#include <cstdint>
#include <vector>

#include "TraceStore.h"
#include "check.h"

using hmi::Column;
using hmi::RateMeter;
using hmi::TraceStore;

namespace {

constexpr double kPi = 3.14159265358979323846;

// One sample per call across every channel, channel c carrying value + c.
void push(TraceStore& store, double value) {
    std::vector<double> frame(static_cast<std::size_t>(store.channels()));
    for (int c = 0; c < store.channels(); ++c) {
        frame[static_cast<std::size_t>(c)] = value + c;
    }
    store.append(frame.data(), store.channels(), 1);
}

void pushSine(TraceStore& store, double hz, double amplitude, double offset, int samples) {
    const std::uint64_t start = store.total();
    for (int i = 0; i < samples; ++i) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(i)) / store.sampleRateHz();
        const double v = offset + amplitude * std::sin(2.0 * kPi * hz * t);
        std::vector<double> frame(static_cast<std::size_t>(store.channels()), v);
        store.append(frame.data(), store.channels(), 1);
    }
}

}  // namespace

int main() {
    medtest::section("the window is the rate times the seconds, and bounded");
    {
        TraceStore store;
        store.configure(8, 250.0, 10.0);
        CHECK_EQ(store.capacity(), 2500);
        CHECK_EQ(store.channels(), 8);
        store.configure(8, 16000.0, 10.0);
        CHECK_EQ(store.capacity(), 160000);
        // A header claiming an absurd rate must not become an allocation.
        store.configure(8, 1e12, 60.0);
        CHECK_EQ(store.capacity(), 2000000);
        store.configure(8, 0.0, 10.0);
        CHECK_EQ(store.capacity(), 2);
    }

    medtest::section("before the sweep wraps, only what was written is drawn");
    {
        TraceStore store;
        store.configure(2, 100.0, 1.0);  // 100 samples
        for (int i = 0; i < 30; ++i) {
            push(store, static_cast<double>(i));
        }
        std::vector<Column> columns;
        store.envelope(0, 100, columns);
        CHECK_EQ(columns.size(), static_cast<std::size_t>(100));
        CHECK(!columns[0].empty());
        CHECK(!columns[29].empty());
        CHECK(columns[30].empty());
        CHECK(columns[99].empty());
        CHECK_NEAR(columns[7].lo, 7.0, 1e-6);
        // Channels do not bleed into each other.
        store.envelope(1, 100, columns);
        CHECK_NEAR(columns[7].lo, 8.0, 1e-6);
    }

    medtest::section("after it wraps, new samples replace old ones in place and a gap leads the sweep");
    {
        TraceStore store;
        store.configure(1, 100.0, 1.0);
        for (int i = 0; i < 130; ++i) {
            push(store, static_cast<double>(i));
        }
        CHECK_EQ(store.head(), 30);
        CHECK(store.gap() >= 1);
        std::vector<Column> columns;
        store.envelope(0, 100, columns);
        CHECK_NEAR(columns[29].lo, 129.0, 1e-6);  // the newest sample
        CHECK_NEAR(columns[5].lo, 105.0, 1e-6);   // this lap
        CHECK(columns[30].empty());               // the gap
        CHECK(columns[static_cast<std::size_t>(30 + store.gap() - 1)].empty());
        const std::size_t firstOld = static_cast<std::size_t>(30 + store.gap());
        CHECK(!columns[firstOld].empty());        // the previous lap, still shown
        CHECK_NEAR(columns[firstOld].lo, static_cast<double>(firstOld), 1e-6);
    }

    medtest::section("a one-sample spike survives any reduction");
    {
        TraceStore store;
        store.configure(1, 1000.0, 10.0);  // 10000 samples
        for (int i = 0; i < 9000; ++i) {
            push(store, i == 4321 ? 500.0 : 0.0);
        }
        std::vector<Column> columns;
        store.envelope(0, 100, columns);  // 100 samples per column
        CHECK_NEAR(columns[43].hi, 500.0, 1e-6);
        CHECK_NEAR(columns[43].lo, 0.0, 1e-6);
        CHECK_NEAR(columns[42].hi, 0.0, 1e-6);
        // More columns than samples is clamped, never read past the buffer.
        store.envelope(0, 50000, columns);
        CHECK_EQ(columns.size(), static_cast<std::size_t>(10000));
        // Out-of-range requests draw nothing.
        store.envelope(3, 100, columns);
        CHECK(columns.empty());
        store.envelope(-1, 100, columns);
        CHECK(columns.empty());
    }

    medtest::section("a frame of another shape is ignored, not guessed at");
    {
        TraceStore store;
        store.configure(8, 250.0, 1.0);
        std::vector<double> four(4, 1.0);
        store.append(four.data(), 4, 1);
        CHECK_EQ(store.total(), static_cast<std::uint64_t>(0));
        store.append(nullptr, 8, 1);
        CHECK_EQ(store.total(), static_cast<std::uint64_t>(0));
    }

    medtest::section("rms is about the mean, over the newest samples");
    {
        TraceStore store;
        store.configure(1, 1000.0, 4.0);
        pushSine(store, 10.0, 100.0, 5000.0, 3000);
        // A sine's rms is amplitude / sqrt(2), whatever it rides on.
        CHECK_NEAR(store.rms(0, 1000), 100.0 / std::sqrt(2.0), 0.5);
        CHECK_NEAR(store.peak(0), 100.0, 1.0);
        CHECK_NEAR(store.rms(0, 1), 0.0, 1e-12);
        CHECK_NEAR(store.rms(5, 1000), 0.0, 1e-12);
    }

    medtest::section("the high-pass removes an offset and starts without a step");
    {
        TraceStore store;
        store.configure(1, 250.0, 20.0);
        store.setFilters(0.5, 0.0);
        pushSine(store, 10.0, 50.0, 30000.0, 2500);  // 30 mV of electrode offset
        std::vector<Column> columns;
        store.envelope(0, store.capacity(), columns);
        // The very first samples already sit near zero, not near 30 mV.
        CHECK(std::fabs(columns[0].lo) < 60.0);
        // And the signal passes: 10 Hz is far above the corner.
        CHECK_NEAR(store.rms(0, 500), 50.0 / std::sqrt(2.0), 1.0);
        CHECK(store.peak(0) < 60.0);
    }

    medtest::section("the notch removes the mains line and leaves the band beside it");
    {
        TraceStore store;
        store.configure(1, 250.0, 20.0);
        store.setFilters(0.0, 60.0);
        pushSine(store, 60.0, 1000.0, 0.0, 5000);
        CHECK(store.rms(0, 500) < 10.0);  // more than 40 dB down once settled

        store.configure(1, 250.0, 20.0);
        store.setFilters(0.0, 60.0);
        pushSine(store, 10.0, 100.0, 0.0, 5000);
        CHECK_NEAR(store.rms(0, 500), 100.0 / std::sqrt(2.0), 1.0);
    }

    medtest::section("a filter the sample rate cannot carry is off, not aliased");
    {
        TraceStore store;
        store.configure(1, 100.0, 10.0);
        store.setFilters(0.0, 60.0);  // above Nyquist at 100 Hz
        pushSine(store, 10.0, 100.0, 0.0, 800);
        CHECK_NEAR(store.rms(0, 500), 100.0 / std::sqrt(2.0), 0.5);
    }

    medtest::section("filters off, the store holds what it was given");
    {
        TraceStore store;
        store.configure(1, 250.0, 1.0);
        for (int i = 0; i < 100; ++i) {
            push(store, 187500.0 - i);
        }
        std::vector<Column> columns;
        store.envelope(0, store.capacity(), columns);
        CHECK_NEAR(columns[0].lo, 187500.0, 0.02);
        CHECK_NEAR(columns[99].lo, 187401.0, 0.02);
    }

    medtest::section("an input that does not vary is said to be so, whatever the filters draw");
    {
        // The board, 2026-10-11: open inputs at the converter's rail. With the
        // high-pass on, the trace is a calm line at zero and its rms is zero -
        // which is also what a perfect electrode would look like.
        TraceStore store;
        store.configure(2, 250.0, 10.0);
        store.setFilters(0.5, 0.0);
        CHECK(!store.rawLevel(0).measured);
        for (int i = 0; i < 600; ++i) {
            const double frame[2] = {187500.0, 20.0 * std::sin(2.0 * kPi * 10.0 * i / 250.0) - 3000.0};
            store.append(frame, 2, 1);
        }
        CHECK_NEAR(store.rms(0, 250), 0.0, 1e-6);  // the lie the screen would tell
        CHECK(store.rawLevel(0).measured);
        CHECK_NEAR(store.rawLevel(0).mean, 187500.0, 1e-6);
        CHECK_NEAR(store.rawLevel(0).span, 0.0, 0.0);
        // A live input beside it: a level, and a span that is not zero.
        CHECK_NEAR(store.rawLevel(1).mean, -3000.0, 1.0);
        CHECK_NEAR(store.rawLevel(1).span, 40.0, 1.0);
        CHECK(!store.rawLevel(7).measured);
        // Reconfiguring forgets it: a new window has measured nothing yet.
        store.configure(2, 250.0, 5.0);
        CHECK(!store.rawLevel(0).measured);
    }

    medtest::section("the delivered rate is counted, not copied from the header");
    {
        const std::int64_t frameNs = 56000000;  // 14 samples at 250 Hz
        RateMeter meter(2.0);
        CHECK(!meter.measured());
        std::int64_t now = 0;
        for (int i = 0; i < 80; ++i) {
            meter.add(now, 14);
            now += frameNs;
        }
        CHECK(meter.measured());
        CHECK_NEAR(meter.rateHz(), 250.0, 0.5);

        // A clock that is not the stream's must not close the window. The
        // display polls four times a second; closed by that poll, a window
        // catches a frame more or fewer at each end and the rate wanders by
        // 7 Hz over a stream that loses nothing (the board, 2026-10-11:
        // 242 to 252). Polled between every pair of frames here, at an
        // offset that drifts across the frame period.
        meter.reset();
        now = 0;
        double lowest = 1e9;
        double highest = 0.0;
        for (int i = 0; i < 400; ++i) {
            meter.add(now, 14);
            meter.tick(now + 1000000 + (i * 7919000LL) % 50000000);
            if (meter.measured()) {
                lowest = meter.rateHz() < lowest ? meter.rateHz() : lowest;
                highest = meter.rateHz() > highest ? meter.rateHz() : highest;
            }
            now += frameNs;
        }
        CHECK(meter.measured());
        CHECK_NEAR(lowest, 250.0, 0.01);
        CHECK_NEAR(highest, 250.0, 0.01);

        // The defect this exists for: the stream declares 250 and half of it
        // arrives. Every second frame is lost; the meter must say ~125.
        meter.reset();
        CHECK(!meter.measured());
        now = 0;
        for (int i = 0; i < 80; ++i) {
            if (i % 2 == 0) {
                meter.add(now, 14);
            }
            now += frameNs;
        }
        CHECK(meter.measured());
        CHECK_NEAR(meter.rateHz(), 125.0, 2.0);

        // And a stream that stops reads zero instead of its last good number.
        // The window open when it stopped still holds the tail of the stream,
        // so the first close is low and the one after it is zero.
        meter.tick(now + 3000000000LL);
        CHECK(meter.rateHz() < 30.0);
        meter.tick(now + 6000000000LL);
        CHECK_NEAR(meter.rateHz(), 0.0, 1e-9);
    }

    return medtest::report();
}
