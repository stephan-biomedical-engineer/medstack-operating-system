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
 * The sample clock, derived from SysTick at 1 kHz. Every ms_per_sample-th tick
 * is a sample instant - the synthetic source's only, which is why that source
 * has no rate above 1000. ms_ticks is ours and 64-bit: HAL's uwTick is 32-bit
 * and wraps after 49.7 days, and a timestamp that wraps is a timestamp that
 * goes backwards.
 */
static volatile uint32_t ms_per_sample = 1000u / MED_PRODUCER_RATE_HZ; /* 4 */

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
    if (ms_ticks % ms_per_sample == 0u) {
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
static uint32_t conversions_lost_seen; /* the interrupt's losses already answered for */
#define DRAIN_BATCH (2u * MED_PRODUCER_SAMPLES_PER_FRAME)
static uint32_t work_us_per_conversion; /* over the last full pass of the loop */
/* The gain in force as the whole number it is, and the sensor channel's own. */
static uint32_t gain_in_force = 24u, gain_of_sensor = 12u;
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
                afe_registers(afe_rate_field(producer.rate_hz), afe_pga_field(producer.gain),
                              producer.test_signal == MED_TEST_SIGNAL_INTERNAL,
                              producer.bias_drive, producer.lead_off,
                              producer.montage == MED_MONTAGE_REFERENTIAL,
                              producer.input == MED_INPUT_SHORTED, producer.temperature,
                              &regs);
                if (afe_apply(&regs) != AFE_OK) {
                    med_producer_refuse(&producer, &pending_ack,
                                        "the converter did not take the settings");
                } else {
                    struct afe_counters ac;
                    afe_counters(&ac);
                    conversions_lost_seen = ac.overruns + ac.spi_errors;
                    gain_in_force = (uint32_t)producer.gain;
                    gain_of_sensor = (uint32_t)afe_sensor_gain(producer.gain);
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
        if (producer.streaming) {
            ms_per_sample = 1000u / producer.rate_hz;
        }
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
static int drain_converter(void)
{
    struct afe_sample s;
    struct afe_counters ac;
    int32_t nv[AFE_CHANNELS];
    uint8_t off_p, off_n;
    unsigned c;
    unsigned taken = 0;
    const uint64_t began = med_now_us();

    /*
     * At most two frames' worth per pass, then back to the loop. On the
     * board, 2026-10-13, at 1000 SPS: this function was written to run until
     * the queue was empty, the loop could not empty it, and so it never
     * returned - no control message was read (a prescription for another rate
     * went unanswered), no report line was printed, and the losses below were
     * looked at once and never again. A producer that cannot keep a rate must
     * still be able to be told to stop keeping it.
     */
    while (taken < DRAIN_BATCH && afe_take(&s)) {
        ++taken;
        ++conversions_taken;

        /* Conversions the interrupt could not keep. They are nowhere in the
         * queue, so the frame being filled is short of them: the producer
         * gives it up, and the receiver sees a gap instead of fourteen
         * samples that are not consecutive. Looked at for every conversion:
         * a loss noticed late would be charged to the wrong frame. */
        afe_counters(&ac);
        if (ac.overruns + ac.spi_errors != conversions_lost_seen) {
            conversions_lost_seen = ac.overruns + ac.spi_errors;
            med_producer_on_lost(&producer);
        }

        if (!afe_status_valid(s.raw)) {
            ++bad_status;
            med_producer_on_lost(&producer);
            /* Say which, and what it held: a desync at conversion 1 is a
             * start-up artefact, one in mid-stream is a timing defect. */
            if (bad_status <= 4u) {
                printf("[afe] bad status %02x %02x %02x at conversion %lu of this run\r\n",
                       s.raw[0], s.raw[1], s.raw[2], (unsigned long)conversions_taken);
            }
            continue;
        }
        for (c = 0; c < AFE_CHANNELS; ++c) {
            /* The sensor's channel has a gain of its own (afe_regs.h). */
            nv[c] = afe_code_to_nv_exact(afe_code24(&s.raw[3 + 3 * c]),
                                         (producer.temperature && c == MED_TEMPERATURE_CHANNEL)
                                             ? gain_of_sensor
                                             : gain_in_force);
        }
        /* The conversion's own lead-off bits, from its status word; the
         * producer drops them unless detection was prescribed. */
        afe_status_lead_off(s.raw, &off_p, &off_n);
        if (med_producer_on_values(&producer, s.at_us, nv, off_p, off_n, frame)) {
            send_frame();
        }
    }

    /* What one conversion costs this loop, frames and their sending included,
     * on the wall clock: the interrupts that preempted the pass are inside the
     * number. Against the sample period, it is whether a rate can be kept. */
    if (taken == DRAIN_BATCH) {
        work_us_per_conversion = (uint32_t)((med_now_us() - began) / DRAIN_BATCH);
    }
    return taken == DRAIN_BATCH;
}
#endif

/* ------------------------------------------------------------------ cache */

/*
 * The data cache, switched on. This firmware is linked into DDR
 * (stm32mp2xx_DDR_m33_ns.ld) and until 2026-10-13 ran there uncached: every
 * instruction and every variable was a DDR access. The bench measured the
 * result before anyone knew its cause - 222 us for a 27-byte read, 828 us of
 * loop per conversion, a core that could not keep 1000 conversions a second.
 * With this cache on the read takes 59 us and the same core keeps 8000, and
 * 16000 with the read at twice the bus speed (afe.c).
 *
 * How it is set up was found on the board, one variant at a time, loaded
 * through the remoteproc's sysfs (implementation_plan_hmi_desktop.md,
 * section 12.14). What each one did:
 *
 *   ST's example, whole     MPU regions for the instruction cache's alias,
 *                           the data region as write-back and the shared
 *                           megabyte uncached, then both caches. The firmware
 *                           stopped inside the creation of its endpoint, with
 *                           no fault line. It is that example's
 *                           ICACHE_DCACHE_USE path, which this firmware had
 *                           never been built with and nobody here had seen
 *                           run.
 *   instruction cache only  ran, and changed nothing: the read stayed at
 *                           241 us. The code is fetched from where the data
 *                           is, and it is the data cache that serves it.
 *   data cache, no MPU      ran four times faster and sent sixteen frames in
 *                           ten seconds: the rpmsg rings were cached, so the
 *                           buffers Linux gave back were never seen.
 *   data cache, and the MPU describing ONLY the shared megabyte, uncached
 *                           this function. Everything else is the default
 *                           memory map.
 *
 * So the one region is the whole point. The resource table, the trace buffer
 * Linux reads (both linked into IPC_SHMEM_1) and the rpmsg rings and buffers
 * (the devicetree's vdev0vring0/1 and vdev0buffer) all live in that megabyte,
 * and nothing the other processor reads or writes may sit in a cache it
 * cannot see. Its bounds are the linker script's IPC_SHMEM_1 + VIRTIO_SHMEM.
 *
 * Why ST's other two regions stop this firmware is NOT known; they were
 * removed, not understood. A step that fails here is reported in the trace
 * and the firmware goes on uncached: slow, and saying why.
 */
static DCACHE_HandleTypeDef hdcache;
static int dcache_on;

static void enable_data_cache(void)
{
    MPU_Region_InitTypeDef region = {0};
    MPU_Attributes_InitTypeDef attributes = {0};

    HAL_MPU_Disable();

    attributes.Attributes = INNER_OUTER(MPU_NOT_CACHEABLE);
    attributes.Number = MPU_ATTRIBUTES_NUMBER1;
    HAL_MPU_ConfigMemoryAttributes(&attributes);

    region.Enable = MPU_REGION_ENABLE;
    region.Number = MPU_REGION_NUMBER2;
    region.AttributesIndex = MPU_ATTRIBUTES_NUMBER1;
    region.BaseAddress = 0x81200000;
    region.LimitAddress = 0x812FFFFF;
    region.AccessPermission = MPU_REGION_ALL_RW;
    region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    region.IsShareable = MPU_ACCESS_INNER_SHAREABLE;
    HAL_MPU_ConfigRegion(&region);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

    hdcache.Instance = DCACHE;
    hdcache.Init.ReadBurstType = DCACHE_READ_BURST_WRAP;
    dcache_on = HAL_DCACHE_Init(&hdcache) == HAL_OK; /* enables it */
}

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
    enable_data_cache(); /* where ST's example does it: straight after HAL_Init() */
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
    printf("[med] data cache %s\r\n", dcache_on ? "on" : "NOT ON - this core will be slow");
    printf("[med] CPACR 0x%08lx (CP10/CP11 %s)\r\n", (unsigned long)SCB->CPACR,
           ((SCB->CPACR >> 20) & 0xFu) == 0xFu ? "enabled" : "DENIED - floating point will fault");
    printf("[med] core clock %lu Hz, %u ch x %u samples, %u SPS unless prescribed, frame %u bytes\r\n",
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

    printf("[med] link: OpenAMP init\r\n");
    if (MX_OPENAMP_Init(RPMSG_REMOTE, NULL) != 0) {
        printf("[med] OpenAMP init failed\r\n");
        Error_Handler();
    }
    printf("[med] link: creating the endpoint\r\n");
    if (OPENAMP_create_endpoint(&endpoint, CHANNEL_NAME, RPMSG_ADDR_ANY,
                                on_message, on_unbind) < 0) {
        printf("[med] endpoint '%s' could not be created\r\n", CHANNEL_NAME);
        Error_Handler();
    }
    printf("[med] endpoint '%s' announced; waiting for a prescription\r\n", CHANNEL_NAME);

    for (;;) {
        int behind = 0;

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
        behind = drain_converter();
#endif

        if (ms_ticks >= next_report_ms) {
            struct afe_counters ac;
            afe_counters(&ac);
            next_report_ms += 10000u;
            printf("[med] t=%lus streaming=%d %luSPS seq=%lu sent=%lu dropped=%lu "
                   "broken=%lu | drdy=%lu lost=%lu spi_err=%lu bad_status=%lu read=%luus "
                   "queue=%lu work=%luus\r\n",
                   (unsigned long)(ms_ticks / 1000u), producer.streaming,
                   (unsigned long)producer.rate_hz,
                   (unsigned long)producer.sequence, (unsigned long)frames_sent,
                   (unsigned long)frames_dropped, (unsigned long)producer.frames_broken,
                   (unsigned long)ac.drdy,
                   (unsigned long)ac.overruns, (unsigned long)ac.spi_errors,
                   (unsigned long)bad_status, (unsigned long)ac.spi_us_last,
                   (unsigned long)ac.queue_high, (unsigned long)work_us_per_conversion);
        }

        /* With conversions still waiting there is nothing to sleep for. */
        if (!behind) {
            __WFI();
        }
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
