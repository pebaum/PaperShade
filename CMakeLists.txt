cmake_minimum_required(VERSION 3.25)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PAPERSHADE_VERSION LIMIT_COUNT 1)
project(PaperShade VERSION ${PAPERSHADE_VERSION} LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CTest)
add_library(papershade_core STATIC core/PaperShadeCore.cpp)
target_include_directories(papershade_core PUBLIC core/include PRIVATE src)
if(BUILD_TESTING)
    add_executable(FilterTests tests/FilterTests.cpp)
    target_include_directories(FilterTests PRIVATE src)
    add_test(NAME filter-math COMMAND FilterTests)
    add_executable(CoreBridgeTests tests/CoreBridgeTests.cpp)
    target_include_directories(CoreBridgeTests PRIVATE src)
    target_link_libraries(CoreBridgeTests PRIVATE papershade_core)
    add_test(NAME shared-core-abi COMMAND CoreBridgeTests)
endif()

if(NOT WIN32)
    message(STATUS "Building the portable filter core. Build the macOS app with macOS/build.sh.")
    return()
endif()
if(NOT MSVC)
    message(FATAL_ERROR "The Windows app requires the MSVC Windows SDK toolchain.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build native x64 or ARM64. Magnification does not support WOW64.")
endif()
if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION VERSION_LESS "10.0.26100.0")
    message(FATAL_ERROR "Build with Windows SDK 10.0.26100.0 or newer.")
endif()
enable_language(RC)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Filter.hlsl")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/Filter.hlsl" FILTER_SHADER)
configure_file(src/ShaderSource.h.in generated/ShaderSource.h @ONLY)
configure_file(src/AppVersion.h.in generated/AppVersion.h @ONLY)
configure_file(src/App.manifest.in generated/App.manifest @ONLY)
set(PAPERSHADE_ICON "${CMAKE_CURRENT_BINARY_DIR}/generated/PaperShade.ico")
add_custom_command(OUTPUT "${PAPERSHADE_ICON}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/GenerateIcon.ps1" -OutputPath "${PAPERSHADE_ICON}"
    DEPENDS tools/GenerateIcon.ps1 VERBATIM)
configure_file(src/App.rc.in generated/App.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/App.rc"
    PROPERTIES OBJECT_DEPENDS "${PAPERSHADE_ICON}")

add_library(papershade_gpu STATIC src/GpuRenderer.cpp)
target_include_directories(papershade_gpu PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(papershade_gpu PUBLIC d3d11 dxgi d3dcompiler windowsapp)

add_executable(PaperShade WIN32
    src/Main.cpp src/Settings.cpp src/ColorEffect.cpp src/CaptureEngine.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/App.rc" "${PAPERSHADE_ICON}")
target_link_libraries(PaperShade PRIVATE papershade_gpu
    windowsapp dcomp dwmapi Magnification shell32 advapi32 wtsapi32 powrprof uuid)
target_link_options(PaperShade PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_BINARY_DIR}/generated/App.manifest")
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT PaperShade)

option(PAPERSHADE_TEST_WARP "Use Microsoft's reference renderer in shader tests (not in the app)" OFF)
if(BUILD_TESTING)
    add_executable(ShaderTests tests/ShaderTests.cpp)
    target_link_libraries(ShaderTests PRIVATE papershade_gpu)
    if(PAPERSHADE_TEST_WARP)
        add_test(NAME shader-reference COMMAND ShaderTests --warp)
    else()
        add_test(NAME shader-reference COMMAND ShaderTests)
    endif()
    add_executable(DesktopTests tests/DesktopTests.cpp src/ColorEffect.cpp src/CaptureEngine.cpp)
    target_link_libraries(DesktopTests PRIVATE papershade_gpu windowsapp dcomp dwmapi Magnification)
endif()

install(TARGETS PaperShade RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
install(FILES tools/Install.ps1 DESTINATION .)
