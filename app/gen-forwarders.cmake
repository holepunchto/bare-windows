# Generate a .def that re-exports bare-kit.dll's runtime symbols from the host
# executable as forwarders. Run at build time via `cmake -P`, because the DLL has
# to exist before its export table can be read - see app/CMakeLists.txt.
#
# Expects READOBJ (path to llvm-readobj), DLL (the built bare-kit.dll) and OUT
# (the .def to write).

foreach(var READOBJ DLL OUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "gen-forwarders.cmake: ${var} is required")
  endif()
endforeach()

execute_process(
  COMMAND "${READOBJ}" --coff-exports "${DLL}"
  OUTPUT_VARIABLE exports_raw
  RESULT_VARIABLE exports_result
)

if(NOT exports_result EQUAL 0)
  message(FATAL_ERROR "llvm-readobj failed on ${DLL} (${exports_result})")
endif()

string(REPLACE "\n" ";" export_lines "${exports_raw}")

set(forwarders "EXPORTS\n")
set(count 0)

# The identifier pattern also filters out the ~2600 mangled C++ V8 exports,
# leaving the C symbols that addons can actually import.
foreach(line ${export_lines})
  if(line MATCHES "Name: ([A-Za-z_][A-Za-z0-9_]*)")
    string(APPEND forwarders "  ${CMAKE_MATCH_1} = bare-kit.${CMAKE_MATCH_1}\n")
    math(EXPR count "${count} + 1")
  endif()
endforeach()

if(count EQUAL 0)
  message(FATAL_ERROR "${DLL} exports nothing parseable - forwarding cannot work")
endif()

file(WRITE "${OUT}" "${forwarders}")

message(STATUS "bare-windows: forwarding ${count} bare-kit exports")
