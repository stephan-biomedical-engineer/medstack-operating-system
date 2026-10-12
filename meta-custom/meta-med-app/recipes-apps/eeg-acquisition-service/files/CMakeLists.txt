cmake_minimum_required(VERSION 3.16)

project(eeg-acquisition-service
        VERSION 1.0.0
        DESCRIPTION "MedPlatform EEG acquisition service"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)

# The only dependency, by design. If this list ever grows an OS or BSP library,
# a layer boundary has been crossed.
pkg_check_modules(MEDFRAMEWORK REQUIRED IMPORTED_TARGET medframework)

add_executable(eeg-acquisition-service src/main.cpp)

# The clinical file of a finished session (implementation_plan_bdf.md). A
# second program and not a thread of the first: acquisition never waits for
# it, and it never runs in acquisition's path. Same single dependency.
add_executable(eeg-record-export src/export_main.cpp src/record_export.cpp)
add_executable(medctl src/medctl.cpp)

foreach(target eeg-acquisition-service eeg-record-export medctl)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    )
    target_link_libraries(${target} PRIVATE PkgConfig::MEDFRAMEWORK)
endforeach()

install(TARGETS eeg-acquisition-service eeg-record-export medctl
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
