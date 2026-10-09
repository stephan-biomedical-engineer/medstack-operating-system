/* SPDX-License-Identifier: MIT */
/*
 * The ADS1299 on the 40-pin header, driven from the Cortex-M33.
 *
 * Wiring (BRINGUP_STM32MP2.md §9.13) and where each fact comes from:
 *
 *   SPI6 SCK  PF7  AF3   header 23   stm32mp25-pinctrl.dtsi, spi6_pins_a
 *   SPI6 MOSI PC7  AF3   header 19   idem
 *   SPI6 MISO PC4  AF3   header 21   idem
 *   CS        PF4  GPIO  header 24   driven by hand, low across a whole command
 *   PWDN      PC10 GPIO  header 29   active low; driven high to power up
 *   DRDY      PH8  EXTI1 header 31   falling edge: a conversion is ready
 *
 * Every resource is checked with ST's resource manager before it is touched,
 * because touching SPI6 while it belongs to the A35 is an illegal access - and
 * writing an RCC register this core does not own was found to reset the whole
 * SoC (§9.15). That is also why nothing here enables a GPIO port clock.
 *
 * The data path is one interrupt per conversion. DRDY's falling edge enters
 * EXTI1_8_IRQHandler, which takes the time FIRST - the timestamp is the reason
 * the amp link exists - then reads the 27-byte frame and queues it. The loop
 * turns frames into nanovolts and sample frames. Reading in the interrupt is
 * deliberate: in RDATAC the converter overwrites its output on the next DRDY,
 * so the read has a 4 ms deadline at 250 SPS, and a loop that may be busy
 * sending an rpmsg message is not the place to keep it.
 */

#include "afe.h"

#include <stdio.h>
#include <string.h>

#include "stm32mp2xx_hal.h"
#include "res_mgr.h"

#define ADS_ID_FAMILY(id) (((id) >> 2) & 0x7u) /* 0b111 for this family */
#define ADS_ID_NCHAN(id)  ((id) & 0x3u)        /* 00: 4, 01: 6, 10: 8 */

/* SPI6's alternate function on these pins - stm32mp25-pinctrl.dtsi. There is
 * no GPIO_AF3_SPI6 in the HAL, and borrowing another peripheral's AF3 macro
 * would read as a claim about the wrong peripheral. */
#define AF_SPI6 ((uint8_t)0x03)

#define CS_PORT   GPIOF
#define CS_PIN    GPIO_PIN_4
#define PWDN_PORT GPIOC
#define PWDN_PIN  GPIO_PIN_10
#define DRDY_PORT GPIOH
#define DRDY_PIN  GPIO_PIN_8
#define DRDY_LINE (1u << 8) /* EXTI1 line 8 */

/*
 * Below SysTick (0) and above the IPCC mailbox (2). SysTick outranks this on
 * purpose: med_now_us() is only race-free if no interrupt can sit between a
 * SysTick reload and its handler (main.c). A SysTick preempting the 27-byte
 * read costs about a microsecond; the read has a 4 ms deadline.
 */
#define DRDY_IRQ_PRIORITY 1u

static SPI_HandleTypeDef spi6;

/* ------------------------------------------------------------- the queue */

#define QUEUE 32u /* 128 ms at 250 SPS */
static struct afe_sample queue[QUEUE];
static volatile uint32_t q_head;
static uint32_t q_tail;
static volatile struct afe_counters counters;

/* ---------------------------------------------------------------- the pins */

struct owned {
    const char *what;
    ResMgr_Res_Type_t type;
    uint8_t number;
};

static const struct owned kResources[] = {
    {"SPI6", RESMGR_RESOURCE_RIFSC, STM32MP25_RIFSC_SPI6_ID},
    {"PC4 (MISO)", RESMGR_RESOURCE_RIF_GPIOC, RESMGR_GPIO_PIN(4)},
    {"PC7 (MOSI)", RESMGR_RESOURCE_RIF_GPIOC, RESMGR_GPIO_PIN(7)},
    {"PF7 (SCK)", RESMGR_RESOURCE_RIF_GPIOF, RESMGR_GPIO_PIN(7)},
    {"PF4 (CS)", RESMGR_RESOURCE_RIF_GPIOF, RESMGR_GPIO_PIN(4)},
    {"PC10 (PWDN)", RESMGR_RESOURCE_RIF_GPIOC, RESMGR_GPIO_PIN(10)},
    {"PH8 (DRDY)", RESMGR_RESOURCE_RIF_GPIOH, RESMGR_GPIO_PIN(8)},
    {"EXTI1 line 8 (DRDY)", RESMGR_RESOURCE_RIF_EXTI1, 8},
};

static void cs_assert(int asserted)
{
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static HAL_StatusTypeDef transfer(const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    HAL_StatusTypeDef st;
    cs_assert(1);
    st = HAL_SPI_TransmitReceive(&spi6, (uint8_t *)tx, rx, n, 10);
    cs_assert(0);
    return st;
}

static HAL_StatusTypeDef command(uint8_t opcode)
{
    uint8_t rx;
    return transfer(&opcode, &rx, 1);
}

static void configure_pins(void)
{
    GPIO_InitTypeDef pin = {0};

    /* No __HAL_RCC_GPIOx_CLK_ENABLE(): those are the secure side's, and the
     * ports are clocked already because Linux drives other pins on them. The
     * first version of this file enabled them and reset the SoC (§9.15). */

    /* CS inactive (high) BEFORE the pin becomes an output, so the converter
     * never sees a falling edge it did not ask for. */
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET);
    pin.Pin = CS_PIN;
    pin.Mode = GPIO_MODE_OUTPUT_PP;
    pin.Pull = GPIO_NOPULL;
    pin.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(CS_PORT, &pin);

    pin.Pin = PWDN_PIN;
    HAL_GPIO_Init(PWDN_PORT, &pin);

    /*
     * DRDY on EXTI1 for THIS core. GPIO_EXTI1_IT_FALLING, never
     * GPIO_MODE_IT_FALLING: in this HAL the latter is an alias for the EXTI2
     * variant, which is the Cortex-M0+ domain's controller - the "obvious" name
     * would arm an interrupt the M33 never receives.
     */
    pin.Pin = DRDY_PIN;
    pin.Mode = GPIO_EXTI1_IT_FALLING;
    HAL_GPIO_Init(DRDY_PORT, &pin);

    pin.Mode = GPIO_MODE_AF_PP;
    pin.Alternate = AF_SPI6;
    pin.Speed = GPIO_SPEED_FREQ_MEDIUM;
    pin.Pin = GPIO_PIN_7; /* SCK */
    HAL_GPIO_Init(GPIOF, &pin);
    pin.Pin = GPIO_PIN_7 | GPIO_PIN_4; /* MOSI, MISO */
    HAL_GPIO_Init(GPIOC, &pin);
}

static HAL_StatusTypeDef configure_spi(void)
{
    /* SPI6's own gate follows RIFSC 27, which this core owns now:
     * clk-stm32mp25.c, SEC_RIFSC(27) for ck_icn_p_spi6 and ck_ker_spi6. */
    __HAL_RCC_SPI6_CLK_ENABLE();

    /*
     * Reset the peripheral itself, in the same RCC register. A firmware that
     * Linux stopped (echo stop > state resets the M33, not SPI6) can leave the
     * controller mid-transfer, and HAL_SPI_Init() configures it without
     * draining that - the bench read 0xFF from a converter that had answered
     * 0x3E minutes earlier (BRINGUP_STM32MP2.md §9.16).
     */
    __HAL_RCC_SPI6_FORCE_RESET();
    __HAL_RCC_SPI6_RELEASE_RESET();

    spi6.Instance = SPI6;
    spi6.Init.Mode = SPI_MODE_MASTER;
    spi6.Init.Direction = SPI_DIRECTION_2LINES;
    spi6.Init.DataSize = SPI_DATASIZE_8BIT;
    /* SPI mode 1: clock idles low, data sampled on the falling edge. */
    spi6.Init.CLKPolarity = SPI_POLARITY_LOW;
    spi6.Init.CLKPhase = SPI_PHASE_2EDGE;
    spi6.Init.NSS = SPI_NSS_SOFT;
    /*
     * Prescaler 32. The 27-byte read sits inside a 4 ms window and inside an
     * interrupt, so it should be short; the converter accepts up to 20 MHz.
     * What the kernel clock divided by 32 actually is was not read anywhere -
     * the duration of every read is measured instead (spi_us_last) and logged.
     */
    spi6.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
    spi6.Init.FirstBit = SPI_FIRSTBIT_MSB;
    spi6.Init.TIMode = SPI_TIMODE_DISABLE;
    spi6.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    spi6.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    spi6.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    /* Keep SCK driven between transfers: a floating clock line between the
     * opcode and the next byte is an edge the converter may count. */
    spi6.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    return HAL_SPI_Init(&spi6);
}

/* ------------------------------------------------------------------ probe */

int afe_probe(void)
{
    size_t i;
    int denied = 0;
    uint8_t tx[3] = {AFE_CMD_RREG | AFE_REG_ID, 0x00, 0x00};
    uint8_t rx[3] = {0, 0, 0};
    uint8_t id;

    for (i = 0; i < sizeof(kResources) / sizeof(kResources[0]); ++i) {
        ResMgr_Status_t st = ResMgr_Request(kResources[i].type, kResources[i].number);
        if (st != RESMGR_STATUS_ACCESS_OK) {
            printf("[afe] %s is not this core's (ResMgr status %u)\r\n", kResources[i].what,
                   (unsigned)st);
            ++denied;
        }
    }
    if (denied) {
        printf("[afe] %d resource(s) not assigned to the M33: front-end NOT probed.\r\n"
               "[afe] The OP-TEE devicetree in this FIP predates the amp-link RIF patch.\r\n",
               denied);
        return AFE_NOT_OWNED;
    }
    printf("[afe] SPI6, its pins and DRDY all belong to this core\r\n");

    HAL_NVIC_DisableIRQ(EXTI1_8_IRQn); /* armed only by afe_start() */
    configure_pins();

    /*
     * Power-cycle the converter through PWDN, whatever state it was left in:
     * low long enough to power it down, then high and the power-on reset,
     * 2^18 periods of its 2.048 MHz clock = 128 ms, rounded up. Without this a
     * part left converting in RDATAC by a firmware that was stopped from Linux
     * answers the probe with 0xFF (§9.16). Driving PWDN low is safe: it is
     * this core's pin, and powering down a converter nobody is reading is
     * the state a stopped device should be in anyway.
     */
    HAL_GPIO_WritePin(PWDN_PORT, PWDN_PIN, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(PWDN_PORT, PWDN_PIN, GPIO_PIN_SET);
    HAL_Delay(150);

    if (configure_spi() != HAL_OK) {
        printf("[afe] HAL_SPI_Init failed\r\n");
        return AFE_SPI_ERROR;
    }

    /* RESET by command (RESET is not on the header), then 18 converter clocks,
     * 8.8 us, rounded up to a tick; then SDATAC, because the part comes out of
     * reset in continuous-read mode and ignores RREG there. */
    if (command(AFE_CMD_RESET) != HAL_OK) {
        printf("[afe] RESET: SPI transfer failed\r\n");
        return AFE_SPI_ERROR;
    }
    HAL_Delay(1);
    if (command(AFE_CMD_SDATAC) != HAL_OK) {
        printf("[afe] SDATAC: SPI transfer failed\r\n");
        return AFE_SPI_ERROR;
    }
    HAL_Delay(1);

    if (transfer(tx, rx, sizeof(tx)) != HAL_OK) {
        printf("[afe] RREG ID: SPI transfer failed\r\n");
        return AFE_SPI_ERROR;
    }
    id = rx[2];
    printf("[afe] ID register: 0x%02x (family %u, channel field %u, revision %u); DRDY reads %d\r\n",
           id, ADS_ID_FAMILY(id), ADS_ID_NCHAN(id), (unsigned)(id >> 5),
           HAL_GPIO_ReadPin(DRDY_PORT, DRDY_PIN) == GPIO_PIN_SET);

    /* 0x00 and 0xFF are what an absent or unpowered part reads as - and on
     * 2026-10-07 0xFF is exactly what an unpowered board gave - and neither
     * decodes to the family, so the family test is also "is anything there". */
    if (ADS_ID_FAMILY(id) != 0x7u || ADS_ID_NCHAN(id) == 0x3u) {
        printf("[afe] not a converter of this family\r\n");
        return AFE_WRONG_ID;
    }
    printf("[afe] converter answered: %u channels\r\n",
           ADS_ID_NCHAN(id) == 0u ? 4u : ADS_ID_NCHAN(id) == 1u ? 6u : 8u);
    return AFE_OK;
}

/* ------------------------------------------------------------ configuration */

static HAL_StatusTypeDef write_registers(uint8_t first, const uint8_t *values, uint8_t n)
{
    uint8_t tx[2 + AFE_CHANNELS];
    uint8_t rx[sizeof(tx)];
    tx[0] = (uint8_t)(AFE_CMD_WREG | first);
    tx[1] = (uint8_t)(n - 1u);
    memcpy(&tx[2], values, n);
    return transfer(tx, rx, (uint16_t)(2u + n));
}

static HAL_StatusTypeDef read_registers(uint8_t first, uint8_t *values, uint8_t n)
{
    uint8_t tx[2 + AFE_CHANNELS] = {0};
    uint8_t rx[sizeof(tx)];
    HAL_StatusTypeDef st;
    tx[0] = (uint8_t)(AFE_CMD_RREG | first);
    tx[1] = (uint8_t)(n - 1u);
    st = transfer(tx, rx, (uint16_t)(2u + n));
    memcpy(values, &rx[2], n);
    return st;
}

/* Write n registers from first, read them back, compare under mask. The
 * mask exists for CONFIG3, whose bit 0 is a status bit (BIAS lead-off), not
 * configuration. */
static int write_and_verify(const char *what, uint8_t first, const uint8_t *values, uint8_t n,
                            uint8_t mask)
{
    uint8_t back[AFE_CHANNELS];
    unsigned i;

    if (write_registers(first, values, n) != HAL_OK ||
        read_registers(first, back, n) != HAL_OK) {
        printf("[afe] %s: SPI transfer failed\r\n", what);
        return AFE_SPI_ERROR;
    }
    for (i = 0; i < n; ++i) {
        if ((back[i] & mask) != values[i]) {
            printf("[afe] %s+%u read back %02x, wrote %02x\r\n", what, i, back[i], values[i]);
            return AFE_SPI_ERROR;
        }
    }
    return AFE_OK;
}

/*
 * The status word's lead-off bits against the LOFF_STAT registers, from one
 * conversion taken outside continuous mode: START, wait for DRDY, RDATA, STOP,
 * then read the registers. The layout in afe_status_lead_off() has no oracle
 * but the part itself, and this is the part answering the same question by two
 * paths. A contact that changes between the conversion and the register read
 * would also disagree, so one retry is allowed before it is called a mismatch.
 */
static int check_status_layout(void)
{
    int attempt;

    for (attempt = 0; attempt < 2; ++attempt) {
        uint8_t tx[1 + AFE_FRAME_BYTES] = {AFE_CMD_RDATA};
        uint8_t rx[sizeof(tx)];
        uint8_t stat[2];
        uint8_t from_word_p, from_word_n;
        int read;

        if (command(AFE_CMD_START) != HAL_OK) {
            return AFE_SPI_ERROR;
        }
        /* Two conversions, the first discarded: DRDY may already be low from
         * one nobody read, and a read raises it [SBAS499?], so the second low
         * is fresh. If that claim is wrong, the retry below absorbs it. */
        for (read = 0; read < 2; ++read) {
            const uint32_t t0 = HAL_GetTick();
            while (HAL_GPIO_ReadPin(DRDY_PORT, DRDY_PIN) == GPIO_PIN_SET) {
                if (HAL_GetTick() - t0 > 100u) {
                    (void)command(AFE_CMD_STOP);
                    printf("[afe] lead-off check: no DRDY within 100 ms\r\n");
                    return AFE_SPI_ERROR;
                }
            }
            if (transfer(tx, rx, sizeof(tx)) != HAL_OK) {
                (void)command(AFE_CMD_STOP);
                return AFE_SPI_ERROR;
            }
        }
        if (command(AFE_CMD_STOP) != HAL_OK ||
            read_registers(AFE_REG_LOFF_STATP, stat, 2) != HAL_OK) {
            return AFE_SPI_ERROR;
        }
        EXTI1->FPR1 = DRDY_LINE; /* this conversion was ours, not the ISR's */

        if (!afe_status_valid(&rx[1])) {
            printf("[afe] lead-off check: status %02x %02x %02x is not a status word\r\n",
                   rx[1], rx[2], rx[3]);
            continue;
        }
        afe_status_lead_off(&rx[1], &from_word_p, &from_word_n);
        printf("[afe] lead-off check: status word P %02x N %02x, LOFF_STAT P %02x N %02x\r\n",
               from_word_p, from_word_n, stat[0], stat[1]);
        if (from_word_p == stat[0] && from_word_n == stat[1]) {
            return AFE_OK;
        }
    }
    printf("[afe] lead-off: the status word and LOFF_STAT disagree - layout not trusted\r\n");
    return AFE_SPI_ERROR;
}

int afe_apply(const struct afe_regs *regs)
{
    const uint8_t config[3] = {regs->config1, regs->config2, regs->config3};
    uint8_t chset[AFE_CHANNELS];
    int st;

    afe_stop();
    memset(chset, regs->chset, sizeof(chset));

    /*
     * The per-channel selections before the bits that switch the amplifier and
     * the comparators on - the order ti-ads1299.c's set_bias() keeps, so that
     * nothing is ever powered with a stale derivation. Every block is read
     * back: a register the converter did not take is a prescription the
     * device would acquire under while the record claims another, and
     * answering "accepted" then is the silent drop MedicalDevice.h calls a
     * defect.
     */
    st = write_and_verify("BIAS/LOFF_SENS", AFE_REG_BIAS_SENSP, regs->sens, 4, 0xFFu);
    if (st == AFE_OK) {
        st = write_and_verify("LOFF", AFE_REG_LOFF, &regs->loff, 1, 0xFFu);
    }
    if (st == AFE_OK) {
        st = write_and_verify("CONFIG4", AFE_REG_CONFIG4, &regs->config4, 1, 0xFFu);
    }
    if (st == AFE_OK) {
        st = write_and_verify("CONFIG1", AFE_REG_CONFIG1, config, 2, 0xFFu);
    }
    if (st == AFE_OK) {
        /* Bit 0 is BIAS_STAT, the bias electrode's lead-off status: read-only,
         * and 1 on a bench with nothing attached, so it is not compared. */
        st = write_and_verify("CONFIG3", AFE_REG_CONFIG3, &config[2], 1, 0xFEu);
    }
    if (st == AFE_OK) {
        st = write_and_verify("CHnSET", AFE_REG_CH1SET, chset, AFE_CHANNELS, 0xFFu);
    }
    if (st != AFE_OK) {
        return st;
    }

    /* The internal reference buffer was just enabled (CONFIG3 bit 7); give it
     * time to settle before the first conversion is trusted. */
    HAL_Delay(150);

    if (regs->config4 != 0u && (st = check_status_layout()) != AFE_OK) {
        return st;
    }

    printf("[afe] configured: CONFIG1 %02x CONFIG2 %02x CONFIG3 %02x CHnSET %02x "
           "SENS %02x %02x %02x %02x CONFIG4 %02x\r\n",
           config[0], config[1], config[2], regs->chset, regs->sens[0], regs->sens[1],
           regs->sens[2], regs->sens[3], regs->config4);
    return AFE_OK;
}

/* ------------------------------------------------------------- conversion */

int afe_start(void)
{
    q_tail = q_head; /* nothing from an earlier run */

    if (command(AFE_CMD_RDATAC) != HAL_OK) {
        return AFE_SPI_ERROR;
    }
    /* EXTI1, written directly - never a __HAL_GPIO_EXTI_* macro, which in this
     * HAL writes EXTI2, the M0+ domain's controller (see the IRQ handler). */
    EXTI1->FPR1 = DRDY_LINE;
    HAL_NVIC_SetPriority(EXTI1_8_IRQn, DRDY_IRQ_PRIORITY, 0);
    HAL_NVIC_EnableIRQ(EXTI1_8_IRQn);

    /* START is pulled down on the board, so conversions start by command. */
    return command(AFE_CMD_START) == HAL_OK ? AFE_OK : AFE_SPI_ERROR;
}

void afe_stop(void)
{
    HAL_NVIC_DisableIRQ(EXTI1_8_IRQn);
    (void)command(AFE_CMD_STOP);
    (void)command(AFE_CMD_SDATAC);
    EXTI1->FPR1 = DRDY_LINE;
}

void afe_power_down(void)
{
    afe_stop();
    HAL_GPIO_WritePin(PWDN_PORT, PWDN_PIN, GPIO_PIN_RESET);
}

/*
 * DRDY. The pending bit is cleared on EXTI1 directly: the HAL's
 * HAL_GPIO_EXTI_IRQHandler() and its __HAL_GPIO_EXTI_* macros operate on
 * EXTI2, so using them here would never clear this line and the core would
 * re-enter this handler for ever.
 */
void EXTI1_8_IRQHandler(void)
{
    static const uint8_t zeros[AFE_FRAME_BYTES];
    const uint64_t at = med_now_us(); /* first: this is the sample's time */
    const uint32_t head = q_head;
    struct afe_sample *slot;
    uint64_t done;

    EXTI1->FPR1 = DRDY_LINE;
    ++counters.drdy;

    if (head - q_tail >= QUEUE) {
        /* The loop is 128 ms behind. Read anyway, so the converter's output
         * is consumed and DRDY keeps its rhythm, and count the loss. */
        uint8_t discard[AFE_FRAME_BYTES];
        (void)transfer(zeros, discard, AFE_FRAME_BYTES);
        ++counters.overruns;
        return;
    }

    slot = &queue[head % QUEUE];
    slot->at_us = at;
    if (transfer(zeros, slot->raw, AFE_FRAME_BYTES) != HAL_OK) {
        ++counters.spi_errors;
        return;
    }
    done = med_now_us();
    counters.spi_us_last = (uint32_t)(done - at);
    q_head = head + 1u;
}

int afe_take(struct afe_sample *out)
{
    if (q_tail == q_head) {
        return 0;
    }
    *out = queue[q_tail % QUEUE];
    ++q_tail;
    return 1;
}

void afe_counters(struct afe_counters *out)
{
    out->drdy = counters.drdy;
    out->overruns = counters.overruns;
    out->spi_errors = counters.spi_errors;
    out->spi_us_last = counters.spi_us_last;
}
