// SPDX-License-Identifier: MIT
//
// Host-side functional tests for the MedFramework.
//
// These run against the library's own sources compiled for the host, with no
// bitbake, no image and no target. What they can therefore NOT say anything
// about is in this directory's README, and the list is not short - the point of
// writing it down is that an absence must not be read as a result.

#include <cstdio>

#include "check.h"

void testTypes();
void testConfiguration();
void testStorage();
void testDevice();
void testAmpAbi();
#ifdef MED_TESTS_HAVE_SYSTEMD
void testLogger();
void testUpdate();
#endif

int main() {
    std::printf("MedFramework - verificações de host\n");

    testTypes();
    testConfiguration();
    testStorage();
    testDevice();
    testAmpAbi();
#ifdef MED_TESTS_HAVE_SYSTEMD
    testLogger();
    testUpdate();
#endif

    const int status = medtest::report();

    // What was NOT covered, printed on every run including a green one.
    //
    // A green suite that quietly tested less is the failure mode this
    // repository has already paid for: 22 assertions reported 22/22 on an image
    // delivering 94 of 250 samples per second, because every one of them
    // measured state and none measured function. The cheapest defence is to
    // make the boundary of the suite part of its output.
    std::printf("\nNão coberto por esta suíte:\n");
#ifndef MED_TESTS_HAVE_SYSTEMD
    std::printf("  - MedicalLogger  (libsystemd ausente neste host:"
                " instale libsystemd-dev e recompile)\n");
    std::printf("  - MedicalUpdate  (inteiro: libsystemd ausente neste host)\n");
#else
    std::printf("  - MedicalUpdate  (a metade D-Bus: connect, install, mark*;"
                " precisa do daemon do RAUC no barramento)\n");
#endif
    std::printf("  - MedicalIPC     (o caminho socket é exercitado por make check, no alvo)\n");
    std::printf("  - atomicidade de MedicalStorage (precisa de corte de energia)\n");
    std::printf("  - qualquer coisa que dependa de hardware: ver README.md\n");

    return status;
}
