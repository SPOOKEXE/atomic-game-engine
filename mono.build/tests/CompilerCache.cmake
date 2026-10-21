# Exercise Clang and GCC PCH regeneration through the real launcher and isolated caches.
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

find_program(cache NAMES ccache)
if(NOT cache)
	message(STATUS "compiler-cache-check: fixtures skipped (requires ccache)")
	return()
endif()
if(NOT DEFINED MONO_TEST_ROOT)
	message(FATAL_ERROR "Set MONO_TEST_ROOT to a directory under the selected build tree")
endif()

get_filename_component(sourceRoot "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(testRoot "${MONO_TEST_ROOT}/compiler-cache-test" ABSOLUTE)
file(MAKE_DIRECTORY "${testRoot}/path with spaces")

# The launcher must retain argument boundaries with either capability marker.
foreach(namespaceOption IN ITEMS "" "--namespace")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
			"${CMAKE_COMMAND}" "${sourceRoot}" ${namespaceOption} --
			"-DMONO_CACHE_ARGUMENT_PROBE=${testRoot}/arguments.txt"
			-P "${CMAKE_CURRENT_LIST_FILE}" --
			"one\\" "two" "[first" "second]" "a;b" "a\"b" "\${literal}" ""
		RESULT_VARIABLE argumentStatus)
	file(READ "${testRoot}/arguments.txt" arguments)
	set(expectedArguments "4:one\\\n3:two\n6:[first\n7:second]\n3:a;b\n3:a\"b\n10:\${literal}\n0:\n")
	if(NOT argumentStatus STREQUAL "0" OR NOT arguments STREQUAL expectedArguments)
		message(FATAL_ERROR "Launcher changed arguments:\n${arguments}")
	endif()
endforeach()

# Clang PCH bytes must survive a checkout that changes only header mtimes.
# A real same-size content edit must still invalidate the PCH and reach the consumer.
function(check_clang_pch compiler standard suffix pchLanguage assertion)
	set(clangRoot "${testRoot}/clang-${suffix}")
	file(MAKE_DIRECTORY "${clangRoot}/path with spaces")
	file(REMOVE_RECURSE "${clangRoot}/cache")
	set(ENV{CCACHE_DIR} "${clangRoot}/cache")
	set(ENV{CCACHE_STATSLOG} "${clangRoot}/stats.log")
	set(header "${clangRoot}/path with spaces/Probe.${suffix}")
	set(pch "${clangRoot}/Probe.pch")
	if(suffix STREQUAL "hpp")
		set(source "${clangRoot}/Probe.cpp")
	else()
		set(source "${clangRoot}/Probe.c")
	endif()
	set(pchSource "${clangRoot}/PchInput.${suffix}")
	set(object "${clangRoot}/Probe.o")
	file(WRITE "${header}" "#define CACHE_VALUE 1\n")
	file(WRITE "${source}" "${assertion}(CACHE_VALUE == 1, \"changed header reached consumer\");\n")
	file(WRITE "${pchSource}" "/* Header is supplied with -include. */\n")
	file(REMOVE "$ENV{CCACHE_STATSLOG}")
	# ccache rejects inputs newer than its invocation on coarse filesystems.
	execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)

	function(compile_clang output input)
		execute_process(
			COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
				"${cache}" "${sourceRoot}" -- "${compiler}" "-std=${standard}" ${ARGN}
				-MD -MF "${clangRoot}/Probe.d" -o "${output}" -c "${input}"
			RESULT_VARIABLE compileStatus OUTPUT_VARIABLE standardOutput ERROR_VARIABLE standardError)
		if(NOT compileStatus STREQUAL "0")
			message(FATAL_ERROR "Clang ${suffix} compile failed: ${standardOutput}${standardError}")
		endif()
	endfunction()

	compile_clang("${pch}" "${pchSource}"
		-Xclang -fno-pch-timestamp -Xclang -emit-pch
		-Xclang -include -Xclang "${header}" -x "${pchLanguage}")
	compile_clang("${object}" "${source}"
		-Xclang -include-pch -Xclang "${pch}" -Xclang -include -Xclang "${header}")
	file(SHA256 "${pch}" firstPch)
	file(READ "$ENV{CCACHE_STATSLOG}" stats)
	string(REGEX MATCHALL "\ncache_miss\n" misses "${stats}")
	list(LENGTH misses missCount)
	if(missCount LESS 2)
		message(FATAL_ERROR "Expected cold Clang ${suffix} PCH and consumer misses:\n${stats}")
	endif()

	execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
	file(TOUCH "${header}")
	execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
	file(REMOVE "${pch}" "${object}")
	compile_clang("${pch}" "${pchSource}"
		-Xclang -fno-pch-timestamp -Xclang -emit-pch
		-Xclang -include -Xclang "${header}" -x "${pchLanguage}")
	compile_clang("${object}" "${source}"
		-Xclang -include-pch -Xclang "${pch}" -Xclang -include -Xclang "${header}")
	file(SHA256 "${pch}" restoredPch)
	file(READ "$ENV{CCACHE_STATSLOG}" stats)
	string(REGEX MATCHALL "\ndirect_cache_hit\n" hits "${stats}")
	list(LENGTH hits hitCount)
	if(NOT firstPch STREQUAL restoredPch OR hitCount LESS 2)
		message(FATAL_ERROR "Expected unchanged Clang ${suffix} PCH and two direct hits:\n${stats}")
	endif()

	execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
	file(WRITE "${header}" "#define CACHE_VALUE 2\n")
	file(REMOVE "${pch}" "${object}")
	execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
	compile_clang("${pch}" "${pchSource}"
		-Xclang -fno-pch-timestamp -Xclang -emit-pch
		-Xclang -include -Xclang "${header}" -x "${pchLanguage}")
	file(READ "$ENV{CCACHE_STATSLOG}" stats)
	string(REGEX MATCHALL "\ncache_miss\n" misses "${stats}")
	list(LENGTH misses missCount)
	if(missCount LESS 3)
		message(FATAL_ERROR "Expected Clang ${suffix} PCH recompilation after a header edit:\n${stats}")
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
			"${cache}" "${sourceRoot}" -- "${compiler}" "-std=${standard}"
			-Xclang -include-pch -Xclang "${pch}" -Xclang -include -Xclang "${header}"
			-MD -MF "${clangRoot}/Probe.d" -o "${object}" -c "${source}"
		RESULT_VARIABLE failure OUTPUT_VARIABLE standardOutput ERROR_VARIABLE standardError)
	if(failure STREQUAL "0" OR NOT standardError MATCHES "changed header reached consumer")
		message(FATAL_ERROR "Clang ${suffix} hid a header edit: ${standardOutput}${standardError}")
	endif()
	message(STATUS "compiler-cache-check: Clang ${suffix} PCH mtime hits and header edits verified")
endfunction()

find_program(clangCpp NAMES clang++-21 clang++-20 clang++-19 clang++)
find_program(clangC NAMES clang-21 clang-20 clang-19 clang)
if(clangCpp)
	check_clang_pch("${clangCpp}" c++20 hpp c++-header static_assert)
else()
	message(STATUS "compiler-cache-check: Clang C++ fixture skipped (clang++ unavailable)")
endif()
if(clangC)
	check_clang_pch("${clangC}" c17 h c-header _Static_assert)
else()
	message(STATUS "compiler-cache-check: Clang C fixture skipped (clang unavailable)")
endif()

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
	message(STATUS "compiler-cache-check: GCC/Linux fixture skipped on this platform")
	return()
endif()
find_program(compiler NAMES g++)
if(NOT compiler)
	message(STATUS "compiler-cache-check: GCC fixture skipped (g++ unavailable)")
	return()
endif()

# No PCH: identical sources in two checkouts must not restore stale DWARF paths.
execute_process(COMMAND "${cache}" --version OUTPUT_VARIABLE cacheVersion)
set(namespaceOption "")
if(cacheVersion MATCHES "ccache version ([0-9]+\\.[0-9]+(\\.[0-9]+)?)" AND
	CMAKE_MATCH_1 VERSION_GREATER_EQUAL "4.5")
	set(namespaceOption --namespace)
endif()
set(scopeRoot "${testRoot}/workspace-scope")
file(REMOVE_RECURSE "${scopeRoot}")
foreach(workspace IN ITEMS checkout-a checkout-b)
	file(MAKE_DIRECTORY "${scopeRoot}/${workspace}/preset-one" "${scopeRoot}/${workspace}/preset-two")
	file(WRITE "${scopeRoot}/${workspace}/Probe.cpp"
		"struct Payload { int Value; };\nint Value() { Payload payload{41}; return payload.Value; }\n")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
set(ENV{CCACHE_DIR} "${scopeRoot}/cache")
set(ENV{CCACHE_STATSLOG} "${scopeRoot}/stats.log")
set(originalNamespace "$ENV{CCACHE_NAMESPACE}")
set(ENV{CCACHE_NAMESPACE} "fixture-caller")
# Seed an entry with the old unscoped launcher settings. New keys must reject it.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
	"CCACHE_BASEDIR=${scopeRoot}/checkout-a" CCACHE_NOHASHDIR=true CCACHE_DEPEND=true
	"${cache}" "${compiler}" -g -O0 -MD -MF Probe.d -o Probe.o
	-c "${scopeRoot}/checkout-a/Probe.cpp"
	WORKING_DIRECTORY "${scopeRoot}/checkout-a/preset-one"
	RESULT_VARIABLE seedStatus OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT seedStatus STREQUAL "0")
	message(FATAL_ERROR "Unscoped cache seed failed: ${output}${error}")
endif()
function(compile_workspace workspace preset)
	execute_process(COMMAND "${CMAKE_COMMAND}" -P "${sourceRoot}/mono.build/CompilerCache.cmake"
		"${cache}" "${scopeRoot}/${workspace}" ${namespaceOption} -- "${compiler}"
		-g -O0 -MD -MF Probe.d -o Probe.o -c "${scopeRoot}/${workspace}/Probe.cpp"
		WORKING_DIRECTORY "${scopeRoot}/${workspace}/${preset}"
		RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT status STREQUAL "0")
		message(FATAL_ERROR "Workspace compile failed: ${output}${error}")
	endif()
endfunction()
compile_workspace(checkout-a preset-one)
compile_workspace(checkout-b preset-one)
file(READ "$ENV{CCACHE_STATSLOG}" scopeStats)
string(REGEX MATCHALL "\ncache_miss\n" scopeMisses "${scopeStats}")
list(LENGTH scopeMisses scopeMissCount)
if(NOT scopeMissCount EQUAL 3)
	message(FATAL_ERROR "Different source roots or old unscoped entries shared a cache entry:\n${scopeStats}")
endif()
compile_workspace(checkout-a preset-two)
file(READ "$ENV{CCACHE_STATSLOG}" scopeStats)
if(namespaceOption AND NOT scopeStats MATCHES "\ndirect_cache_hit\n")
	message(FATAL_ERROR "One source root did not share across preset directories:\n${scopeStats}")
endif()
find_program(scopeReadelf NAMES readelf llvm-readelf)
if(scopeReadelf)
	execute_process(COMMAND "${scopeReadelf}" --debug-dump=info
		"${scopeRoot}/checkout-b/preset-one/Probe.o"
		RESULT_VARIABLE status OUTPUT_VARIABLE debugInfo ERROR_VARIABLE error)
	if(NOT status STREQUAL "0" OR NOT debugInfo MATCHES "checkout-b" OR debugInfo MATCHES "checkout-a")
		message(FATAL_ERROR "Debug information names the wrong checkout:\n${debugInfo}${error}")
	endif()
endif()

# Exercise the old-version path even when the installed ccache has namespaces.
set(namespaceOption "")
set(ENV{CCACHE_DIR} "${scopeRoot}/fallback-cache")
file(REMOVE "$ENV{CCACHE_STATSLOG}")
compile_workspace(checkout-a preset-one)
compile_workspace(checkout-b preset-one)
file(READ "$ENV{CCACHE_STATSLOG}" scopeStats)
string(REGEX MATCHALL "\ncache_miss\n" scopeMisses "${scopeStats}")
list(LENGTH scopeMisses scopeMissCount)
if(NOT scopeMissCount EQUAL 2)
	message(FATAL_ERROR "Fallback cwd hashing did not isolate source roots:\n${scopeStats}")
endif()
set(ENV{CCACHE_NAMESPACE} "${originalNamespace}")
message(STATUS "compiler-cache-check: source-root isolation, debug paths, cross-preset reuse and fallback verified")

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
