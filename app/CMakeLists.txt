# app.manifest goes in as a source, not through a .rc: a .rc collides with the
# manifest CMake generates ("duplicate resource: type MANIFEST (ID 24)").
add_executable(bare_windows WIN32 main.c app.manifest)

set_target_properties(bare_windows PROPERTIES C_STANDARD 11)

resolve_node_module(bare-headers bare_headers WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

target_include_directories(
  bare_windows
  PRIVATE
    .
    "${bare_headers}/include"
)

# Prebuilt addons delay-load `bare.dll` and resolve it against the host exe via
# GetProcAddress(exe, "bare_module_find"). The runtime lives in bare-kit.dll, so
# the exe re-exports its symbols as forwarders or no addon loads - silently,
# since win32 IPC surfaces no end-of-stream.
#
# Read the DLL's export table rather than bare's .def files: those omit
# uv_queue_work, which the stock sodium-native prebuild delay-imports.
find_program(llvm_readobj NAMES llvm-readobj REQUIRED)

get_target_property(bare_kit_dll bare_kit IMPORTED_LOCATION)

execute_process(
  COMMAND "${llvm_readobj}" --coff-exports "${bare_kit_dll}"
  OUTPUT_VARIABLE exports_raw
  RESULT_VARIABLE exports_result
)

if(NOT exports_result EQUAL 0)
  message(FATAL_ERROR "llvm-readobj failed (${exports_result})")
endif()

string(REPLACE "\n" ";" export_lines "${exports_raw}")

set(forwarders "EXPORTS\n")
set(count 0)

# Plain C identifiers only, which also drops the mangled C++ V8 exports. Leave
# the pattern unanchored: llvm-readobj's output keeps its \r, so anchoring with $
# would match nothing.
foreach(line ${export_lines})
  if(line MATCHES "Name: ([A-Za-z_][A-Za-z0-9_]*)")
    string(APPEND forwarders "  ${CMAKE_MATCH_1} = bare-kit.${CMAKE_MATCH_1}\n")
    math(EXPR count "${count} + 1")
  endif()
endforeach()

# Check the symbols, not the count: a partial parse would still link, then fail to
# load addons at runtime with nothing reported.
foreach(symbol bare_module_find uv_queue_work)
  if(NOT forwarders MATCHES "  ${symbol} = ")
    message(FATAL_ERROR "bare-kit.dll exports no ${symbol} - addons cannot resolve the runtime")
  endif()
endforeach()

set(forwarders_def "${CMAKE_CURRENT_BINARY_DIR}/bare-kit.def")

file(GENERATE OUTPUT "${forwarders_def}" CONTENT "${forwarders}")

message(STATUS "bare-windows: forwarding ${count} bare-kit exports")

# sync_hrpc/sync_schema are OBJECT libraries; objects reached only transitively
# through another OBJECT library aren't pulled in, so link both directly.
target_link_libraries(
  bare_windows
  PRIVATE
    bare_kit
    sync_hrpc
    sync_schema
    rpc_static
    comctl32
    user32
)

# CMake wires a .def source into /DEF: for shared libraries only, and silently
# ignores it on an executable. No -Wl, prefix: bare-make drives lld-link.exe
# directly, which discards it.
#
# WIN32 above selects the GUI subsystem, which looks for WinMain; this host
# defines main.
target_link_options(
  bare_windows
  PRIVATE
    "/DEF:${forwarders_def}"
    /entry:mainCRTStartup
)

set_target_properties(bare_windows PROPERTIES LINK_DEPENDS "${forwarders_def}")

target_compile_definitions(bare_windows PRIVATE BUNDLE_PATH="${CMAKE_BINARY_DIR}/app/app.bundle")

add_dependencies(bare_windows app_bundle)
