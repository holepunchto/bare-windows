cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-bare-bundle REQUIRED PATHS node_modules/cmake-bare-bundle)

project(bare_windows C)

bare_arch(arch)

# The release zip extracts to win32/<arch>/bare-kit.{dll,lib,def}.
fetch_package("https://github.com/holepunchto/bare-kit/releases/download/v2.4.3/prebuilds.zip" SOURCE_DIR barekit)

set(barekit_dir "${barekit}/win32/${arch}")

add_library(bare_kit SHARED IMPORTED)

set_target_properties(
  bare_kit
  PROPERTIES
    IMPORTED_LOCATION "${barekit_dir}/bare-kit.dll"
    IMPORTED_IMPLIB "${barekit_dir}/bare-kit.lib"
)

# Link the worklet's native addons into build/app. No CMake module for bare-link
# yet, so drive it directly.
find_program(node NAMES node REQUIRED)

# bin.js, not node_modules/.bin/bare-link: npm on Windows writes shell/.cmd/.ps1
# shims there, which node cannot parse.
resolve_node_module(bare-link bare_link WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# --host explicitly, or bare-link defaults to Node's arch. The usual Node on
# Windows-on-ARM is x64 under emulation, which would link win32-x64 addons into
# an arm64 process.
execute_process(
  COMMAND "${node}" "${bare_link}/bin.js" --host "win32-${arch}" --out "${CMAKE_BINARY_DIR}/app" .
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE bare_link_result
)

if(NOT bare_link_result EQUAL 0)
  message(FATAL_ERROR "bare-link failed (${bare_link_result})")
endif()

# Windows has no rpath: the loader searches the exe's own directory, so the DLL
# goes in beside the addons.
file(COPY "${barekit_dir}/bare-kit.dll" DESTINATION "${CMAKE_BINARY_DIR}/app")

# Pack the worklet bundle from bare-switch-core's backend.
add_bare_bundle(app_bundle
  ENTRY node_modules/bare-switch-core/backend.js
  OUT "${CMAKE_BINARY_DIR}/app/app.bundle"
  LINKED
)

# Typed RPC: librpc provides the `rpc` target and transitively fetches libcompact.
# Pinned to a short hash (native libraries aren't release-tagged).
fetch_package("github:holepunchto/librpc#165ee9e")

resolve_node_module(bare-switch-core switch_core WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# schema before hrpc: hrpc's sync_schema target must exist first.
add_subdirectory("${switch_core}/spec/schema" "${CMAKE_BINARY_DIR}/spec/schema")
add_subdirectory("${switch_core}/spec/hrpc" "${CMAKE_BINARY_DIR}/spec/hrpc")

add_subdirectory(app)
