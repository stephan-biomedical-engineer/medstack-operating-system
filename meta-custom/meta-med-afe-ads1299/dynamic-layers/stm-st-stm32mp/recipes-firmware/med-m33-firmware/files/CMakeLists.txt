# MedPlatform Cortex-M33 producer firmware.
#
# Built against an STM32CubeMP2 tree that the recipe fetches at a pinned
# revision (CUBE_DIR), and against the AMP wire format from med-amp-abi
# (MED_AMP_ABI_INCLUDE_DIR). Modelled on ST's own CMake for the DK's M33
# examples, minus FreeRTOS, USB-PD, SCMI and the board support package, none
# of which this firmware uses: it is bare metal, one endpoint, one clock.
#
#   cmake -S . -B build -DCUBE_DIR=<cube> -DMED_AMP_ABI_INCLUDE_DIR=<dir>
#
# arm-none-eabi-gcc must be on PATH: libmetal's own platform file names the
# compiler by prefix, and that is the generation step below.

cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(med_m33_producer C ASM)

if(NOT CUBE_DIR OR NOT EXISTS "${CUBE_DIR}/Drivers/STM32MP2xx_HAL_Driver")
    message(FATAL_ERROR "CUBE_DIR must point at an STM32CubeMP2 tree")
endif()
if(NOT MED_AMP_ABI_INCLUDE_DIR OR NOT EXISTS "${MED_AMP_ABI_INCLUDE_DIR}/med_amp_abi.h")
    message(FATAL_ERROR "MED_AMP_ABI_INCLUDE_DIR must hold med_amp_abi.h (recipe med-amp-abi)")
endif()

set(HAL      ${CUBE_DIR}/Drivers/STM32MP2xx_HAL_Driver)
set(CMSIS    ${CUBE_DIR}/Drivers/CMSIS)
set(OPENAMP  ${CUBE_DIR}/Middlewares/Third_Party/OpenAMP)
set(LIBMETAL ${OPENAMP}/libmetal)

# libmetal's headers are generated, and its generic system sources are copied
# under new names, by its own CMake for ST's platform file. ST's projects run
# this as a pre-build step; it is a configure-time step here, so that a failure
# stops the build before a single object is compiled.
execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${LIBMETAL} -B ${CMAKE_BINARY_DIR}/libmetal
            -G "Unix Makefiles"
            -DCMAKE_TOOLCHAIN_FILE=${LIBMETAL}/cmake/platforms/stm32mp-generic-gcc.cmake
    RESULT_VARIABLE libmetal_result)
if(NOT libmetal_result EQUAL 0)
    message(FATAL_ERROR "libmetal generation failed (${libmetal_result})")
endif()

set(CPU_FLAGS -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard)

add_executable(med_m33_producer.elf
    # Ours.
    src/main.c
    src/med_producer.c
    src/afe.c
    src/afe_regs.c

    # ST's OpenAMP glue, copied with marked changes (st/README.md).
    st/openamp.c
    st/rsc_table.c
    st/mbox_ipcc.c
    st/openamp_log.c

    # HAL, CMSIS start-up and the M33 non-secure system file.
    ${HAL}/Src/stm32mp2xx_hal.c
    ${HAL}/Src/stm32mp2xx_hal_cortex.c
    ${HAL}/Src/stm32mp2xx_hal_ipcc.c
    ${HAL}/Src/stm32mp2xx_hal_gpio.c
    ${HAL}/Src/stm32mp2xx_hal_spi.c
    ${HAL}/Src/stm32mp2xx_hal_spi_ex.c
    ${HAL}/Src/stm32mp2xx_hal_rcc.c
    ${HAL}/Src/stm32mp2xx_hal_rcc_ex.c
    ${HAL}/Src/stm32mp2xx_hal_pwr.c
    ${HAL}/Src/stm32mp2xx_hal_pwr_ex.c
    ${CMSIS}/Device/ST/STM32MP2xx/Source/Templates/system_stm32mp2xx_m33_ns.c
    ${CMSIS}/Device/ST/STM32MP2xx/Source/Templates/gcc/startup_stm32mp257cxx_m33.s

    # The shutdown handshake with Linux (IPCC channel 3).
    ${CUBE_DIR}/Utilities/CoproSync/copro_sync.c

    # Asks the RIF whether a resource is this core's before touching it.
    ${CUBE_DIR}/Utilities/ResourcesManager/res_mgr.c

    # OpenAMP and libmetal, the bare-metal ("generic") system.
    ${OPENAMP}/open-amp/lib/remoteproc/remoteproc_virtio.c
    ${OPENAMP}/open-amp/lib/rpmsg/rpmsg.c
    ${OPENAMP}/open-amp/lib/rpmsg/rpmsg_virtio.c
    ${OPENAMP}/open-amp/lib/virtio/virtio.c
    ${OPENAMP}/open-amp/lib/virtio/virtqueue.c
    ${LIBMETAL}/lib/device.c
    ${LIBMETAL}/lib/init.c
    ${LIBMETAL}/lib/io.c
    ${LIBMETAL}/lib/log.c
    ${LIBMETAL}/lib/shmem.c
    ${LIBMETAL}/lib/system/generic/condition.c
    ${LIBMETAL}/lib/system/generic/generic_device.c
    ${LIBMETAL}/lib/system/generic/generic_init.c
    ${LIBMETAL}/lib/system/generic/generic_io.c
    ${LIBMETAL}/lib/system/generic/generic_shmem.c
    ${LIBMETAL}/lib/system/generic/time.c
    ${LIBMETAL}/lib/system/generic/template/sys.c

    # newlib's _write and friends; printf lands in the trace buffer.
    ${CUBE_DIR}/Utilities/Services/GenericServices/syscalls.c
)

target_include_directories(med_m33_producer.elf PRIVATE
    src
    ${MED_AMP_ABI_INCLUDE_DIR}
)
# Everything that is not ours is a SYSTEM include, so that the warnings our own
# sources are held to are not raised against headers we do not maintain.
target_include_directories(med_m33_producer.elf SYSTEM PRIVATE
    st
    ${HAL}/Inc
    ${CMSIS}/Core/Include
    ${CMSIS}/Device/ST/STM32MP2xx/Include
    ${OPENAMP}/open-amp/lib/include
    ${CMAKE_BINARY_DIR}/libmetal/lib/include
    ${CUBE_DIR}/Utilities/CoproSync
    ${CUBE_DIR}/Utilities/ResourcesManager
)

# The same definitions ST's OpenAMP example is built with (its .cproject), plus
# __LOG_TRACE_IO_, which sends printf to the resource table's trace buffer -
# readable from Linux in /sys/kernel/debug/remoteproc/remoteprocN/trace0. This
# firmware has no UART of its own.
target_compile_definitions(med_m33_producer.elf PRIVATE
    CORE_CM33
    STM32MP257Cxx
    STM32MP25xxxx
    USE_HAL_DRIVER
    METAL_INTERNAL
    METAL_MAX_DEVICE_REGIONS=2
    VIRTIO_DEVICE_ONLY
    NO_ATOMIC_64_SUPPORT
    LINUX_RPROC_MASTER
    __LOG_TRACE_IO_
)

target_compile_options(med_m33_producer.elf PRIVATE
    ${CPU_FLAGS} -Os -g3 -ffunction-sections -fdata-sections --specs=nano.specs
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
)

# Warnings as errors for OUR sources only: ST's and OpenAMP's are compiled as
# their authors ship them, and holding them to this project's flags would turn
# every Cube update into a patch series.
set_source_files_properties(src/main.c src/med_producer.c src/afe.c src/afe_regs.c PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow;-Werror")

target_link_options(med_m33_producer.elf PRIVATE
    ${CPU_FLAGS} --specs=nano.specs
    -T${CMSIS}/Device/ST/STM32MP2xx/Source/Templates/gcc/linker/stm32mp2xx_DDR_m33_ns.ld
    -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/med_m33_producer.map
    -z max-page-size=4096 -static
)
target_link_libraries(med_m33_producer.elf PRIVATE m)

add_custom_command(TARGET med_m33_producer.elf POST_BUILD
    COMMAND arm-none-eabi-size --format=berkeley med_m33_producer.elf
    COMMAND arm-none-eabi-objcopy -S med_m33_producer.elf med_m33_producer_stripped.elf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
