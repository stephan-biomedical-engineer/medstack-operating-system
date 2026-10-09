/* SPDX-License-Identifier: MIT */
/*
 * MedPlatform Cortex-M33 producer: the converter on the header, or nothing.
 *
 * At start-up the converter is probed once (afe.c). Found, every sample is
 * the converter's, timed by its DRDY interrupt. Not found, every prescription
 * is refused with the reason, and nothing streams. The synthetic generator of
 * phase 3 is built only with -DMED_M33_SYNTHETIC, for a bench without a
 * converter - never as a fallback (med_producer.h says why).
 *
 * What this file owns, and the only things it owns: the clock that decides
 * when a sample exists, the rpmsg endpoint Linux reaches it through, and the
 * order in which the two meet. Everything about the content of a frame or an
 * answer is med_producer.c, which is compiled and tested on the host too.
 *
 * Shape:
 *
 *   SysTick, every 1 ms   - every 4th tick is a sample instant at 250 SPS; its
 *                           time is captured HERE, in the interrupt, and queued.
 *   main loop             - mailbox poll (which runs the rpmsg callback),
 *                           then one med_producer_on_sample() per queued
 *                           instant, sending each frame that completes.
 *
 * The sample's time is taken in the interrupt and not in the loop because the
 * timestamp is the reason this link exists: on "amp" a sample is dated on the
 * core that took it, at the instant it was taken (implementation_plan_
 * m33_firmware.md §6.4). Taken in the loop, it would carry the loop's latency,
 * which is exactly the error the "usb" link has and this one must not.
 *
 * The clock is the M33's own, counted from when this firmware started. It is
 * a phase, not a date; turning it into wall time is the Linux side's problem
 * and is not solved there yet (RESULTS.md §9).
 */

#include <stdio.h>
#include <string.h>

#include "stm32mp2xx_hal.h"
#include "openamp.h"
#include "copro_sync.h"
#include "med_producer.h"
#include "afe.h"

IPCC_HandleTypeDef hipcc1;

void Error_Handler(void);

/* --------------------------------------------------------------- the clock */

/*
 * The sample clock, derived from SysTick at 1 kHz. Every MS_PER_SAMPLE-th tick
 * is a sample instant. ms_ticks is ours and 64-bit: HAL's uwTick is 32-bit
 * and wraps after 49.7 days, and a timestamp that wraps is a timestamp that
 * goes backwards.
 */
#define MS_PER_SAMPLE (1000u / MED_PRODUCER_RATE_HZ) /* 4 */

static volatile uint64_t ms_ticks;

/*
 * Sample instants waiting for the loop. Single producer (SysTick), single
 * consumer (main loop), so two indices and no lock. 64 entries is 256 ms of
 * samples; a loop that falls that far behind is broken, and that is counted
 * rather than absorbed.
 */
#define PENDING 64u
static volatile uint64_t pending_us[PENDING];
static volatile uint32_t pending_head;
static uint32_t pending_tail;
static volatile uint32_t overruns;

/*
 * Microseconds since this firmware started: the elapsed whole milliseconds,
 * plus how far the down-counter has run since it reloaded - which, read in an
 * interrupt, is that interrupt's own entry latency, and is kept instead of
 * rounded away because it is the jitter phase 5 has to measure.
 *
 * SysTick runs at the HIGHEST priority (main() sets it), so a reload is
 * followed at once by its handler, and ms_ticks can only change by preempting
 * this function. Reading the count on both sides of the counter and retrying
 * if it moved therefore gives a consistent pair from any context.
 *
 * The first version ran the DRDY handler ABOVE SysTick and corrected with the
 * pending bit. That misses one case: DRDY preempting the SysTick handler
 * itself, after the pending bit was cleared and before ++ms_ticks. The bench
 * saw it as frame intervals of 55009 and 57009 us - one whole millisecond
 * wrong in some stamps (BRINGUP_STM32MP2.md §9.16).
 */
uint64_t med_now_us(void)
{
    const uint32_t load = SysTick->LOAD + 1u;
    uint64_t ms;
    uint32_t val;

    do {
        ms = ms_ticks;
        val = SysTick->VAL;
    } while (ms != ms_ticks);
    return ms * 1000u + (uint64_t)(load - val) * 1000u / load;
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    ++ms_ticks;

#ifdef MED_M33_SYNTHETIC
    if (ms_ticks % MS_PER_SAMPLE == 0u) {
        const uint32_t head = pending_head;
        if (head - pending_tail >= PENDING) {
            ++overruns;
            return;
        }
        pending_us[head % PENDING] = med_now_us();
        pending_head = head + 1u;
    }
#endif
}

/* ------------------------------------------------------------ the endpoint */

/*
 * The channel name is the contract with Linux: rpmsg_char binds itself to a
 * channel called "rpmsg-raw" and nothing else (drivers/rpmsg/rpmsg_char.c), and
 * that is what makes a /dev/rpmsgN appear without an ioctl.
 */
#define CHANNEL_NAME "rpmsg-raw"

static struct rpmsg_endpoint endpoint;
static struct med_producer producer;

/* The answer to the last control message, sent from the loop and not from the
 * callback, so that nothing is transmitted while a receive buffer is held. */
static med_amp_control_ack pending_ack;
static int ack_due;
static uint32_t linux_addr = RPMSG_ADDR_ANY;

static uint8_t frame[MED_PRODUCER_FRAME_BYTES];
static uint32_t bad_status;
static uint32_t conversions_taken; /* since the last afe_start() */
static uint32_t frames_sent;
static uint32_t frames_dropped;

static int on_message(struct rpmsg_endpoint *ept, void *data, size_t length,
                      uint32_t src, void *priv)
{
    (void)ept;
    (void)priv;

    printf("[med] message from 0x%lx, %u bytes\r\n", (unsigned long)src, (unsigned)length);
    if (med_producer_on_control(&producer, data, length, &pending_ack)) {
        linux_addr = src;
        ack_due = 1;
        /*
         * The converter is reprogrammed for every prescription, accepted or
         * not: a refused one stops it, and an accepted one is only accepted
         * once the registers have been written AND read back. The answer
         * leaves after that, so "accepted" means "configured".
         */
        if (producer.source == MED_SOURCE_CONVERTER) {
            afe_stop();
            if (producer.streaming) {
                struct afe_regs regs;
                afe_registers(afe_pga_field(producer.gain),
                              producer.test_signal == MED_TEST_SIGNAL_INTERNAL,
                              producer.bias_drive, producer.lead_off, &regs);
                if (afe_apply(&regs) != AFE_OK) {
                    med_producer_refuse(&producer, &pending_ack,
                                        "the converter did not take the settings");
                } else {
                    conversions_taken = 0u;
                    if (afe_start() != AFE_OK) {
                        med_producer_refuse(&producer, &pending_ack,
                                            "the converter did not start");
                    }
                }
            }
        }
        /* Instants queued before this answer belong to no session: dropping
         * them is what keeps the first frame after the ack starting from the
         * first sample after the ack. */
        pending_tail = pending_head;
    } else {
        printf("[med] ignored a %u-byte message (malformed: %lu so far)\r\n",
               (unsigned)length, (unsigned long)producer.controls_malformed);
    }
    return RPMSG_SUCCESS;
}

static void on_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    producer.streaming = 0;
    linux_addr = RPMSG_ADDR_ANY;
}

static void send_ack(void)
{
    int ret = rpmsg_sendto(&endpoint, &pending_ack, sizeof(pending_ack), linux_addr);
    ack_due = 0;
    printf("[med] control %s (accepted %lu, rejected %lu): %s - ack %s\r\n",
           pending_ack.rejectedIndex ? "REJECTED" : "accepted",
           (unsigned long)producer.controls_accepted,
           (unsigned long)producer.controls_rejected,
           pending_ack.detail, ret < 0 ? "NOT SENT" : "sent");
}

/*
 * A frame that cannot be handed to the transport right now is dropped, not
 * waited for: waiting would delay every later sample, and a late sample with a
 * correct timestamp is still a lost sample to a viewer, while a dropped frame
 * is a sequence gap the receiver sees and counts. The trysend variant never
 * blocks.
 */
static void send_frame(void)
{
    if (linux_addr == RPMSG_ADDR_ANY ||
        rpmsg_trysendto(&endpoint, frame, sizeof(frame), linux_addr) < 0) {
        ++frames_dropped;
        return;
    }
    ++frames_sent;
}

/* ------------------------------------------------------------- converter */


#ifndef MED_M33_SYNTHETIC

/*
 * Conversions the DRDY handler queued, turned into nanovolts and frames. A
 * frame whose status word does not start with 1100 was read out of step with
 * the converter; its samples are not samples, so it is dropped and counted -
 * never converted into a waveform.
 */
static void drain_converter(void)
{
    struct afe_sample s;
    int32_t nv[AFE_CHANNELS];
    uint8_t off_p, off_n;
    unsigned c;

    while (afe_take(&s)) {
        ++conversions_taken;
        if (!afe_status_valid(s.raw)) {
            ++bad_status;
            /* Say which, and what it held: a desync at conversion 1 is a
             * start-up artefact, one in mid-stream is a timing defect. */
            if (bad_status <= 4u) {
                printf("[afe] bad status %02x %02x %02x at conversion %lu of this run\r\n",
                       s.raw[0], s.raw[1], s.raw[2], (unsigned long)conversions_taken);
            }
            continue;
        }
        for (c = 0; c < AFE_CHANNELS; ++c) {
            nv[c] = afe_code_to_nv(afe_code24(&s.raw[3 + 3 * c]), producer.gain, AFE_VREF_UV);
        }
        /* The conversion's own lead-off bits, from its status word; the
         * producer drops them unless detection was prescribed. */
        afe_status_lead_off(s.raw, &off_p, &off_n);
        if (med_producer_on_values(&producer, s.at_us, nv, off_p, off_n, frame)) {
            send_frame();
        }
    }
}
#endif

/* ------------------------------------------------------------------- main */

static void MX_IPCC_Init(void)
{
    hipcc1.Instance = IPCC1;
    if (HAL_IPCC_Init(&hipcc1) != HAL_OK) {
        Error_Handler();
    }
    HAL_NVIC_SetPriority(IPCC1_RX_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(IPCC1_RX_IRQn);
}

/*
 * The FPU, before anything can execute a floating-point instruction - which
 * means before main(), not at its first line.
 *
 * The firmware is built for the hard-float ABI, and ST's non-secure
 * SystemInit() for this core does not enable CP10/CP11: it sets VTOR and the
 * clock variable and nothing else. The first FPU instruction then raises a
 * UsageFault (NOCP), escalated to a HardFault.
 *
 * This used to be the first statement of main(), and that worked only while
 * main() had no floating point in it. Once the converter path was inlined
 * into main(), its PROLOGUE saved d8 (vpush) before the first statement ran,
 * and the firmware faulted at main+4 (BRINGUP_STM32MP2.md §9.16: CFSR
 * 0x00080000, PC 0x80101114). A constructor runs from __libc_init_array(),
 * after SystemInit() and before main(), and has no floating point of its own.
 *
 * Under TrustZone the secure side can deny non-secure FPU access (NSACR), in
 * which case this write is ignored; main() logs the read-back.
 */
__attribute__((constructor(101))) static void enable_fpu(void)
{
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    __DSB();
    __ISB();
}

int main(void)
{
    uint64_t next_report_ms = 10000u;

    HAL_Init();
    MX_IPCC_Init();
    if (!IS_DEVELOPER_BOOT_MODE()) {
        CoproSync_Init();
    }

    /*
     * HAL_Init() programmed SysTick from whatever SystemCoreClock held at reset.
     * Once the real core clock is read back, the tick has to be programmed
     * again, or every "4 ms" is 4 ms of the wrong clock - and every timestamp
     * and the sample rate with it. ST's example updates the variable and never
     * re-programs the tick; this is the line it lacks.
     */
    SystemCoreClockUpdate();
    /* Priority 0, the highest: see med_now_us() for why the clock has to
     * outrank the DRDY interrupt (1) and the mailbox (2). */
    HAL_InitTick(0);

    printf("\r\n[med] MedPlatform M33 producer (%s %s)\r\n", __DATE__, __TIME__);
    printf("[med] CPACR 0x%08lx (CP10/CP11 %s)\r\n", (unsigned long)SCB->CPACR,
           ((SCB->CPACR >> 20) & 0xFu) == 0xFu ? "enabled" : "DENIED - floating point will fault");
    printf("[med] core clock %lu Hz, %u ch x %u samples at %u SPS, frame %u bytes\r\n",
           (unsigned long)SystemCoreClock, MED_PRODUCER_CHANNELS,
           MED_PRODUCER_SAMPLES_PER_FRAME, MED_PRODUCER_RATE_HZ,
           (unsigned)MED_PRODUCER_FRAME_BYTES);

    med_producer_init(&producer);

#ifdef MED_M33_SYNTHETIC
    med_producer_set_source(&producer, MED_SOURCE_SYNTHETIC, NULL);
    printf("[med] SYNTHETIC build: no converter is used\r\n");
#else
    /* The converter on the header, once, before the endpoint exists. */
    switch (afe_probe()) {
    case AFE_OK:
        med_producer_set_source(&producer, MED_SOURCE_CONVERTER, NULL);
        break;
    case AFE_NOT_OWNED:
        med_producer_set_source(&producer, MED_SOURCE_ABSENT,
                                "SPI6 is not assigned to the M33 (RIF)");
        break;
    case AFE_WRONG_ID:
        med_producer_set_source(&producer, MED_SOURCE_ABSENT,
                                "no converter answered on SPI6");
        break;
    default:
        med_producer_set_source(&producer, MED_SOURCE_ABSENT, "SPI6 transfer failed");
        break;
    }
    printf("[med] source: %s\r\n", producer.source == MED_SOURCE_CONVERTER
                                       ? "converter" : producer.absent_reason);
#endif

    if (MX_OPENAMP_Init(RPMSG_REMOTE, NULL) != 0) {
        printf("[med] OpenAMP init failed\r\n");
        Error_Handler();
    }
    if (OPENAMP_create_endpoint(&endpoint, CHANNEL_NAME, RPMSG_ADDR_ANY,
                                on_message, on_unbind) < 0) {
        printf("[med] endpoint '%s' could not be created\r\n", CHANNEL_NAME);
        Error_Handler();
    }
    printf("[med] endpoint '%s' announced; waiting for a prescription\r\n", CHANNEL_NAME);

    for (;;) {
        OPENAMP_check_for_message();

        if (ack_due) {
            send_ack();
        }

#ifdef MED_M33_SYNTHETIC
        while (pending_tail != pending_head) {
            const uint64_t at = pending_us[pending_tail % PENDING];
            ++pending_tail;
            if (med_producer_on_sample(&producer, at, frame)) {
                send_frame();
            }
        }
#else
        drain_converter();
#endif

        if (ms_ticks >= next_report_ms) {
            struct afe_counters ac;
            afe_counters(&ac);
            next_report_ms += 10000u;
            printf("[med] t=%lus streaming=%d seq=%lu sent=%lu dropped=%lu | drdy=%lu "
                   "lost=%lu spi_err=%lu bad_status=%lu read=%luus\r\n",
                   (unsigned long)(ms_ticks / 1000u), producer.streaming,
                   (unsigned long)producer.sequence, (unsigned long)frames_sent,
                   (unsigned long)frames_dropped, (unsigned long)ac.drdy,
                   (unsigned long)ac.overruns, (unsigned long)ac.spi_errors,
                   (unsigned long)bad_status, (unsigned long)ac.spi_us_last);
        }

        __WFI();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}

/*
 * ST's HAL configuration keeps USE_FULL_ASSERT on, and so does this firmware: a
 * HAL call with an invalid argument stops here, with the file and line in the
 * trace buffer, instead of carrying on with a peripheral half configured.
 */
void assert_failed(uint8_t *file, uint32_t line);

void assert_failed(uint8_t *file, uint32_t line)
{
    printf("[med] HAL assertion failed: %s:%lu\r\n", (const char *)file, (unsigned long)line);
    Error_Handler();
}

/*
 * Linux is about to stop this core (IPCC channel 3, CoproSync). ST's weak
 * version only acknowledges; this one leaves the converter powered down first,
 * so the next firmware - or nothing - does not inherit a part converting in
 * RDATAC with nobody reading it. Runs in the IPCC interrupt, below SysTick, so
 * the blocking SPI transfers in afe_stop() still see the tick advance.
 */
void CoproSync_ShutdownCb(IPCC_HandleTypeDef *hipcc, uint32_t ChannelIndex,
                          IPCC_CHANNELDirTypeDef ChannelDir)
{
    (void)ChannelDir;
    if (producer.source == MED_SOURCE_CONVERTER) {
        afe_power_down();
    }
    HAL_IPCC_NotifyCPU(hipcc, ChannelIndex, IPCC_CHANNEL_DIR_RX);
}

/* ------------------------------------------------------- the other vectors */

void IPCC1_RX_IRQHandler(void)
{
    HAL_IPCC_RX_IRQHandler(&hipcc1);
}

/*
 * A fault says what it was before it stops. ST's handlers spin in silence,
 * which on a core with no debugger attached turns every fault into "the
 * firmware went quiet"; the bench lost an hour to exactly that. The stacked
 * PC is the instruction that faulted - look it up in med_m33_producer.map.
 */
static void report_fault(const char *what, const uint32_t *stacked)
{
    printf("[med] %s: CFSR 0x%08lx HFSR 0x%08lx MMFAR 0x%08lx BFAR 0x%08lx PC 0x%08lx LR 0x%08lx\r\n",
           what, (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR,
           (unsigned long)SCB->MMFAR, (unsigned long)SCB->BFAR,
           (unsigned long)stacked[6], (unsigned long)stacked[5]);
    Error_Handler();
}

#define FAULT_HANDLER(name, what)                                        \
    __attribute__((naked)) void name(void)                               \
    {                                                                    \
        __asm volatile("tst lr, #4\n"                                    \
                       "ite eq\n"                                        \
                       "mrseq r1, msp\n"                                 \
                       "mrsne r1, psp\n"                                 \
                       "ldr r0, =1f\n"                                   \
                       "b report_fault_trampoline\n"                     \
                       "1: .asciz \"" what "\"\n"                       \
                       ".align 2\n");                                    \
    }

void report_fault_trampoline(const char *what, const uint32_t *stacked);
void report_fault_trampoline(const char *what, const uint32_t *stacked)
{
    report_fault(what, stacked);
}

FAULT_HANDLER(HardFault_Handler, "HardFault")
FAULT_HANDLER(MemManage_Handler, "MemManage")
FAULT_HANDLER(BusFault_Handler, "BusFault")
FAULT_HANDLER(UsageFault_Handler, "UsageFault")
FAULT_HANDLER(SecureFault_Handler, "SecureFault")

void NMI_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}
void SVC_Handler(void) {}
