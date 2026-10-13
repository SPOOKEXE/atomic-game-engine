# Exercise GCC PCH regeneration through the real launcher and an isolated cache.
if(DEFINED MONO_CACHE_ARGUMENT_PROBE)
	file(WRITE "${MONO_CACHE_ARGUMENT_PROBE}" "")
	set(recording FALSE)
	math(EXPR lastArgument "${CMAKE_ARGC} - 1")
	foreach(index RANGE 0 ${lastArgument})
		set(argument "${CMAKE_ARGV${index}}")
		if(recording)
			string(LENGTH "${argument}" length)
			file(APPEND "${MONO_CACHE_ARGUMENT_PROBE}" "${length}:${argument}\n")
		elseif(argument STREQUAL "--")
			set(recording TRUE)
		endif()
	endforeach()
	return()
endif()

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
	message(STATUS "compiler-cache-check: GCC/Linux fixture skipped on this platform")
	return()
endif()
find_program(cache NAMES ccache)
find_program(compiler NAMES g++)
if(NOT cache OR NOT compiler)
	message(STATUS "compiler-cache-check: fixture skipped (requires ccache and g++)")
	return()
endif()
if(NOT DEFINED MONO_TEST_ROOT)
	message(FATAL_ERROR "Set MONO_TEST_ROOT to a directory under the selected build tree")
endif()

get_filename_component(sourceRoot "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(testRoot "${MONO_TEST_ROOT}/compiler-cache-test" ABSOLUTE)
file(MAKE_DIRECTORY "${testRoot}/path with spaces")

# The launcher must retain argument boundaries before it can be trusted with flags.
execute_process(
	COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
		"${CMAKE_COMMAND}" "${sourceRoot}" --
		"-DMONO_CACHE_ARGUMENT_PROBE=${testRoot}/arguments.txt"
		-P "${CMAKE_CURRENT_LIST_FILE}" --
		"one\\" "two" "[first" "second]" "a;b" "a\"b" "\${literal}" ""
	RESULT_VARIABLE argumentStatus)
file(READ "${testRoot}/arguments.txt" arguments)
set(expectedArguments "4:one\\\n3:two\n6:[first\n7:second]\n3:a;b\n3:a\"b\n10:\${literal}\n0:\n")
if(NOT argumentStatus STREQUAL "0" OR NOT arguments STREQUAL expectedArguments)
	message(FATAL_ERROR "Launcher changed arguments:\n${arguments}")
endif()

set(ENV{CCACHE_DIR} "${testRoot}/cache")
set(ENV{CCACHE_STATSLOG} "${testRoot}/stats.log")
file(REMOVE "$ENV{CCACHE_STATSLOG}")
set(header "${testRoot}/path with spaces/Probe.hpp")
set(pch "${header}.gch")
set(source "${testRoot}/Probe.cpp")
set(pchSource "${testRoot}/Probe.hpp.cpp")
file(WRITE "${header}" "#include <vector>\n")
file(WRITE "${source}" "int Count() { return std::vector<int>{1, 2, 3}.size(); }\n")
file(WRITE "${pchSource}" "/* Headers are supplied with -include. */\n")
# Input mtimes must precede the first ccache invocation, even on coarse filesystems.
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
set(probeStandard "c++20")

function(compile_probe output compileSource)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
			"${cache}" "${sourceRoot}" -- "${compiler}" "-std=${probeStandard}" ${ARGN}
			-MD -MF "${testRoot}/Probe.d" -o "${output}" -c "${compileSource}"
		RESULT_VARIABLE result
		OUTPUT_VARIABLE standardOutput ERROR_VARIABLE standardError)
	if(NOT result STREQUAL "0")
		message(FATAL_ERROR "Probe compile failed: ${standardOutput}${standardError}")
	endif()
endfunction()

compile_probe("${pch}" "${pchSource}" -x c++-header -include "${header}")
compile_probe("${testRoot}/Probe.o" "${source}" -include "${header}")
file(SHA256 "${pch}" firstPch)
# The old output still exists here, reproducing a normal Ninja regeneration.
file(REMOVE "${testRoot}/Probe.o")
compile_probe("${pch}" "${pchSource}" -x c++-header -include "${header}")
file(SHA256 "${pch}" restoredPch)
compile_probe("${testRoot}/Probe.o" "${source}" -include "${header}")
if(NOT firstPch STREQUAL restoredPch)
	message(FATAL_ERROR "PCH regeneration changed bytes instead of restoring the cached PCH")
endif()
file(READ "$ENV{CCACHE_STATSLOG}" stats)
string(REGEX MATCHALL "\ndirect_cache_hit\n" hits "${stats}")
list(LENGTH hits hitCount)
if(hitCount LESS 2 OR stats MATCHES "modified_input_file")
	message(FATAL_ERROR "Expected direct hits for PCH and consumer regeneration:\n${stats}")
endif()

# SDL generates a C PCH through the same launcher.
find_program(cCompiler NAMES gcc REQUIRED)
set(compiler "${cCompiler}")
set(probeStandard "c17")
set(header "${testRoot}/path with spaces/Probe.h")
set(pch "${header}.gch")
set(source "${testRoot}/Probe.c")
set(pchSource "${testRoot}/Probe.h.c")
file(WRITE "${header}" "#include <stdint.h>\n")
file(WRITE "${source}" "uint32_t Count(void) { return 3; }\n")
file(WRITE "${pchSource}" "/* Headers are supplied with -include. */\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
compile_probe("${pch}" "${pchSource}" -x c-header -include "${header}")
compile_probe("${testRoot}/ProbeC.o" "${source}" -include "${header}")
file(SHA256 "${pch}" firstPch)
compile_probe("${pch}" "${pchSource}" -x c-header -include "${header}")
file(SHA256 "${pch}" restoredPch)
compile_probe("${testRoot}/ProbeC.o" "${source}" -include "${header}")
file(READ "$ENV{CCACHE_STATSLOG}" stats)
string(REGEX MATCHALL "\ndirect_cache_hit\n" hits "${stats}")
list(LENGTH hits hitCount)
if(NOT firstPch STREQUAL restoredPch OR hitCount LESS 4 OR stats MATCHES "modified_input_file")
	message(FATAL_ERROR "Expected unchanged C PCH and direct hits for both languages:\n${stats}")
endif()

# A warm cache must still notice edits and refuse invalid source.
file(WRITE "${source}" "#error intentional compiler-cache-check failure\n")
execute_process(
	COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
		"${cache}" "${sourceRoot}" -- "${compiler}" -include "${header}"
		-MD -MF "${testRoot}/Probe.d" -o "${testRoot}/Probe.o" -c "${source}"
	RESULT_VARIABLE failure OUTPUT_VARIABLE standardOutput ERROR_VARIABLE standardError)
if(failure STREQUAL "0" OR NOT standardError MATCHES "intentional compiler-cache-check failure")
	message(FATAL_ERROR "Launcher hid a compilation failure: ${standardOutput}${standardError}")
endif()
message(STATUS "compiler-cache-check: argument boundaries, C/C++ PCH hits, unchanged bytes and errors verified")
