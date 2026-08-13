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

# A prebuilt addon delay-loads `bare.dll` and resolves it against the host
# executable, so the exe re-exports bare-kit's symbols or no addon loads -
# silently, since win32 IPC surfaces no end-of-stream. bare-kit ships the
# forwarders for the ABI it exposes.
set(forwarders_def "${barekit_dir}/bare-kit.def")

if(NOT EXISTS "${forwarders_def}")
  message(FATAL_ERROR "no ${forwarders_def} - addons cannot resolve the runtime")
endif()

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
