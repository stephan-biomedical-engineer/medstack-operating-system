/* SPDX-License-Identifier: MIT */
/*
 * MedFramework - wire format of the AMP link, the one source of truth.
 *
 * Everything that crosses the boundary between the Linux side and the firmware
 * on the real-time core is declared here and nowhere else: the sample frame
 * header, the control message that carries a front-end prescription, and the
 * producer's answer to it. MedicalDevice.h includes this file and names the
 * same types in med::amp; the firmware includes it directly. One source, two
 * compilers - for two architectures, and in two languages.
 *
 * Why a C header and not the C++ one: a bare-metal Cortex-M build has no
 * libstdc++, and MedicalDevice.h pulls in <map>, <string> and <memory>. The
 * alternative - the firmware keeping its own copy of these structs - is two
 * definitions of one format, with the guarantee that they diverge one day
 * without anything failing: the CRC would still close, over the wrong fields.
 *
 * The rules this file keeps, and that a change to it must keep:
 *
 *   - C99 and freestanding. The only inclusions are <stdint.h> and <stddef.h>,
 *     which every hosted and freestanding compiler provides without a C
 *     library. tests/framework compiles it with -nostdinc against the
 *     compiler's own headers to hold that.
 *   - Every structure is packed and its size AND the offset of every field is
 *     asserted at compile time, below. Equal sizes are not equal layouts: two
 *     fields of the same width swapped keep the size and change the format.
 *   - Multi-byte fields are little-endian, which is what both cores are. A
 *     big-endian producer would have to convert; none exists.
 *
 * This header is part of an ABI shared with firmware built from another
 * recipe. Changing a field is a protocol change: bump the version and teach
 * both sides, never edit in place.
 */

#ifndef MED_AMP_ABI_H
#define MED_AMP_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ sample frames */

/*
 * "MEEG" is a historical spelling and stays: the magic is ABI shared with
 * firmware, and a frame is a frame whatever the device measures.
 */
#define MED_AMP_FRAME_MAGIC   0x4D454547u /* "MEEG" */
/*
 * Version 2 (2026-10-08) appends med_amp_lead_off after the samples and
 * extends the CRC over it. Version 1 had no way to say whether an electrode
 * was attached, so a producer asked to detect lead-off could either refuse or
 * detect into the void - and detecting into the void is accepting a safety
 * setting without honouring it. No v1 producer or receiver remains: the
 * firmware and the framework ship in the same image.
 */
#define MED_AMP_FRAME_VERSION 2u

#pragma pack(push, 1)

/*
 * Followed on the wire by channelCount * samplesPerChannel little-endian int32
 * samples, channel-interleaved: sample i of channel c is at i * channelCount + c,
 * and then by one med_amp_lead_off. The CRC covers both.
 *
 * Size, against the transport: 8 channels x 14 samples is 40 + 448 + 8 = 496
 * bytes, which is the whole payload of a stock 512-byte rpmsg buffer (512 less
 * its 16-byte header: MAX_RPMSG_BUF_SIZE and struct rpmsg_hdr in Linux's
 * virtio_rpmsg_bus.c; OpenAMP's side was not read, and a frame it could not
 * carry would show up as the firmware's "dropped" count). No margin is left. A ninth channel or a fifteenth
 * sample per frame does not fit, and must change the geometry, not the buffer.
 */
typedef struct med_amp_frame_header {
    uint32_t magic;
    uint16_t version;
    uint16_t channelCount;
    uint32_t samplesPerChannel;
    uint32_t sampleRateMilliHz;
    /* Monotonic; a gap is reported by the receiver as lost frames. */
    uint64_t sequence;
    /*
     * Taken on the producer's core, at the conversion - in the data-ready ISR
     * when there is a converter, at the timer when the signal is synthetic.
     * A stamp taken on the Linux side would erase the one property that
     * distinguishes this link from a USB one, and break nothing visible.
     */
    uint64_t timestampMicros;
    /*
     * Nanovolts per LSB of the int32 samples that follow.
     *
     * Producers are expected to deliver NANOVOLTS and set this to 1, not to
     * send raw converter counts with a rounded step. The reason is arithmetic:
     * a high-resolution converter's step is rarely a whole number of
     * nanovolts - 22.35 nV, say - and an integer field can only say 22, a 1.6%
     * gain error on every sample of every trace, invisible because the
     * waveform still looks plausible. One multiplication at the producer
     * removes it and costs no ABI change: an int32 of nanovolts spans +-2.1 V.
     *
     * The field stays, and stays honest: a producer that genuinely has an
     * integer step may still declare it.
     */
    int32_t scaleNanoUnitsPerLsb;
    /* CRC-32 (see med_amp_crc32) over everything after the header: the
     * samples and the lead-off block. Not over the header itself. */
    uint32_t crc32;
} med_amp_frame_header;

/*
 * Electrode contact over the frame, one bit per channel input: bit c is
 * channel c, for c < 16. A channel beyond the sixteenth cannot be monitored in
 * this version - a producer with more channels leaves them out of "monitored",
 * and the receiver must not read that as "attached".
 *
 * "monitored" is the half that makes the other meaningful. A clear "off" bit
 * means "attached" only where the matching "monitored" bit is set; everywhere
 * else it means nothing was checked. That distinction is the reason this block
 * has four fields and not two: a converter whose detection is switched off
 * reports every electrode attached, and a status with no "monitored" beside it
 * would forward that as a fact.
 *
 * "off" is the union over every conversion in the frame: an electrode that lost
 * contact for one conversion out of fourteen is reported off for the frame.
 * Coarser than the converter, and on the side that raises an alarm rather than
 * the one that misses it.
 */
typedef struct med_amp_lead_off {
    uint16_t monitoredPositive;
    uint16_t monitoredNegative;
    uint16_t offPositive;
    uint16_t offNegative;
} med_amp_lead_off;

#pragma pack(pop)

/* ---------------------------------------------------------- control channel */

/*
 * The settings the application prescribes and the front-end has to program
 * into a converter's registers. Deliberately a list of opaque key/value
 * strings and not a struct of named settings: the framework must not acquire
 * an opinion about what a front-end has, and a struct would need a new field,
 * and therefore a new ABI, for every converter.
 */
#define MED_AMP_CONTROL_MAGIC     0x4D435452u /* "MCTR" */
#define MED_AMP_CONTROL_ACK_MAGIC 0x4D435441u /* "MCTA" */
#define MED_AMP_CONTROL_VERSION   1u

/*
 * Sized against the transport, not against taste: a stock rpmsg buffer is 512
 * bytes with about 496 usable, and 8 options of 56 bytes plus a 16 byte header
 * is 464. Needing a ninth option is a protocol change - a producer that reads
 * a truncated option list would program a converter from half a prescription,
 * so the sender refuses rather than trimming.
 */
#define MED_AMP_CONTROL_MAX_OPTIONS 8u
#define MED_AMP_CONTROL_KEY_SIZE    32u
#define MED_AMP_CONTROL_VALUE_SIZE  24u
#define MED_AMP_CONTROL_DETAIL_SIZE 64u

#pragma pack(push, 1)

typedef struct med_amp_control_option {
    char key[MED_AMP_CONTROL_KEY_SIZE];     /* NUL terminated */
    char value[MED_AMP_CONTROL_VALUE_SIZE]; /* NUL terminated */
} med_amp_control_option;

typedef struct med_amp_control_message {
    uint32_t magic;
    uint16_t version;
    uint16_t optionCount;
    uint32_t crc32; /* over the first optionCount options only */
    uint32_t reserved;
    med_amp_control_option options[MED_AMP_CONTROL_MAX_OPTIONS];
} med_amp_control_message;

/*
 * The producer's answer. Not optional: an option the front-end did not
 * understand must come back as a refusal, because the alternative is a device
 * acquiring at a gain nobody prescribed.
 */
typedef struct med_amp_control_ack {
    uint32_t magic;
    uint16_t version;
    /* 0 accepted; otherwise 1 + the index of the first rejected option. */
    uint16_t rejectedIndex;
    char detail[MED_AMP_CONTROL_DETAIL_SIZE]; /* NUL terminated */
} med_amp_control_ack;

#pragma pack(pop)

/* ------------------------------------------------------ layout, asserted */

/*
 * C11 and C++ have a static assertion; C99 does not, so it gets the negative
 * array size idiom, which fails to compile with the same effect. The message
 * names the field either way - in C99 through the typedef's name.
 */
#if defined(__cplusplus)
#define MED_AMP_ASSERT(name, cond) static_assert(cond, "AMP ABI: " #name)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MED_AMP_ASSERT(name, cond) _Static_assert(cond, "AMP ABI: " #name)
#else
#define MED_AMP_ASSERT(name, cond) \
    typedef char med_amp_abi_assert_##name[(cond) ? 1 : -1]
#endif

MED_AMP_ASSERT(frame_size, sizeof(med_amp_frame_header) == 40);
MED_AMP_ASSERT(frame_magic, offsetof(med_amp_frame_header, magic) == 0);
MED_AMP_ASSERT(frame_version, offsetof(med_amp_frame_header, version) == 4);
MED_AMP_ASSERT(frame_channelCount, offsetof(med_amp_frame_header, channelCount) == 6);
MED_AMP_ASSERT(frame_samplesPerChannel, offsetof(med_amp_frame_header, samplesPerChannel) == 8);
MED_AMP_ASSERT(frame_sampleRateMilliHz, offsetof(med_amp_frame_header, sampleRateMilliHz) == 12);
MED_AMP_ASSERT(frame_sequence, offsetof(med_amp_frame_header, sequence) == 16);
MED_AMP_ASSERT(frame_timestampMicros, offsetof(med_amp_frame_header, timestampMicros) == 24);
MED_AMP_ASSERT(frame_scaleNanoUnitsPerLsb, offsetof(med_amp_frame_header, scaleNanoUnitsPerLsb) == 32);
MED_AMP_ASSERT(frame_crc32, offsetof(med_amp_frame_header, crc32) == 36);

MED_AMP_ASSERT(lead_off_size, sizeof(med_amp_lead_off) == 8);
MED_AMP_ASSERT(lead_off_monitoredPositive, offsetof(med_amp_lead_off, monitoredPositive) == 0);
MED_AMP_ASSERT(lead_off_monitoredNegative, offsetof(med_amp_lead_off, monitoredNegative) == 2);
MED_AMP_ASSERT(lead_off_offPositive, offsetof(med_amp_lead_off, offPositive) == 4);
MED_AMP_ASSERT(lead_off_offNegative, offsetof(med_amp_lead_off, offNegative) == 6);

MED_AMP_ASSERT(option_size, sizeof(med_amp_control_option) == 56);
MED_AMP_ASSERT(option_key, offsetof(med_amp_control_option, key) == 0);
MED_AMP_ASSERT(option_value, offsetof(med_amp_control_option, value) == 32);

MED_AMP_ASSERT(control_size, sizeof(med_amp_control_message) == 464);
MED_AMP_ASSERT(control_magic, offsetof(med_amp_control_message, magic) == 0);
MED_AMP_ASSERT(control_version, offsetof(med_amp_control_message, version) == 4);
MED_AMP_ASSERT(control_optionCount, offsetof(med_amp_control_message, optionCount) == 6);
MED_AMP_ASSERT(control_crc32, offsetof(med_amp_control_message, crc32) == 8);
MED_AMP_ASSERT(control_reserved, offsetof(med_amp_control_message, reserved) == 12);
MED_AMP_ASSERT(control_options, offsetof(med_amp_control_message, options) == 16);

MED_AMP_ASSERT(ack_size, sizeof(med_amp_control_ack) == 72);
MED_AMP_ASSERT(ack_magic, offsetof(med_amp_control_ack, magic) == 0);
MED_AMP_ASSERT(ack_version, offsetof(med_amp_control_ack, version) == 4);
MED_AMP_ASSERT(ack_rejectedIndex, offsetof(med_amp_control_ack, rejectedIndex) == 6);
MED_AMP_ASSERT(ack_detail, offsetof(med_amp_control_ack, detail) == 8);

#undef MED_AMP_ASSERT

/* ------------------------------------------------------------------ CRC-32 */

/*
 * CRC-32 as IEEE 802.3 defines it (reflected, polynomial 0xEDB88320, initial
 * value and final XOR 0xFFFFFFFF; the "check" value of "123456789" is
 * 0xCBF43926). Bitwise and table-free on purpose: a producer on a small core
 * pays eight shifts per byte instead of a kilobyte of RAM, and at 456 bytes,
 * 18 frames a second, that is nothing. The Linux side keeps its own
 * table-driven implementation; tests/framework holds the two equal.
 */
static inline uint32_t med_amp_crc32(const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

#ifdef __cplusplus
}
#endif

#endif /* MED_AMP_ABI_H */
