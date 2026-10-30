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

add_library(ember_core STATIC src/vfs/vfs.cpp src/vfs/bhd5.cpp src/vfs/dcx.cpp src/vfs/bnd.cpp
    src/vfs/survey.cpp src/formats/flver.cpp src/formats/dds.cpp src/formats/tpf.cpp src/vfs/tpf_scan.cpp src/app/model_loader.cpp src/app/settings.cpp)
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
    add_executable(survey_test tests/survey_test.cpp)
    target_link_libraries(survey_test PRIVATE ember_core)
    add_test(NAME survey_test COMMAND survey_test)
    add_executable(split_test tests/split_test.cpp)
    target_link_libraries(split_test PRIVATE ember_core)
    add_test(NAME split_test COMMAND split_test)
    add_executable(flver_test tests/flver_test.cpp)
    target_link_libraries(flver_test PRIVATE ember_core)
    add_test(NAME flver_test COMMAND flver_test)
    add_executable(texture_test tests/texture_test.cpp)
    target_link_libraries(texture_test PRIVATE ember_core)
    add_test(NAME texture_test COMMAND texture_test)
    add_executable(settings_test tests/settings_test.cpp)
    target_link_libraries(settings_test PRIVATE ember_core)
    add_test(NAME settings_test COMMAND settings_test)
    add_executable(camera_test tests/camera_test.cpp)
    target_link_libraries(camera_test PRIVATE ember_core)
    add_test(NAME camera_test COMMAND camera_test)
endif()

if(ANDROID)
    set(GLUE_DIR ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)
    add_library(native_app_glue STATIC ${GLUE_DIR}/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC ${GLUE_DIR})

    # Shaders: compiled with the NDK's glslc into C arrays included by mesh_renderer.cpp.
    find_program(EMBER_GLSLC glslc REQUIRED
        HINTS ${CMAKE_ANDROID_NDK}/shader-tools/linux-x86_64
              ${CMAKE_ANDROID_NDK}/shader-tools/darwin-x86_64
              ${CMAKE_ANDROID_NDK}/shader-tools/windows-x86_64)
    set(EMBER_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${EMBER_SHADER_DIR})
    set(EMBER_SHADER_INCS)
    foreach(shader mesh.vert mesh.frag)
        add_custom_command(
            OUTPUT ${EMBER_SHADER_DIR}/${shader}.inc
            COMMAND ${EMBER_GLSLC} -mfmt=c -O -o ${EMBER_SHADER_DIR}/${shader}.inc
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${shader}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${shader}
            COMMENT "glslc ${shader}")
        list(APPEND EMBER_SHADER_INCS ${EMBER_SHADER_DIR}/${shader}.inc)
    endforeach()

    add_library(ember SHARED
        src/platform/android_main.cpp
        src/gfx/renderer.cpp
        src/gfx/mesh_renderer.cpp
        ${EMBER_SHADER_INCS}
        src/app/engine.cpp
        src/core/crash.cpp)
    target_include_directories(ember PRIVATE ${EMBER_SHADER_DIR})
    target_compile_definitions(ember PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
    target_link_libraries(ember PRIVATE ember_core native_app_glue android log vulkan)
    # Keep the glue's entry point from being stripped.
    target_link_options(ember PRIVATE -u ANativeActivity_onCreate)
endif()
