# MedFramework - the C++ abstraction layer of the MedPlatform reference
# architecture.
#
# Deliberately has exactly two external dependencies, both of them properties
# of the OS policy that meta-med-distro guarantees on every target:
#   libsystemd - sd-journal (MedicalLogger) and sd-bus (MedicalUpdate)
#   libcrypto  - SHA-256 for the audit hash chain and the configuration digest
#
# Everything else is POSIX. That is what lets the same library build for
# qemux86-64 and for the STM32MP257 without a single conditional.

cmake_minimum_required(VERSION 3.16)

project(medframework
        VERSION 1.0.0
        DESCRIPTION "MedPlatform medical abstraction framework"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(OpenSSL REQUIRED)

# The AMP wire format is not a dependency in the sense the two above are: it is
# this project's own header, packaged by med-amp-abi so that the firmware on the
# real-time core can include the same definition. MedicalDevice.h includes it,
# so it is part of this library's public interface. find_path rather than a
# hard-coded path, so that it is found in a cross sysroot and on a host alike.
find_path(MED_AMP_ABI_INCLUDE_DIR med_amp_abi.h PATH_SUFFIXES medplatform)
if(NOT MED_AMP_ABI_INCLUDE_DIR)
    message(FATAL_ERROR "med_amp_abi.h not found: build med-amp-abi first, or set "
                        "MED_AMP_ABI_INCLUDE_DIR to the directory holding it")
endif()

add_library(medframework SHARED
    src/med-framework.cpp
    src/MedicalLogger.cpp
    src/MedicalIPC.cpp
    src/MedicalStorage.cpp
    src/MedicalConfiguration.cpp
    src/MedicalUpdate.cpp
    src/MedicalDevice.cpp
)

target_compile_definitions(medframework
    PRIVATE MED_FRAMEWORK_VERSION="${PROJECT_VERSION}"
)

target_compile_options(medframework PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter
)

target_include_directories(medframework
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${MED_AMP_ABI_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(medframework
    PRIVATE
        PkgConfig::SYSTEMD
        OpenSSL::Crypto
)

# SOVERSION is part of the regulatory story, not only of packaging: an
# application validated against ABI 1 must not silently load ABI 2 after an
# update.
set_target_properties(medframework PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

install(TARGETS medframework
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/medplatform
        FILES_MATCHING PATTERN "*.h")

configure_file(medframework.pc.in medframework.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/medframework.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
