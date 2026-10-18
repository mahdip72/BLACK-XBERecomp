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
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)
# src/sound, src/d3d, src/audio and src/common/game_files.cpp replace the SDK ones
list(FILTER XBE_RUNTIME_SOURCES EXCLUDE REGEX "/(xdk/dsound|xdk/d3d8|gpu/d3d12|gpu/render_thread|audio/xaudio2|app/game_files)\\.cpp$")

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

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
        ${SDK}/runtime/gpu/shaders.cpp)
    target_include_directories(new_smoke_dof_warp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
    target_link_libraries(new_smoke_dof_warp PRIVATE d3d12 dxgi dxguid d3dcompiler)

    # Explicit hardware-only, offscreen GPU timestamps for the smoke DoF shader path.
    add_executable(new_smoke_dof_gpu_bench
        tools/tests/new_smoke_dof_gpu_bench.cpp
        ${SDK}/runtime/gpu/shaders.cpp)
    target_include_directories(new_smoke_dof_gpu_bench PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${SDK}/runtime")
    target_link_libraries(new_smoke_dof_gpu_bench PRIVATE d3d12 dxgi dxguid d3dcompiler)

    # Hardware regression for the production atlas clear, including large rectangle lists.
    add_executable(particle_clear_d3d12 tools/tests/particle_clear_d3d12.cpp)
    target_include_directories(particle_clear_d3d12 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(particle_clear_d3d12 PRIVATE d3d12 dxgi dxguid)
endif()

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
