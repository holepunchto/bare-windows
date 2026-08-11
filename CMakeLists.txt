cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-fetch REQUIRED PATHS node_modules/cmake-fetch)
find_package(cmake-bare-bundle REQUIRED PATHS node_modules/cmake-bare-bundle)

project(bare_windows C)

bare_arch(arch)

# No bare-kit release ships win32 artefacts yet, so build it from source as a
# subproject. The publish job merged upstream on 2026-08-11; once a release runs
# it, this becomes a fetch_package of the release zip, as bare-linux does.
fetch_package("github:holepunchto/bare-kit@2.3.0")

# Link the worklet's native addons (sodium-native, udx-native, ...) for this
# host into build/app, next to where the executable lands. Windows has no rpath;
# the loader searches the directory containing the exe. There is no CMake module
# for bare-link yet, so drive it directly.
find_program(node NAMES node REQUIRED)

# Call bin.js, not node_modules/.bin/bare-link. On Linux that path is a symlink
# to the JS, which is what bare-linux relies on; npm on Windows writes shell,
# .cmd and .ps1 shims there, so node parses a bash script and dies with
# "SyntaxError: missing ) after argument list".
resolve_node_module(bare-link bare_link WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Pass --host explicitly. bare-link defaults to
# `${process.platform}-${process.arch}` - Node's architecture, not the build's.
# The usual Node on Windows-on-ARM is an x64 build under emulation, so the
# default silently links win32-x64 addons into an arm64 process and every load
# fails with "%1 is not a valid Win32 application".
execute_process(
  COMMAND "${node}" "${bare_link}/bin.js" --host "win32-${arch}" --out "${CMAKE_BINARY_DIR}/app" .
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RESULT_VARIABLE bare_link_result
)

if(NOT bare_link_result EQUAL 0)
  message(FATAL_ERROR "bare-link failed (${bare_link_result})")
endif()

# Pack the worklet bundle from bare-switch-core's backend.
add_bare_bundle(app_bundle
  ENTRY node_modules/bare-switch-core/backend.js
  OUT "${CMAKE_BINARY_DIR}/app/app.bundle"
  LINKED
)

# Typed RPC: librpc provides the `rpc` target and transitively fetches libcompact.
# Pinned to a short hash (native libraries aren't release-tagged).
fetch_package("github:holepunchto/librpc#38deb71")

resolve_node_module(bare-switch-core switch_core WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# schema before hrpc: hrpc's sync_schema target must exist first.
add_subdirectory("${switch_core}/spec/schema" "${CMAKE_BINARY_DIR}/spec/schema")
add_subdirectory("${switch_core}/spec/hrpc" "${CMAKE_BINARY_DIR}/spec/hrpc")

add_subdirectory(app)
