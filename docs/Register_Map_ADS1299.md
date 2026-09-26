# ADS1299 — Register Map

> [!note] Formatação
> O conteúdo técnico foi mantido conforme o arquivo enviado; esta versão reorganiza apenas a estrutura Markdown para renderização consistente no Obsidian.

## Table 11 — Register Assignments

| ADDRESS | REGISTER | DEFAULT SETTING | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Read Only ID Registers** |  |  |  |  |  |  |  |  |  |  |
| 00h | ID | xxh | REV_ID[2:0] | — | — | 1 | DEV_ID[1:0] | — | NU_CH[1:0] | — |
| **Global Settings Across Channels** |  |  |  |  |  |  |  |  |  |  |
| 01h | CONFIG1 | 96h | 1 | DAISY_EN | CLK_EN | 1 | 0 | DR[2:0] | — | — |
| 02h | CONFIG2 | C0h | 1 | 1 | 0 | INT_CAL | 0 | CAL_AMP0 | CAL_FREQ[1:0] | — |
| 03h | CONFIG3 | 60h | PD_REFBUF | 1 | 1 | BIAS_MEAS | BIASREF_INT | PD_BIAS | BIAS_LOFF_SENS | BIAS_STAT |
| 04h | LOFF | 00h | COMP_TH[2:0] | — | — | 0 | ILEAD_OFF[1:0] | — | FLEAD_OFF[1:0] | — |
| **Channel-Specific Settings** |  |  |  |  |  |  |  |  |  |  |
| 05h | CH1SET | 61h | PD1 | GAIN1[2:0] | — | — | SRB2 | MUX1[2:0] | — | — |
| 06h | CH2SET | 61h | PD2 | GAIN2[2:0] | — | — | SRB2 | MUX2[2:0] | — | — |
| 07h | CH3SET | 61h | PD3 | GAIN3[2:0] | — | — | SRB2 | MUX3[2:0] | — | — |
| 08h | CH4SET | 61h | PD4 | GAIN4[2:0] | — | — | SRB2 | MUX4[2:0] | — | — |
| 09h | CH5SET(1) | 61h | PD5 | GAIN5[2:0] | — | — | SRB2 | MUX5[2:0] | — | — |
| 0Ah | CH6SET(1) | 61h | PD6 | GAIN6[2:0] | — | — | SRB2 | MUX6[2:0] | — | — |
| 0Bh | CH7SET(2) | 61h | PD7 | GAIN7[2:0] | — | — | SRB2 | MUX7[2:0] | — | — |
| 0Ch | CH8SET(2) | 61h | PD8 | GAIN8[2:0] | — | — | SRB2 | MUX8[2:0] | — | — |
| 0Dh | BIAS_SENSP | 00h | BIASP8(2) | BIASP7(2) | BIASP6(1) | BIASP5(1) | BIASP4 | BIASP3 | BIASP2 | BIASP1 |
| 0Eh | BIAS_SENSN | 00h | BIASN8(2) | BIASN7(2) | BIASN6(1) | BIASN5(1) | BIASN4 | BIASN3 | BIASN2 | BIASN1 |
| 0Fh | LOFF_SENSP | 00h | LOFFP8(2) | LOFFP7(2) | LOFFP6(1) | LOFFP5(1) | LOFFP4 | LOFFP3 | LOFFP2 | LOFFP1 |
| 10h | LOFF_SENSN | 00h | LOFFM8(2) | LOFFM7(2) | LOFFM6(1) | LOFFM5(1) | LOFFM4 | LOFFM3 | LOFFM2 | LOFFM1 |
| 11h | LOFF_FLIP | 00h | LOFF_FLIP8(2) | LOFF_FLIP7(2) | LOFF_FLIP6(1) | LOFF_FLIP5(1) | LOFF_FLIP4 | LOFF_FLIP3 | LOFF_FLIP2 | LOFF_FLIP1 |
| **Lead-Off Status Registers (Read-Only Registers)** |  |  |  |  |  |  |  |  |  |  |
| 12h | LOFF_STATP | 00h | IN8P_OFF | IN7P_OFF | IN6P_OFF | IN5P_OFF | IN4P_OFF | IN3P_OFF | IN2P_OFF | IN1P_OFF |
| 13h | LOFF_STATN | 00h | IN8M_OFF | IN7M_OFF | IN6M_OFF | IN5M_OFF | IN4M_OFF | IN3M_OFF | IN2M_OFF | IN1M_OFF |
| **GPIO and OTHER Registers** |  |  |  |  |  |  |  |  |  |  |
| 14h | GPIO | 0Fh | GPIOD[4:1] | — | — | — | GPIOC[4:1] | — | — | — |
| 15h | MISC1 | 00h | 0 | 0 | SRB1 | 0 | 0 | 0 | 0 | 0 |
| 16h | MISC2 | 00h | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 17h | CONFIG4 | 00h | 0 | 0 | 0 | 0 | SINGLE_SHOT | 0 | PD_LOFF_COMP | 0 |
*(1) Register or bit only available in the ADS1299-6 and ADS1299. Register bits set to 0h or 00h in the ADS1299-4.*

*(2) Register or bit only available in the ADS1299. Register bits set to 0h or 00h in the ADS1299-4 and ADS1299-6.*

*Nota: O traço em travessão (`—`) indica a extensão de campos que abrangem múltiplos bits a partir do bit mais significativo à esquerda.*

---

## 9.6.1.1 ID: ID Control Register (address = 00h) (reset = xxh)

### Figure 50. ID Control Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| REV_ID[2:0] | — | — | 1 | DEV_ID[1:0] | — | NU_CH[1:0] | — |
| R-xh | — | — | R-1h | R-3h | — | R-xh | — |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 12. ID Control Register Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:5 | REV_ID[2:0] | R | xh | **Reserved.**<br><br>These bits indicate the revision of the device and are subject to change without notice. |
| 4 | Reserved | R | 1h | **Reserved.**<br><br>Always read 1. |
| 3:2 | DEV_ID[1:0] | R | 3h | **Device Identification.**<br><br>These bits indicates the device.<br><br>11 : ADS1299-x |
| 1:0 | NU_CH[1:0] | R | xh | **Number of Channels.**<br><br>These bits indicates number of channels.<br><br>00 : 4-channel ADS1299-4<br><br>01 : 6-channel ADS1299-6<br><br>10 : 8-channel ADS1299 |

---

## 9.6.1.2 CONFIG1: Configuration Register 1 (address = 01h) (reset = 96h)

### Figure 51. CONFIG1: Configuration Register 1

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | DAISY_EN | CLK_EN | 1 | 0 | DR[2:0] | — | — |
| R/W-1h | R/W-0h | R/W-0h | R/W-1h | R/W-0h | R/W-6h | — | — |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 13. Configuration Register 1 Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | Reserved | R/W | 1h | **Reserved**<br><br>Always write 1h |
| 6 | DAISY_EN | R/W | 0h | **Daisy-chain or multiple readback mode**<br><br>This bit determines which mode is enabled.<br><br>0 : Daisy-chain mode<br><br>1 : Multiple readback mode |
| 5 | CLK_EN | R/W | 0h | **CLK connection(1)**<br><br>This bit determines if the internal oscillator signal is connected to the CLK pin when the CLKSEL pin = 1.<br><br>0 : Oscillator clock output disabled<br><br>1 : Oscillator clock output enabled |
| 4:3 | Reserved | R/W | 2h | **Reserved**<br><br>Always write 2h |
| 2:0 | DR[2:0] | R/W | 6h | **Output data rate**<br><br>These bits determine the output data rate of the device. fMOD = fCLK / 2.<br><br>000 : fMOD / 64 (16 kSPS)<br><br>001 : fMOD / 128 (8 kSPS)<br><br>010 : fMOD / 256 (4 kSPS)<br><br>011 : fMOD / 512 (2 kSPS)<br><br>100 : fMOD / 1024 (1 kSPS)<br><br>101 : fMOD / 2048 (500 SPS)<br><br>110 : fMOD / 4096 (250 SPS)<br><br>111 : Reserved (do not use) |

*(1) Additional power is consumed when driving external devices.*

---

## 9.6.1.3 CONFIG2: Configuration Register 2 (address = 02h) (reset = C0h)

### Figure 52. CONFIG2: Configuration Register 2

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1 | 0 | INT_CAL | 0 | CAL_AMP | CAL_FREQ[1:0] | — |
| R/W-1h | R/W-1h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | — |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 14. Configuration Register 2 Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:5 | Reserved | R/W | 6h | **Reserved**<br><br>Always write 6h |
| 4 | INT_CAL | R/W | 0h | **TEST source**<br><br>This bit determines the source for the test signal.<br><br>0 : Test signals are driven externally<br><br>1 : Test signals are generated internally |
| 3 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |
| 2 | CAL_AMP | R/W | 0h | **Test signal amplitude**<br><br>These bits determine the calibration signal amplitude.<br><br>0 : 1 × –(VREFP – VREFN) / 2400<br><br>1 : 2 × –(VREFP – VREFN) / 2400 |
| 1:0 | CAL_FREQ[1:0] | R/W | 0h | **Test signal frequency**<br><br>These bits determine the calibration signal frequency.<br><br>00 : Pulsed at fCLK / 221<br><br>01 : Pulsed at fCLK / 220<br><br>10 : Do not use<br><br>11 : At dc |

---

## 9.6.1.4 CONFIG3: Configuration Register 3 (address = 03h) (reset = 60h)

### Figure 53. CONFIG3: Configuration Register 3

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PD_REFBUF | 1 | 1 | BIAS_MEAS | BIASREF_INT | PD_BIAS | BIAS_LOFF_SENS | BIAS_STAT |
| R/W-0h | R/W-1h | R/W-1h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 15. Configuration Register 3 Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | PD_REFBUF | R/W | 0h | **Power-down reference buffer**<br><br>This bit determines the power-down reference buffer state.<br><br>0 : Power-down internal reference buffer<br><br>1 : Enable internal reference buffer |
| 6:5 | Reserved | R/W | 3h | **Reserved**<br><br>Always write 3h. |
| 4 | BIAS_MEAS | R/W | 0h | **BIAS measurement**<br><br>This bit enables BIAS measurement. The BIAS signal may be measured with any channel.<br><br>0 : Open<br><br>1 : BIAS_IN signal is routed to the channel that has the MUX_Setting 010 (VREF) |
| 3 | BIASREF_INT | R/W | 0h | **BIASREF signal**<br><br>This bit determines the BIASREF signal source.<br><br>0 : BIASREF signal fed externally<br><br>1 : BIASREF signal (AVDD + AVSS) / 2 generated internally |
| 2 | PD_BIAS | R/W | 0h | **BIAS buffer power**<br><br>This bit determines the BIAS buffer power state.<br><br>0 : BIAS buffer is powered down<br><br>1 : BIAS buffer is enabled |
| 1 | BIAS_LOFF_SENS | R/W | 0h | **BIAS sense function**<br><br>This bit enables the BIAS sense function.<br><br>0 : BIAS sense is disabled<br><br>1 : BIAS sense is enabled |
| 0 | BIAS_STAT | R | 0h | **BIAS lead-off status**<br><br>This bit determines the BIAS status.<br><br>0 : BIAS is connected<br><br>1 : BIAS is not connected |

---

## 9.6.1.5 LOFF: Lead-Off Control Register (address = 04h) (reset = 00h)

### Figure 54. LOFF: Lead-Off Control Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| COMP_TH2[2:0] | — | — | 0 | ILEAD_OFF[1:0] | — | FLEAD_OFF[1:0] | — |
| R/W-0h | — | — | R/W-0h | R/W-0h | — | R/W-0h | — |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 16. Lead-Off Control Register Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:5 | COMP_TH[2:0] | R/W | 0h | **Lead-off comparator threshold**<br><br>Comparator positive side<br><br>000 : 95%<br><br>001 : 92.5%<br><br>010 : 90%<br><br>011 : 87.5%<br><br>100 : 85%<br><br>101 : 80%<br><br>110 : 75%<br><br>111 : 70%<br><br>Comparator negative side<br><br>000 : 5%<br><br>001 : 7.5%<br><br>010 : 10%<br><br>011 : 12.5%<br><br>100 : 15%<br><br>101 : 20%<br><br>110 : 25%<br><br>111 : 30% |
| 4 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h. |
| 3:2 | ILEAD_OFF[1:0] | R/W | 0h | **Lead-off current magnitude**<br><br>These bits determine the magnitude of current for the current lead-off mode.<br><br>00 : 6 nA<br><br>01 : 24 nA<br><br>10 : 6 µA<br><br>11 : 24 µA |
| 1:0 | FLEAD_OFF[1:0] | R/W | 0h | **Lead-off frequency**<br><br>These bits determine the frequency of lead-off detect for each channel.<br><br>00 : DC lead-off detection<br><br>01 : AC lead-off detection at 7.8 Hz (fCLK / 218)<br><br>10 : AC lead-off detection at 31.2 Hz (fCLK / 216)<br><br>11 : AC lead-off detection at fDR / 4 |
## 9.6.1.6 CHnSET: Individual Channel Settings (n = 1 to 8) (address = 05h to 0Ch) (reset = 61h)

The CH[1:8]SET control register configures the power mode, PGA gain, and multiplexer settings channels. See the *Input Multiplexer* section for details. CH[2:8]SET are similar to CH1SET, corresponding to the respective channels.

### Figure 55. CHnSET: Individual Channel Settings Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PD*n* | GAIN*n*[2:0] | — | — | SRB2 | MUX*n*[2:0] | — | — |
| R/W-0h | R/W-6h | — | — | R/W-0h | R/W-0h | — | — |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 17. Individual Channel Settings (n = 1 to 8) Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | PD*n* | R/W | 0h | **Power-down**<br><br>This bit determines the channel power mode for the corresponding channel.<br><br>0 : Normal operation<br><br>1 : Channel power-down.<br><br>When powering down a channel, TI recommends that the channel be set to input short by setting the appropriate MUX*n*[2:0] = 001 of the CH*n*SET register. |
| 6:4 | GAIN*n*[2:0] | R/W | 6h | **PGA gain**<br><br>These bits determine the PGA gain setting.<br><br>000 : 1<br><br>001 : 2<br><br>010 : 4<br><br>011 : 6<br><br>100 : 8<br><br>101 : 12<br><br>110 : 24<br><br>111 : Do not use |
| 3 | SRB2 | R/W | 0h | **SRB2 connection**<br><br>This bit determines the SRB2 connection for the corresponding channel.<br><br>0 : Open<br><br>1 : Closed |
| 2:0 | MUX*n*[2:0] | R/W | 1h | **Channel input**<br><br>These bits determine the channel input selection.<br><br>000 : Normal electrode input<br><br>001 : Input shorted (for offset or noise measurements)<br><br>010 : Used in conjunction with BIAS_MEAS bit for BIAS measurements.<br><br>011 : MVDD for supply measurement<br><br>100 : Temperature sensor<br><br>101 : Test signal<br><br>110 : BIAS_DRP (positive electrode is the driver)<br><br>111 : BIAS_DRN (negative electrode is the driver) |

---

## 9.6.1.7 BIAS_SENSP: Bias Drive Positive Derivation Register (address = 0Dh) (reset = 00h)

This register controls the selection of the positive signals from each channel for bias voltage (BIAS) derivation. See the *Bias Drive (DC Bias Circuit)* section for details.

Registers bits[5:4] are not available for the ADS1299-4. Register bits[7:6] are not available for the ADS1299-4, or ADS1299-6. Set unavailable bits for the associated device to 0 when writing to the register.

### Figure 56. BIAS_SENSP: BIAS Positive Signal Derivation Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BIASP8 | BIASP7 | BIASP6 | BIASP5 | BIASP4 | BIASP3 | BIASP2 | BIASP1 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 18. BIAS Positive Signal Derivation Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | BIASP8 | R/W | 0h | **IN8P to BIAS**<br><br>Route channel 8 positive signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 6 | BIASP7 | R/W | 0h | **IN7P to BIAS**<br><br>Route channel 7 positive signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 5 | BIASP6 | R/W | 0h | **IN6P to BIAS**<br><br>Route channel 6 positive signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 4 | BIASP5 | R/W | 0h | **IN5P to BIAS**<br><br>Route channel 5 positive signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 3 | BIASP4 | R/W | 0h | **IN4P to BIAS**<br><br>Route channel 4 positive signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 2 | BIASP3 | R/W | 0h | **IN3P to BIAS**<br><br>Route channel 3 positive signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 1 | BIASP2 | R/W | 0h | **IN2P to BIAS**<br><br>Route channel 2 positive signal into BIAS channel<br><br>0 : Disabled<br><br>1 : Enabled |
| 0 | BIASP1 | R/W | 0h | **IN1P to BIAS**<br><br>Route channel 1 positive signal into BIAS channel<br><br>0 : Disabled<br><br>1 : Enabled |

---

## 9.6.1.8 BIAS_SENSN: Bias Drive Negative Derivation Register (address = 0Eh) (reset = 00h)

This register controls the selection of the negative signals from each channel for bias voltage (BIAS) derivation. See the *Bias Drive (DC Bias Circuit)* section for details.

Registers bits[5:4] are not available for the ADS1299-4. Register bits[7:6] are not available for the ADS1299-4, or ADS1299-6. Set unavailable bits for the associated device to 0 when writing to the register.

### Figure 57. BIAS_SENSN: BIAS Negative Signal Derivation Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BIASN8 | BIASN7 | BIASN6 | BIASN5 | BIASN4 | BIASN3 | BIASN2 | BIASN1 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 19. BIAS Negative Signal Derivation Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | BIASN8 | R/W | 0h | **IN8N to BIAS**<br><br>Route channel 8 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 6 | BIASN7 | R/W | 0h | **IN7N to BIAS**<br><br>Route channel 7 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 5 | BIASN6 | R/W | 0h | **IN6N to BIAS**<br><br>Route channel 6 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 4 | BIASN5 | R/W | 0h | **IN5N to BIAS**<br><br>Route channel 5 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 3 | BIASN4 | R/W | 0h | **IN4N to BIAS**<br><br>Route channel 4 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 2 | BIASN3 | R/W | 0h | **IN3N to BIAS**<br><br>Route channel 3 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 1 | BIASN2 | R/W | 0h | **IN2N to BIAS**<br><br>Route channel 2 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |
| 0 | BIASN1 | R/W | 0h | **IN1N to BIAS**<br><br>Route channel 1 negative signal into BIAS derivation<br><br>0 : Disabled<br><br>1 : Enabled |

---

## 9.6.1.9 LOFF_SENSP: Positive Signal Lead-Off Detection Register (address = 0Fh) (reset = 00h)

This register selects the positive side from each channel for lead-off detection. See the *Lead-Off Detection* section for details. The LOFF_STATP register bits are only valid if the corresponding LOFF_SENSP bits are set to 1.

Registers bits[5:4] are not available for the ADS1299-4. Register bits[7:6] are not available for the ADS1299-4, or ADS1299-6. Set unavailable bits for the associated device to 0 when writing to the register.

### Figure 58. LOFF_SENSP: Positive Signal Lead-Off Detection Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LOFFP8 | LOFFP7 | LOFFP6 | LOFFP5 | LOFFP4 | LOFFP3 | LOFFP2 | LOFFP1 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 20. Positive Signal Lead-Off Detection Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | LOFFP8 | R/W | 0h | **IN8P lead off**<br><br>Enable lead-off detection on IN8P<br><br>0 : Disabled<br><br>1 : Enabled |
| 6 | LOFFP7 | R/W | 0h | **IN7P lead off**<br><br>Enable lead-off detection on IN7P<br><br>0 : Disabled<br><br>1 : Enabled |
| 5 | LOFFP6 | R/W | 0h | **IN6P lead off**<br><br>Enable lead-off detection on IN6P<br><br>0 : Disabled<br><br>1 : Enabled |
| 4 | LOFFP5 | R/W | 0h | **IN5P lead off**<br><br>Enable lead-off detection on IN5P<br><br>0 : Disabled<br><br>1 : Enabled |
| 3 | LOFFP4 | R/W | 0h | **IN4P lead off**<br><br>Enable lead-off detection on IN4P<br><br>0 : Disabled<br><br>1 : Enabled |
| 2 | LOFFP3 | R/W | 0h | **IN3P lead off**<br><br>Enable lead-off detection on IN3P<br><br>0 : Disabled<br><br>1 : Enabled |
| 1 | LOFFP2 | R/W | 0h | **IN2P lead off**<br><br>Enable lead-off detection on IN2P<br><br>0 : Disabled<br><br>1 : Enabled |
| 0 | LOFFP1 | R/W | 0h | **IN1P lead off**<br><br>Enable lead-off detection on IN1P<br><br>0 : Disabled<br><br>1 : Enabled |

---

## 9.6.1.10 LOFF_SENSN: Negative Signal Lead-Off Detection Register (address = 10h) (reset = 00h)

This register selects the negative side from each channel for lead-off detection. See the *Lead-Off Detection* section for details. The LOFF_STATN register bits are only valid if the corresponding LOFF_SENSN bits are set to 1.

Registers bits[5:4] are not available for the ADS1299-4. Register bits[7:6] are not available for the ADS1299-4, or ADS1299-6. Set unavailable bits for the associated device to 0 when writing to the register.

### Figure 59. LOFF_SENSN: Negative Signal Lead-Off Detection Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LOFFM8 | LOFFM7 | LOFFM6 | LOFFM5 | LOFFM4 | LOFFM3 | LOFFM2 | LOFFM1 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 21. Negative Signal Lead-Off Detection Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | LOFFM8 | R/W | 0h | **IN8N lead off**<br><br>Enable lead-off detection on IN8N<br><br>0 : Disabled<br><br>1 : Enabled |
| 6 | LOFFM7 | R/W | 0h | **IN7N lead off**<br><br>Enable lead-off detection on IN7N<br><br>0 : Disabled<br><br>1 : Enabled |
| 5 | LOFFM6 | R/W | 0h | **IN6N lead off**<br><br>Enable lead-off detection on IN6N<br><br>0 : Disabled<br><br>1 : Enabled |
| 4 | LOFFM5 | R/W | 0h | **IN5N lead off**<br><br>Enable lead-off detection on IN5N<br><br>0 : Disabled<br><br>1 : Enabled |
| 3 | LOFFM4 | R/W | 0h | **IN4N lead off**<br><br>Enable lead-off detectionn on IN4N<br><br>0 : Disabled<br><br>1 : Enabled |
| 2 | LOFFM3 | R/W | 0h | **IN3N lead off**<br><br>Enable lead-off detectionion on IN3N<br><br>0 : Disabled<br><br>1 : Enabled |
| 1 | LOFFM2 | R/W | 0h | **IN2N lead off**<br><br>Enable lead-off detectionction on IN2N<br><br>0 : Disabled<br><br>1 : Enabled |
| 0 | LOFFM1 | R/W | 0h | **IN1N lead off**<br><br>Enable lead-off detectionction on IN1N<br><br>0 : Disabled<br><br>1 : Enabled |
## 9.6.1.11 LOFF_FLIP: Lead-Off Flip Register (address = 11h) (reset = 00h)

This register controls the direction of the current used for lead-off derivation. See the *Lead-Off Detection* section for details.

### Figure 60. LOFF_FLIP: Lead-Off Flip Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LOFF_FLIP8 | LOFF_FLIP7 | LOFF_FLIP6 | LOFF_FLIP5 | LOFF_FLIP4 | LOFF_FLIP3 | LOFF_FLIP2 | LOFF_FLIP1 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 22. Lead-Off Flip Register Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | LOFF_FLIP8 | R/W | 0h | **Channel 8 LOFF polarity flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 8 for lead-off detection.<br><br>0 : No flip = IN8P is pulled to AVDD and IN8N pulled to AVSS<br><br>1 : Flipped = IN8P is pulled to AVSS and IN8N pulled to AVDD |
| 6 | LOFF_FLIP7 | R/W | 0h | **Channel 7 LOFF polarity flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 7 for lead-off detection.<br><br>0 : No flip = IN7P is pulled to AVDD and IN7N pulled to AVSS<br><br>1 : Flipped = IN7P is pulled to AVSS and IN7N pulled to AVDD |
| 5 | LOFF_FLIP6 | R/W | 0h | **Channel 6 LOFF polarity flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 6 for lead-off detection.<br><br>0 : No flip = IN6P is pulled to AVDD and IN6N pulled to AVSS<br><br>1 : Flipped = IN6P is pulled to AVSS and IN6N pulled to AVDD |
| 4 | LOFF_FLIP5 | R/W | 0h | **Channel 5 LOFF polarity flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 5 for lead-off detection.<br><br>0 : No flip = IN5P is pulled to AVDD and IN5N pulled to AVSS<br><br>1 : Flipped = IN5P is pulled to AVSS and IN5N pulled to AVDD |
| 3 | LOFF_FLIP4 | R/W | 0h | **Channel 4 LOFF polarity flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 4 for lead-off detection.<br><br>0 : No flip = IN4P is pulled to AVDD and IN4N pulled to AVSS<br><br>1 : Flipped = IN4P is pulled to AVSS and IN4N pulled to AVDD |
| 2 | LOFF_FLIP3 | R/W | 0h | **Channel 3 LOFF polarity flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 3 for lead-off detection.<br><br>0 : No flip = IN3P is pulled to AVDD and IN3N pulled to AVSS<br><br>1 : Flipped = IN3P is pulled to AVSS and IN3N pulled to AVDD |
| 1 | LOFF_FLIP2 | R/W | 0h | **Channel 2 LOFF Polarity Flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 2 for lead-off detection.<br><br>0 : No flip = IN2P is pulled to AVDD and IN2N pulled to AVSS<br><br>1 : Flipped = IN2P is pulled to AVSS and IN2N pulled to AVDD |
| 0 | LOFF_FLIP1 | R/W | 0h | **Channel 1 LOFF Polarity Flip**<br><br>Flip the pull-up or pull-down polarity of the current source on channel 1 for lead-off detection.<br><br>0 : No flip = IN1P is pulled to AVDD and IN1N pulled to AVSS<br><br>1 : Flipped = IN1P is pulled to AVSS and IN1N pulled to AVDD |

---

## 9.6.1.12 LOFF_STATP: Lead-Off Positive Signal Status Register (address = 12h) (reset = 00h)

This register stores the status of whether the positive electrode on each channel is on or off. See the *Lead-Off Detection* section for details. Ignore the LOFF_STATP values if the corresponding LOFF_SENSP bits are not set to 1.

When the LOFF_SENSEP bits are 0, the LOFF_STATP bits should be ignored.

### Figure 61. LOFF_STATP: Lead-Off Positive Signal Status Register (Read-Only)

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| IN8P_OFF | IN7P_OFF | IN6P_OFF | IN5P_OFF | IN4P_OFF | IN3P_OFF | IN2P_OFF | IN1P_OFF |
| R-0h | R-0h | R-0h | R-0h | R-0h | R-0h | R-0h | R-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 23. Lead-Off Positive Signal Status Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | IN8P_OFF | R | 0h | **Channel 8 positive channel lead-off status**<br><br>Status of whether IN8P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 6 | IN7P_OFF | R | 0h | **Channel 7 positive channel lead-off status**<br><br>Status of whether IN7P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 5 | IN6P_OFF | R | 0h | **Channel 6 positive channel lead-off status**<br><br>Status of whether IN6P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 4 | IN5P_OFF | R | 0h | **Channel 5 positive channel lead-off status**<br><br>Status of whether IN5P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 3 | IN4P_OFF | R | 0h | **Channel 4 positive channel lead-off status**<br><br>Status of whether IN4P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 2 | IN3P_OFF | R | 0h | **Channel 3 positive channel lead-off status**<br><br>Status of whether IN3P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 1 | IN2P_OFF | R | 0h | **Channel 2 positive channel lead-off status**<br><br>Status of whether IN2P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 0 | IN1P_OFF | R | 0h | **Channel 1 positive channel lead-off status**<br><br>Status of whether IN1P electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |

---

## 9.6.1.13 LOFF_STATN: Lead-Off Negative Signal Status Register (address = 13h) (reset = 00h)

This register stores the status of whether the negative electrode on each channel is on or off. See the *Lead-Off Detection* section for details. Ignore the LOFF_STATN values if the corresponding LOFF_SENSN bits are not set to 1.

When the LOFF_SENSEN bits are 0, the LOFF_STATP bits should be ignored.

### Figure 62. LOFF_STATN: Lead-Off Negative Signal Status Register (Read-Only)

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| IN8M_OFF | IN7M_OFF | IN6M_OFF | IN5M_OFF | IN4M_OFF | IN3M_OFF | IN2M_OFF | IN1M_OFF |
| R-0h | R-0h | R-0h | R-0h | R-0h | R-0h | R-0h | R-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 24. Lead-Off Negative Signal Status Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7 | IN8N_OFF | R | 0h | **Channel 8 negative channel lead-off status**<br><br>Status of whether IN8N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 6 | IN7N_OFF | R | 0h | **Channel 7 negative channel lead-off status**<br><br>Status of whether IN7N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 5 | IN6N_OFF | R | 0h | **Channel 6 negative channel lead-off status**<br><br>Status of whether IN6N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 4 | IN5N_OFF | R | 0h | **Channel 5 negative channel lead-off status**<br><br>Status of whether IN5N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 3 | IN4N_OFF | R | 0h | **Channel 4 negative channel lead-off status**<br><br>Status of whether IN4N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 2 | IN3N_OFF | R | 0h | **Channel 3 negative channel lead-off status**<br><br>Status of whether IN3N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 1 | IN2N_OFF | R | 0h | **Channel 2 negative channel lead-off status**<br><br>Status of whether IN2N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |
| 0 | IN1N_OFF | R | 0h | **Channel 1 negative channel lead-off status**<br><br>Status of whether IN1N electrode is on or off<br><br>0 : Electrode is on<br><br>1 : Electrode is off |

---

## 9.6.1.14 GPIO: General-Purpose I/O Register (address = 14h) (reset = 0Fh)

The general-purpose I/O register controls the action of the three GPIO pins. When RESP_CTRL[1:0] is in mode 01 and 11, the GPIO2, GPIO3, and GPIO4 pins are not available for use.

### Figure 63. GPIO: General-Purpose I/O Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GPIOD[4:1] | — | — | — | GPIOC[4:1] | — | — | — |
| R/W-0h | — | — | — | R/W-Fh | — | — | — |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 25. General-Purpose I/O Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:4 | GPIOD[4:1] | R/W | 0h | **GPIO data**<br><br>These bits are used to read and write data to the GPIO ports. When reading the register, the data returned correspond to the state of the GPIO external pins, whether they are programmed as inputs or as outputs. As outputs, a write to the GPIOD sets the output value. As inputs, a write to the GPIOD has no effect. GPIO is not available in certain respiration modes. |
| 3:0 | GPIOC[4:1] | R/W | Fh | **GPIO control (corresponding GPIOD)**<br><br>These bits determine if the corresponding GPIOD pin is an input or output.<br><br>0 : Output<br><br>1 : Input |

---

## 9.6.1.15 MISC1: Miscellaneous 1 Register (address = 15h) (reset = 00h)

This register provides the control to route the SRB1 pin to all inverting inputs of the four, six, or eight channels (ADS1299-4, ADS1299-6, or ADS1299).

### Figure 64. MISC1: Miscellaneous 1 Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 0 | SRB1 | 0 | 0 | 0 | 0 | 0 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 26. Miscellaneous 1 Register Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:6 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |
| 5 | SRB1 | R/W | 0h | **Stimulus, reference, and bias 1**<br><br>This bit connects the SRB1 to all 4, 6, or 8 channels inverting inputs<br><br>0 : Switches open<br><br>1 : Switches closed |
| 4:0 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |

---

## 9.6.1.16 MISC2: Miscellaneous 2 (address = 16h) (reset = 00h)

This register is reserved for future use.

### Figure 65. MISC1: Miscellaneous 1 Register

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 27. Miscellaneous 1 Register Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:0 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |
## 9.6.1.17 CONFIG4: Configuration Register 4 (address = 17h) (reset = 00h)

This register configures the conversion mode and enables the lead-off comparators.

### Figure 66. CONFIG4: Configuration Register 4

| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | SINGLE_SHOT | 0 | PD_LOFF_COMP | 0 |
| R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h | R/W-0h |
> **Legend:** `R/W` = Read/Write · `R` = Read only · `-n` = value after reset

### Table 28. Configuration Register 4 Field Descriptions

| Bit | Field | Type | Reset | Description |
| --- | --- | --- | --- | --- |
| 7:4 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |
| 3 | SINGLE_SHOT | R/W | 0h | **Single-shot conversion**<br><br>This bit sets the conversion mode.<br><br>0 : Continuous conversion mode<br><br>1 : Single-shot mode |
| 2 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |
| 1 | PD_LOFF_COMP | R/W | 0h | **Lead-off comparator power-down**<br><br>This bit powers down the lead-off comparators.<br><br>0 : Lead-off comparators disabled<br><br>1 : Lead-off comparators enabled |
| 0 | Reserved | R/W | 0h | **Reserved**<br><br>Always write 0h |