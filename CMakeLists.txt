cmake_minimum_required(VERSION 3.20)
project(AIMP_ETS2_Cast VERSION 0.1.1 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "AIMP ETS2 Cast builds only for Windows")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This prototype must be built as x64 for the installed AIMP x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(aimp_ets2_cast SHARED
    src/aimp_ets2_cast.cpp
    src/aimp_ets2_cast.def
    src/version.rc
)
target_include_directories(aimp_ets2_cast PRIVATE include)
target_compile_definitions(aimp_ets2_cast PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(aimp_ets2_cast PRIVATE ws2_32)
set_target_properties(aimp_ets2_cast PROPERTIES
    PREFIX ""
    OUTPUT_NAME "aimp_ets2_cast"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

if(MINGW)
    target_compile_options(aimp_ets2_cast PRIVATE -Wall -Wextra -Wpedantic -Wno-cast-function-type)
    target_link_options(aimp_ets2_cast PRIVATE -static-libgcc -static-libstdc++)
elseif(MSVC)
    target_compile_options(aimp_ets2_cast PRIVATE /W4 /permissive- /EHsc)
endif()

add_executable(dsp_host_smoke tests/dsp_host_smoke.cpp)
target_include_directories(dsp_host_smoke PRIVATE include)
if(MINGW)
    target_link_options(dsp_host_smoke PRIVATE -municode -static-libgcc -static-libstdc++)
elseif(MSVC)
    target_link_options(dsp_host_smoke PRIVATE /SUBSYSTEM:CONSOLE)
endif()
set_target_properties(dsp_host_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(stream_probe tests/stream_probe.cpp)
target_link_libraries(stream_probe PRIVATE ws2_32)
if(MINGW)
    target_link_options(stream_probe PRIVATE -static-libgcc -static-libstdc++)
endif()
set_target_properties(stream_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

enable_testing()
add_test(NAME export_table
    COMMAND ${CMAKE_COMMAND} -E env EXPECTED_DLL=$<TARGET_FILE:aimp_ets2_cast>
        ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/tests/check_artifact.cmake)
