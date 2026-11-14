# Native Windows GUI/MCP with multiple isolated serial sessions; no TUI target.
cmake_minimum_required(VERSION 3.21)
project(serial_mcp_cpp LANGUAGES CXX RC)
if(NOT WIN32)
    message(FATAL_ERROR "WinAPI requires a Windows target; use -DCMAKE_TOOLCHAIN_FILE=cmake/mingw64.cmake")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(serial_mcp_gui WIN32 ${SOURCES} app.rc)
target_include_directories(serial_mcp_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
option(SERIAL_MCP_SIMULATED_TESTS "Compile simulated serial log regression tests" OFF)
if(SERIAL_MCP_SIMULATED_TESTS)
    target_sources(serial_mcp_gui PRIVATE tests/simulated_serial_log.cpp)
    target_include_directories(serial_mcp_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(serial_mcp_gui PRIVATE SERIAL_MCP_SIMULATED_TESTS)
endif()
target_compile_definitions(serial_mcp_gui PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(serial_mcp_gui PRIVATE comctl32 dwmapi gdi32 user32 shell32 ws2_32 bcrypt)
if(MINGW)
    target_compile_options(serial_mcp_gui PRIVATE -Wall -Wextra -Wno-misleading-indentation -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(serial_mcp_gui PRIVATE -static -static-libgcc -static-libstdc++ -municode)
elseif(MSVC)
    target_compile_options(serial_mcp_gui PRIVATE /utf-8 /W4)
endif()
# Publish after every link; default publish also repairs/verifies dist on no-op builds.
add_custom_command(TARGET serial_mcp_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DSOURCE=$<TARGET_FILE:serial_mcp_gui>"
        "-DDEST=${CMAKE_SOURCE_DIR}/dist/serial_mcp_gui.exe"
        "-DROOT=${CMAKE_SOURCE_DIR}" -P ${CMAKE_SOURCE_DIR}/publish.cmake
    VERBATIM)
add_custom_target(publish ALL
    COMMAND ${CMAKE_COMMAND} "-DSOURCE=$<TARGET_FILE:serial_mcp_gui>"
        "-DDEST=${CMAKE_SOURCE_DIR}/dist/serial_mcp_gui.exe"
        "-DROOT=${CMAKE_SOURCE_DIR}" -P ${CMAKE_SOURCE_DIR}/publish.cmake
    DEPENDS serial_mcp_gui
    VERBATIM)

enable_testing()
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME self_tests COMMAND serial_mcp_gui --self-test)
    set_tests_properties(self_tests PROPERTIES TIMEOUT 60)
endif()
# Python is a verification dependency only; PE imports can be checked on any host.
find_package(Python3 3.11 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME static_runtime_imports
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/verify.py --binaries
            $<TARGET_FILE:serial_mcp_gui>)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME mcp_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/verify.py)
        set_tests_properties(mcp_integration PROPERTIES TIMEOUT 60)
    endif()
else()
    message(STATUS "Python 3.11+ not found: PE import verification unavailable")
endif()
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} -C $<CONFIG> --output-on-failure
    DEPENDS publish
    USES_TERMINAL
    VERBATIM)
