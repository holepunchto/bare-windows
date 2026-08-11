add_executable(bare_windows main.c)

set_target_properties(bare_windows PROPERTIES C_STANDARD 11)

# `.` for the vendored forward-declaration header (bare-kit.h); bare-headers
# supplies the real <uv.h>.
resolve_node_module(bare-headers bare_headers WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

target_include_directories(
  bare_windows
  PRIVATE
    .
    "${bare_headers}/include"
)

# Prebuilt addons delay-load `bare.dll` and resolve it against the host exe via
# GetProcAddress(exe, "bare_module_find"). The runtime actually lives in
# bare-kit.dll, so re-export its symbols from the exe as forwarders. Task 0
# proved this is necessary: without it the worklet dies during addon load, and
# because win32 IPC surfaces no end-of-stream it dies silently.
#
# This has to run at BUILD time, not configure time: bare-kit is built from
# source here, so the DLL does not exist while CMake is configuring, and
# $<TARGET_FILE:bare_kit> is a generator expression that execute_process would
# receive unexpanded. (The Task 0 spike could read the table during configure
# only because it linked an already-built bare-kit as an IMPORTED target.)
find_program(llvm_readobj NAMES llvm-readobj REQUIRED)

set(forwarders_def "${CMAKE_CURRENT_BINARY_DIR}/bare-kit.def")

add_custom_command(
  OUTPUT "${forwarders_def}"
  COMMAND
    "${CMAKE_COMMAND}"
      -D "READOBJ=${llvm_readobj}"
      -D "DLL=$<TARGET_FILE:bare_kit>"
      -D "OUT=${forwarders_def}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/gen-forwarders.cmake"
  DEPENDS bare_kit "${CMAKE_CURRENT_SOURCE_DIR}/gen-forwarders.cmake"
  COMMENT "Generating bare-kit export forwarders"
  VERBATIM
)

add_custom_target(bare_kit_forwarders DEPENDS "${forwarders_def}")

add_dependencies(bare_windows bare_kit_forwarders)

# sync_hrpc/sync_schema are OBJECT libraries; objects reached only transitively
# through another OBJECT library aren't pulled into the executable, so link both
# directly. Their rpc/compact/utf deps come via rpc_static, whose static-archive
# interface does propagate.
# Link bare-kit through its import library rather than by target name. Naming
# the target would drag in its link interface, and bare-kit's `bare_worklet`
# carries `INTERFACE $<LINK_LIBRARY:WHOLE_ARCHIVE,bare_static>` which win32's
# CMakeLists propagates with PUBLIC - so the exe would get the whole static
# runtime *and* the DLL's import library, and every js_*/uv_* symbol would be
# defined twice ("duplicate symbol: js_create_int32", ~700 of them).
#
# The import library alone is what a consumer of a shared library actually needs.
# bare-linux never hits this because it links a prebuilt .so as an IMPORTED
# target, which has no such interface. The upstream fix is holepunchto/bare-kit
# linking bare_worklet PRIVATE there; this stays correct either way.
add_dependencies(bare_windows bare_kit)

target_link_libraries(
  bare_windows
  PRIVATE
    "$<TARGET_LINKER_FILE:bare_kit>"
    sync_hrpc
    sync_schema
    rpc_static
    user32
)

# CMake only turns a .def SOURCE into /DEF: for shared libraries; on an
# executable it is silently ignored, producing an exe that exports nothing.
# Pass it by hand, and with no -Wl, prefix - bare-make drives lld-link.exe
# directly rather than through the clang driver, so -Wl, flags are discarded.
target_link_options(bare_windows PRIVATE "/DEF:${forwarders_def}")

# Relink if the forwarder list changes, e.g. after a bare-kit version bump.
set_target_properties(bare_windows PROPERTIES LINK_DEPENDS "${forwarders_def}")

target_compile_definitions(bare_windows PRIVATE BUNDLE_PATH="${CMAKE_BINARY_DIR}/app/app.bundle")

add_dependencies(bare_windows app_bundle)

# Windows has no rpath: the loader searches the directory containing the exe, so
# bare-kit.dll has to sit next to it.
add_custom_command(
  TARGET bare_windows POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:bare_kit>" "$<TARGET_FILE_DIR:bare_windows>"
)
