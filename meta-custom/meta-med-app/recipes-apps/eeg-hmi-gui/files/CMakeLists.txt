cmake_minimum_required(VERSION 3.18)

project(eeg-hmi-gui
        VERSION 1.0.0
        DESCRIPTION "MedPlatform EEG operator display"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(PkgConfig REQUIRED)

# Same rule as the acquisition service: the only non-toolkit dependency is
# MedFramework. The HMI reaches the sample stream through MedicalIPC, not
# through a socket of its own.
pkg_check_modules(MEDFRAMEWORK REQUIRED IMPORTED_TARGET medframework)

qt_add_executable(eeg-hmi-gui
    src/main.cpp
    src/EegClient.cpp
    src/EegClient.h
)

qt_add_resources(eeg-hmi-gui "qml"
    PREFIX "/"
    FILES qml/Main.qml
)

target_compile_options(eeg-hmi-gui PRIVATE -Wall -Wextra)

target_link_libraries(eeg-hmi-gui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    PkgConfig::MEDFRAMEWORK
)

install(TARGETS eeg-hmi-gui
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
