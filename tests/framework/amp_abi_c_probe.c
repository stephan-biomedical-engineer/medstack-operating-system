/* SPDX-License-Identifier: MIT */
/*
 * The AMP wire format as a C compiler sees it.
 *
 * med_amp_abi.h is consumed by two languages: C++ in the framework, C in the
 * firmware. This file is the C side, and it is compiled three ways by the
 * Makefile:
 *
 *   - hosted, C99, and linked into the test binary, which compares the layout
 *     below with the one the C++ compiler computes from the same header;
 *   - freestanding with -nostdinc, against the compiler's own headers only,
 *     which proves the header needs no C library;
 *   - with arm-none-eabi-gcc for a Cortex-M33, which is the compiler that will
 *     build the firmware. The header's own static assertions are what is
 *     tested there: if that toolchain packed a field differently, this file
 *     would not compile.
 *
 * Plain C99 on purpose: it is the oldest dialect the header promises, and the
 * one in which its assertions use the typedef idiom instead of _Static_assert.
 */

#include "med_amp_abi.h"

/*
 * Sizes and offsets, in the order test_amp_abi.cpp lists them. Exported as
 * data rather than asserted here, so that a disagreement between the two
 * compilers is reported field by field instead of as one failed build.
 */
const size_t med_amp_c_layout[] = {
    sizeof(med_amp_frame_header),
    offsetof(med_amp_frame_header, magic),
    offsetof(med_amp_frame_header, version),
    offsetof(med_amp_frame_header, channelCount),
    offsetof(med_amp_frame_header, samplesPerChannel),
    offsetof(med_amp_frame_header, sampleRateMilliHz),
    offsetof(med_amp_frame_header, sequence),
    offsetof(med_amp_frame_header, timestampMicros),
    offsetof(med_amp_frame_header, scaleNanoUnitsPerLsb),
    offsetof(med_amp_frame_header, crc32),

    sizeof(med_amp_lead_off),
    offsetof(med_amp_lead_off, monitoredPositive),
    offsetof(med_amp_lead_off, monitoredNegative),
    offsetof(med_amp_lead_off, offPositive),
    offsetof(med_amp_lead_off, offNegative),

    sizeof(med_amp_control_option),
    offsetof(med_amp_control_option, key),
    offsetof(med_amp_control_option, value),

    sizeof(med_amp_control_message),
    offsetof(med_amp_control_message, magic),
    offsetof(med_amp_control_message, version),
    offsetof(med_amp_control_message, optionCount),
    offsetof(med_amp_control_message, crc32),
    offsetof(med_amp_control_message, reserved),
    offsetof(med_amp_control_message, options),

    sizeof(med_amp_control_ack),
    offsetof(med_amp_control_ack, magic),
    offsetof(med_amp_control_ack, version),
    offsetof(med_amp_control_ack, rejectedIndex),
    offsetof(med_amp_control_ack, detail),
};

const size_t med_amp_c_layout_count =
    sizeof(med_amp_c_layout) / sizeof(med_amp_c_layout[0]);

/* The header's CRC, compiled by a C compiler. */
uint32_t med_amp_c_crc32(const void *data, size_t length);

uint32_t med_amp_c_crc32(const void *data, size_t length)
{
    return med_amp_crc32(data, length);
}
