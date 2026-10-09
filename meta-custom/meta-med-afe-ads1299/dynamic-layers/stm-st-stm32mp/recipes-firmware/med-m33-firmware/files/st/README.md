# STMicroelectronics OpenAMP glue

These files are copied from STM32CubeMP2 at the revision the recipe fetches
(`Projects/STM32MP257F-DK/Applications/OpenAMP/OpenAMP_TTY_echo/CM33/NonSecure`),
under ST's BSD-3-Clause licence, which each file's header states. They are the
board's IPCC mailbox, the resource table Linux reads to set up the vrings, the
libmetal shared-memory setup, and the trace log.

They are kept as close to ST's text as possible, so that a later Cube release
can be diffed against them. Every change is marked `MedPlatform:` in place:

| File | Change | Why |
|---|---|---|
| `stm32mp2xx_hal_conf.h` | `HAL_SPI_MODULE_ENABLED` | the converter on SPI6 |
| `openamp_conf.h` | virtual UART disabled | this firmware opens one raw rpmsg endpoint (`rpmsg-raw`) |
| `openamp_log.h` | trace buffer 1048 → 4096 bytes | Linux reads the trace up to the first NUL and the ring writes one after its write point, so a wrap hides everything older; start-up plus the converter probe overflowed 1048 |
| `openamp_log.c` | includes the HAL header instead of the DK's BSP header | no BSP in this firmware |
| `openamp_log.c` | the trace buffer moved from `.bss` to a `.resource_table.trace` section | with the OP-TEE loading the firmware, Linux maps `ipc-shmem-1` and not the M33's RAM, so a buffer in `.bss` was "Trace not available" |
| `mbox_ipcc.c` | mailbox flags `volatile`, and cleared **before** the vring is processed | an interrupt arriving during processing was erased by the clear that followed it |

Nothing else in this directory differs from ST's text.
