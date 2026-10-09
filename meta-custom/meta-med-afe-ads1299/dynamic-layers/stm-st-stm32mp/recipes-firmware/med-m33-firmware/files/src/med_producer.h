/* SPDX-License-Identifier: MIT */
/*
 * The producer side of the AMP link, with no hardware in it.
 *
 * Everything the Cortex-M33 firmware decides - whether a prescription is
 * accepted, what the answer says, which samples a frame carries, what its
 * header and CRC are - lives here, in plain C against med_amp_abi.h and
 * nothing else. main.c owns the clock, the interrupts and the rpmsg endpoint
 * and calls in. The split exists so that tests/m33-producer can compile this
 * file for the host and check it before a board is involved, which is the only
 * kind of evidence firmware logic can have until it runs.
 *
 * Three sources, chosen once at start-up by main.c and never switched at run
 * time:
 *
 *   CONVERTER  - the samples are the converter's, handed in by the DRDY path
 *   ABSENT     - a converter was expected and not found: every prescription is
 *                refused with the reason, and nothing streams
 *   SYNTHETIC  - a generator, for host tests and for a build that asks for it
 *                explicitly; it says so in every answer
 *
 * There is no fallback from CONVERTER to SYNTHETIC. A device that expected a
 * converter and streamed a formula instead would produce a plausible record
 * of a recording that never happened.
 */

#ifndef MED_PRODUCER_H
#define MED_PRODUCER_H

#include <stddef.h>
#include <stdint.h>

#include "med_amp_abi.h"

/*
 * The geometry of the stream. Facts of this producer, not parameters: the
 * Linux side reads them from every frame header and does not configure them.
 * The 40-byte header, 8 x 14 x 4 bytes of samples and the 8-byte lead-off
 * block are 496 bytes: all of a stock 512-byte rpmsg buffer's payload
 * (med_amp_abi.h says where that number comes from).
 */
#define MED_PRODUCER_CHANNELS          8u
#define MED_PRODUCER_SAMPLES_PER_FRAME 14u
#define MED_PRODUCER_RATE_HZ           250u
#define MED_PRODUCER_FRAME_BYTES \
    (sizeof(med_amp_frame_header) + \
     MED_PRODUCER_CHANNELS * MED_PRODUCER_SAMPLES_PER_FRAME * sizeof(int32_t) + \
     sizeof(med_amp_lead_off))

enum med_test_signal { MED_TEST_SIGNAL_OFF = 0, MED_TEST_SIGNAL_INTERNAL = 1 };

enum med_source { MED_SOURCE_SYNTHETIC = 0, MED_SOURCE_CONVERTER = 1, MED_SOURCE_ABSENT = 2 };

struct med_producer {
    enum med_source source;
    const char *absent_reason; /* MED_SOURCE_ABSENT: why, for every refusal */

    /* The prescription last accepted. */
    double gain;
    double reference_uv;
    enum med_test_signal test_signal;
    int bias_drive; /* converter only: derived bias amplifier on */
    int lead_off;   /* converter only: every channel's inputs monitored */

    /* 1 between an accepted prescription and the next control message. */
    int streaming;

    /* The session: restarted by every accepted prescription. */
    uint64_t sequence;     /* of the frame being filled */
    uint32_t filled;       /* samples per channel already in it */
    uint64_t first_us;     /* timestamp of its first sample */
    int32_t samples[MED_PRODUCER_SAMPLES_PER_FRAME * MED_PRODUCER_CHANNELS];
    uint16_t off_positive; /* lead-off, OR of every conversion in the frame */
    uint16_t off_negative;

    /* Counters for the trace log, never reset. */
    uint32_t controls_accepted;
    uint32_t controls_rejected;
    uint32_t controls_malformed;
};

/* Starts SYNTHETIC; main.c sets the real source before the endpoint exists. */
void med_producer_init(struct med_producer *p);
void med_producer_set_source(struct med_producer *p, enum med_source source,
                             const char *absent_reason);

/*
 * A message arrived from Linux. Returns 1 when an answer must be sent, with
 * the answer in *ack; 0 when the message is to be ignored (it was not a
 * control message, or it was malformed - the trace log says which).
 *
 * Silence on a malformed message is deliberate: the sender times out and
 * refuses to acquire, which is the safe outcome, and an answer to a message
 * that may not be what it claims would be a guess about which option it meant.
 *
 * An accepted prescription restarts the session - sequence 0, sample index 0
 * - and starts streaming. A rejected one stops streaming: a front-end whose
 * prescription was refused must not keep producing samples under the old one.
 */
int med_producer_on_control(struct med_producer *p, const void *data, size_t length,
                            med_amp_control_ack *ack);

/*
 * The prescription was accepted here and then not taken by the hardware (a
 * register that did not read back). Turns *ack into a refusal and stops the
 * session. The refusal names no option, because none was wrong: rejectedIndex
 * is 1 and the detail carries the reason.
 */
void med_producer_refuse(struct med_producer *p, med_amp_control_ack *ack, const char *why);

/*
 * One sample instant, at time now_us on the producer's clock. Returns 1 when
 * this sample completed a frame, which has then been written into frame
 * (MED_PRODUCER_FRAME_BYTES long) and must be sent; 0 otherwise. Does nothing
 * while not streaming.
 *
 * The frame's sequence advances whether or not the caller manages to send it,
 * so a frame the link could not carry is a gap the receiver sees and counts,
 * never a silent splice.
 */
int med_producer_on_sample(struct med_producer *p, uint64_t now_us, uint8_t *frame);

/* The same, with the sample's values supplied - one per channel, nanovolts -
 * and the lead-off bits of that conversion, one per channel input. The
 * CONVERTER source's path. The bits are reported only while the accepted
 * prescription asked for lead-off detection; otherwise the frame says
 * "nothing monitored", whatever is passed. */
int med_producer_on_values(struct med_producer *p, uint64_t now_us, const int32_t *nv,
                           uint16_t off_positive, uint16_t off_negative, uint8_t *frame);

/*
 * The synthetic signal, in nanovolts, as a pure function of the global sample
 * index n (sequence * samples-per-frame + position) and the channel. Exposed
 * so that a receiver - the host tests, the bench analyser - can recompute every
 * sample and compare, instead of judging a waveform by eye.
 */
int32_t med_producer_sample_nv(const struct med_producer *p, uint64_t n, uint32_t channel);

#endif /* MED_PRODUCER_H */
