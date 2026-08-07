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

target_compile_options(eeg-acquisition-service PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
)

target_link_libraries(eeg-acquisition-service PRIVATE PkgConfig::MEDFRAMEWORK)

install(TARGETS eeg-acquisition-service
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
