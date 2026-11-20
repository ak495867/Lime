cmake_minimum_required(VERSION 3.16)
project(lime VERSION 1.0.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-specific definitions and libraries
if(WIN32)
    add_compile_definitions(ASMJIT_STATIC)
    set(PLATFORM_LIBS ws2_32 user32 gdi32)
else()
    add_compile_definitions(ASMJIT_STATIC)
    set(PLATFORM_LIBS ${CMAKE_DL_LIBS})
endif()

find_package(Threads REQUIRED)

# Include directories
include_directories(
    include
    third_party/asmjit
)

# Vendored AsmJit source files
file(GLOB ASMJIT_CORE_SRCS "third_party/asmjit/asmjit/core/*.cpp")
file(GLOB ASMJIT_AXL_SRCS  "third_party/asmjit/asmjit/axl/*.cpp")
file(GLOB ASMJIT_ARM_SRCS  "third_party/asmjit/asmjit/arm/*.cpp")
file(GLOB ASMJIT_X86_SRCS  "third_party/asmjit/asmjit/x86/x86_*.cpp")

set(ASMJIT_SRCS
    ${ASMJIT_CORE_SRCS}
    ${ASMJIT_AXL_SRCS}
    ${ASMJIT_ARM_SRCS}
    ${ASMJIT_X86_SRCS}
)
list(FILTER ASMJIT_SRCS EXCLUDE REGEX ".*_test\\.cpp$")

# Lime core sources (excluding main.cpp)
file(GLOB LIME_CORE_SRCS "src/*.cpp")
list(REMOVE_ITEM LIME_CORE_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# Lime CLI Executable
add_executable(lime
    src/main.cpp
    ${LIME_CORE_SRCS}
    ${ASMJIT_SRCS}
)
target_link_libraries(lime PRIVATE Threads::Threads ${PLATFORM_LIBS})

# Lime Test Suite Executable
add_executable(test_lime
    tests/test_lime.cpp
    ${LIME_CORE_SRCS}
    ${ASMJIT_SRCS}
)
target_link_libraries(test_lime PRIVATE Threads::Threads ${PLATFORM_LIBS})

enable_testing()
add_test(NAME test_lime COMMAND test_lime)
