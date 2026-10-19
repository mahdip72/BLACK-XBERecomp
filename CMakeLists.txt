# Black (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(black LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    # This revision contains the compiler/DLL path; its immediate successor reverted it.
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git
        GIT_TAG b47e2e93cdb2166b03354cb299f679a236551d57)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)
if(NOT TARGET compiler_files)
    message(FATAL_ERROR "The SDK must support first-start game DLL compilation (b47e2e93cdb2166b03354cb299f679a236551d57).")
endif()
# src/sound, src/d3d, src/audio and src/common/game_files.cpp replace the SDK ones
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_files)\\.cpp$")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
# gyro aiming reads the controllers' sensors through SDL
set_source_files_properties(src/common/gyro.cpp PROPERTIES INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include")

# partitions as config/game.toml sets them
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

add_xbe_image(Black default.xbe game.toml ${GAME_PARTITIONS} game)

# Build the project's fixed/native-particle DoF shaders independently of the optional
# official FidelityFX host component. This library is also used by balanced/viewmodel
# paths and by offline fallback builds with BLACK_FIDELITYFX_DOF=OFF.
add_executable(build_dof_shaders
    tools/build_dof_shaders.cpp
    ${SDK}/runtime/gpu/shaders.cpp)
target_include_directories(build_dof_shaders PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
target_compile_definitions(build_dof_shaders PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(build_dof_shaders PRIVATE d3dcompiler)
set(DOF_SHADER_LIBRARY_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/dof_shader_library.cpp")
add_custom_command(
    OUTPUT "${DOF_SHADER_LIBRARY_CPP}"
    COMMAND $<TARGET_FILE:build_dof_shaders> "${DOF_SHADER_LIBRARY_CPP}"
    DEPENDS build_dof_shaders
        tools/build_dof_shaders.cpp
        src/d3d/dof_shader_library.h
        src/d3d/shader_cache.h
        src/d3d/dof_shader_source.h
        src/d3d/depth_of_field_shader.h
        src/d3d/viewmodel_dof_shader.h
        src/d3d/fidelityfx_dof_shader.h
        src/d3d/particle_dof_shader.h
        src/d3d/particle_dof_wrapper.h
        src/d3d/particle_dof_small_radius.h
        "${SDK}/runtime/gpu/shaders.h"
    VERBATIM)
set_source_files_properties("${DOF_SHADER_LIBRARY_CPP}" PROPERTIES GENERATED TRUE)
add_custom_target(embedded_dof_shaders DEPENDS "${DOF_SHADER_LIBRARY_CPP}")
target_sources(Black PRIVATE "${DOF_SHADER_LIBRARY_CPP}")
add_dependencies(Black embedded_dof_shaders)

# CPU-only first-use contract test. It compiles the generated table and does not
# construct a graphics device or start the game.
add_executable(dof_first_use
    tools/tests/dof_first_use.cpp
    "${DOF_SHADER_LIBRARY_CPP}"
    ${SDK}/runtime/gpu/shaders.cpp)
target_include_directories(dof_first_use PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
target_link_libraries(dof_first_use PRIVATE d3dcompiler)
add_dependencies(dof_first_use embedded_dof_shaders)

# Offscreen WARP regression for the asynchronous production pipeline cache. It consumes
# only embedded shader blobs and never calls FXC or starts the game.
add_executable(dof_pipeline_cache_test
    tools/tests/dof_pipeline_cache_test.cpp
    "${DOF_SHADER_LIBRARY_CPP}"
    ${SDK}/runtime/gpu/shaders.cpp)
target_include_directories(dof_pipeline_cache_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
target_link_libraries(dof_pipeline_cache_test PRIVATE d3d12 dxgi dxguid d3dcompiler)
add_dependencies(dof_pipeline_cache_test embedded_dof_shaders)

# Explicit, CPU-only compiler study. This experimental target never changes the shipped
# O1 embedded library and never creates a graphics device.
add_executable(dof_shader_optimization_compare
    tools/tests/dof_shader_optimization_compare.cpp
    ${SDK}/runtime/gpu/shaders.cpp)
target_include_directories(dof_shader_optimization_compare PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
target_compile_definitions(dof_shader_optimization_compare PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(dof_shader_optimization_compare PRIVATE d3dcompiler dxguid)

# Explicit offscreen GPU timing harness, independent of the optional FidelityFX SDK adapter.
add_executable(new_smoke_dof_gpu_bench
    tools/tests/new_smoke_dof_gpu_bench.cpp
    ${SDK}/runtime/gpu/shaders.cpp)
target_include_directories(new_smoke_dof_gpu_bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
target_link_libraries(new_smoke_dof_gpu_bench PRIVATE d3d12 dxgi dxguid d3dcompiler)

# Isolated timing measurements for the GPU DoF telemetry helpers. Defaults to WARP;
# hardware timing requires an explicit --hardware argument.
add_executable(dof_gpu_timing_test tools/tests/dof_gpu_timing_test.cpp)
target_include_directories(dof_gpu_timing_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(dof_gpu_timing_test PRIVATE d3d12 dxgi dxguid)

# Offscreen viewmodel DoF WARP regression, independent of the optional FFX adapter.
add_executable(viewmodel_dof_test tools/tests/viewmodel_dof_warp.cpp)
target_include_directories(viewmodel_dof_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(viewmodel_dof_test PRIVATE d3d11 d3dcompiler dxgi dxguid)

# CPU-only radius and dispatch-planner regression.
add_executable(particle_small_radius_cpu tools/tests/particle_small_radius_cpu.cpp)
target_include_directories(particle_small_radius_cpu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(particle_coverage_cpu tools/tests/particle_coverage_cpu.cpp)
target_include_directories(particle_coverage_cpu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Official AMD FidelityFX DoF component and DX12 backend. The SDK itself is
# prepared as a sparse, pinned checkout by tools/cmake/prepare_fidelityfx_sdk.ps1.
option(BLACK_FIDELITYFX_DOF "Build BLACK's DoF adapter against the official FidelityFX SDK" ON)
if(BLACK_FIDELITYFX_DOF)
    set(BLACK_FIDELITYFX_SDK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/out/deps/FidelityFX-SDK" CACHE PATH
        "Pinned FidelityFX-SDK source checkout")
    if(NOT EXISTS "${BLACK_FIDELITYFX_SDK_SOURCE}/sdk/CMakeLists.txt")
        message(FATAL_ERROR
            "FidelityFX SDK source is missing. Run tools/cmake/prepare_fidelityfx_sdk.ps1 first.")
    endif()

    set(FFX_API_BACKEND DX12_X64 CACHE STRING "FidelityFX backend" FORCE)
    set(FFX_DOF ON CACHE BOOL "Build the FidelityFX DoF component" FORCE)
    set(FFX_ALL OFF CACHE BOOL "Build all FidelityFX components" FORCE)
    set(FFX_BUILD_AS_DLL OFF CACHE BOOL "Build FidelityFX as static libraries" FORCE)
    set(FFX_AUTO_COMPILE_SHADERS ON CACHE BOOL "Compile FidelityFX shader permutations" FORCE)

    # The SDK determines its library suffix from this variable. Ninja has no
    # generator platform setting, so tell the SDK explicitly that this target
    # is x64 while leaving CMake's active compiler/generator untouched.
    set(_BLACK_FFX_SAVED_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
    set(CMAKE_GENERATOR_PLATFORM x64)
    add_subdirectory("${BLACK_FIDELITYFX_SDK_SOURCE}/sdk" "${CMAKE_BINARY_DIR}/fidelityfx-sdk")
    set(CMAKE_GENERATOR_PLATFORM "${_BLACK_FFX_SAVED_GENERATOR_PLATFORM}")
    # The upstream Windows toolchain also forces this cache entry, which makes
    # a later Ninja `cmake --build` reject the unsupported -A/platform value.
    set(CMAKE_GENERATOR_PLATFORM "${_BLACK_FFX_SAVED_GENERATOR_PLATFORM}" CACHE STRING "" FORCE)

    # Upstream CMake defaults libraries to <source>/bin/ffx_sdk. Keep all
    # generated files in this build directory instead.
    set_target_properties(ffx_dof_x64 ffx_backend_dx12_x64 PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fidelityfx-sdk/lib")
    foreach(_config Debug Release RelWithDebInfo)
        string(TOUPPER "${_config}" _config_upper)
        set_target_properties(ffx_dof_x64 ffx_backend_dx12_x64 PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY_${_config_upper} "${CMAKE_BINARY_DIR}/fidelityfx-sdk/lib/${_config}")
    endforeach()

    target_compile_definitions(Black PRIVATE BLACK_FIDELITYFX_DOF=1)
    target_link_libraries(Black PRIVATE ffx_dof_x64 ffx_backend_dx12_x64 d3d12 dxgi)

    # CPU-driven WARP harness: creates a D3D12 WARP device only when run.
    # It is independent of the game executable and shares the production adapter.
    add_executable(newffx_dof_warp
        tools/tests/newffx_dof_warp.cpp
        src/d3d/fidelityfx_dof.cpp)
    target_include_directories(newffx_dof_warp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(newffx_dof_warp PRIVATE BLACK_FIDELITYFX_DOF=1)
    target_link_libraries(newffx_dof_warp PRIVATE ffx_dof_x64 ffx_backend_dx12_x64 d3d12 dxgi dxguid d3dcompiler)

    # Offscreen software-adapter checks of the production native particle wrapper.
    add_executable(new_smoke_dof_warp
        tools/tests/new_smoke_dof_warp.cpp
        ${SDK}/runtime/gpu/shaders.cpp
        src/d3d/fidelityfx_dof.cpp)
    target_include_directories(new_smoke_dof_warp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
    target_compile_definitions(new_smoke_dof_warp PRIVATE BLACK_FIDELITYFX_DOF=1)
    target_link_libraries(new_smoke_dof_warp PRIVATE
        ffx_dof_x64 ffx_backend_dx12_x64 d3d12 dxgi dxguid d3dcompiler)

    # Hardware regression for the production atlas clear, including large rectangle lists.
    add_executable(particle_clear_d3d12 tools/tests/particle_clear_d3d12.cpp)
    target_include_directories(particle_clear_d3d12 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(particle_clear_d3d12 PRIVATE d3d12 dxgi dxguid)

    # Disk bytecode-cache regression; no graphics device is created.
    add_executable(shader_cache_test tools/tests/shader_cache_test.cpp)
    target_include_directories(shader_cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(shader_cache_test PRIVATE d3dcompiler)
endif()

# CPU-only geometry regression for the particle DOF planner and clipping rules.
add_executable(particle_dof_geometry tools/tests/particle_dof_geometry.cpp)
target_include_directories(particle_dof_geometry PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# build_info.h for the version resource, rewritten only when the commit changes
find_package(Git QUIET)
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO}
    VERBATIM)
# version.rc also embeds src/common/europe.patch
set_source_files_properties(src/common/version.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    OBJECT_DEPENDS "${BUILD_INFO};${CMAKE_CURRENT_SOURCE_DIR}/src/common/europe.patch")
target_sources(Black PRIVATE src/common/version.rc)
add_dependencies(Black build_info)
set_target_properties(Black PROPERTIES OUTPUT_NAME BLACK)
