# infusion-sim - the simulated mechanism of the infusion pump profile.
#
# Links the MedFramework and nothing else (CLAUDE.md rule 6): the sockets are
# MedicalIPC's and the wire format is the framework's codec.

cmake_minimum_required(VERSION 3.16)

project(infusion-sim
        VERSION 1.0.0
        DESCRIPTION "MedPlatform infusion pump - simulated mechanism"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MEDFRAMEWORK REQUIRED IMPORTED_TARGET medframework)

add_executable(infusion-sim
    src/main.cpp
    src/simulator.cpp
    src/mechanism.cpp
    src/syringe_model.cpp
)

add_executable(infusion-sim-fault src/fault_main.cpp)

foreach(target infusion-sim infusion-sim-fault)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    )
    target_link_libraries(${target} PRIVATE PkgConfig::MEDFRAMEWORK)
endforeach()

install(TARGETS infusion-sim infusion-sim-fault RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
