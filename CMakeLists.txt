cmake_minimum_required(VERSION 3.22)
project(ember LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EMBER_BUILD_TESTS "Build host-side unit tests" OFF)

# Platform independent code (no Vulkan, no Android headers).
# zlib: provided by the NDK on Android (libz), found on the host for tests.
if(ANDROID)
    set(EMBER_ZLIB z)
else()
    find_package(ZLIB REQUIRED)
    set(EMBER_ZLIB ZLIB::ZLIB)
endif()

add_library(ember_core STATIC src/vfs/vfs.cpp src/vfs/bhd5.cpp src/vfs/dcx.cpp src/vfs/bnd.cpp)
target_link_libraries(ember_core PUBLIC ${EMBER_ZLIB})
target_include_directories(ember_core PUBLIC src)

if(EMBER_BUILD_TESTS)
    enable_testing()
    add_executable(vfs_test tests/vfs_test.cpp)
    target_link_libraries(vfs_test PRIVATE ember_core)
    add_test(NAME vfs_test COMMAND vfs_test)
    add_executable(bhd5_test tests/bhd5_test.cpp)
    target_link_libraries(bhd5_test PRIVATE ember_core)
    add_test(NAME bhd5_test COMMAND bhd5_test)
    add_executable(dcx_test tests/dcx_test.cpp)
    target_link_libraries(dcx_test PRIVATE ember_core)
    add_test(NAME dcx_test COMMAND dcx_test)
    add_executable(bnd_test tests/bnd_test.cpp)
    target_link_libraries(bnd_test PRIVATE ember_core)
    add_test(NAME bnd_test COMMAND bnd_test)
endif()

if(ANDROID)
    set(GLUE_DIR ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)
    add_library(native_app_glue STATIC ${GLUE_DIR}/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC ${GLUE_DIR})

    add_library(ember SHARED
        src/platform/android_main.cpp
        src/gfx/renderer.cpp
        src/app/engine.cpp
        src/core/crash.cpp)
    target_compile_definitions(ember PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
    target_link_libraries(ember PRIVATE ember_core native_app_glue android log vulkan)
    # Keep the glue's entry point from being stripped.
    target_link_options(ember PRIVATE -u ANativeActivity_onCreate)
endif()
