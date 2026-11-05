# infusion-control-service - the infusion pump profile's controller.
#
# Links the MedFramework and nothing else (CLAUDE.md rule 6).

cmake_minimum_required(VERSION 3.16)

project(infusion-control-service
        VERSION 1.0.0
        DESCRIPTION "MedPlatform infusion pump - control service"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MEDFRAMEWORK REQUIRED IMPORTED_TARGET medframework)

add_executable(infusion-control-service
    src/main.cpp
    src/controller.cpp
    src/library.cpp
)

target_compile_options(infusion-control-service PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
)
target_link_libraries(infusion-control-service PRIVATE PkgConfig::MEDFRAMEWORK)

install(TARGETS infusion-control-service RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
