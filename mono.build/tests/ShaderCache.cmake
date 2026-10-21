# Exercise cache behaviour with a native compiler fixture and isolated files.
cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED MONO_TEST_ROOT)
	message(FATAL_ERROR "Set MONO_TEST_ROOT to a directory under the selected build tree")
endif()
get_filename_component(testRoot "${MONO_TEST_ROOT}/shader-cache-test" ABSOLUTE)
get_filename_component(cacheScript "${CMAKE_CURRENT_LIST_DIR}/../ShaderCache.cmake" ABSOLUTE)
file(REMOVE_RECURSE "${testRoot}")
set(fixture "${testRoot}/source with spaces")
file(MAKE_DIRECTORY "${fixture}")
file(WRITE "${fixture}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(ShaderCacheProbe LANGUAGES CXX)
add_executable(probe Probe.cpp)
target_compile_features(probe PRIVATE cxx_std_17)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/compiler-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:probe>")
]=])
file(WRITE "${fixture}/Probe.cpp" [=[
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>

int main(int argc, char **argv) {
	std::string include, source, output, flags;
	bool fail = false;
	for (int index = 1; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "-I") include = argv[++index];
		else if (argument == "-o") output = argv[++index];
		else if (argument == "--fail") fail = true;
		else if (argument[0] == '-') flags += argument + "\n";
		else source = argument;
	}
	if (flags.find("--slow") != std::string::npos)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	std::ofstream(include + "/calls.txt", std::ios::app) << "compile\n";
	const bool race = flags.find("--race") != std::string::npos &&
		!std::filesystem::exists(include + "/started");
	if (race) {
		std::ofstream(include + "/started") << "started";
		while (!std::filesystem::exists(include + "/edited"))
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	std::ofstream destination(output, std::ios::binary);
	destination << "tool-one\n" << flags;
	destination << std::ifstream(source).rdbuf();
	destination << std::ifstream(include + "/Common.glsl").rdbuf();
	if (race) {
		std::ofstream(include + "/read") << "read";
		while (!std::filesystem::exists(include + "/reverted"))
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (flags.find("--unstable") != std::string::npos)
		std::ofstream(include + "/Common.glsl", std::ios::app) << "changed\n";
	return fail ? 1 : 0;
}
]=])

function(run_fixture)
	execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT status STREQUAL "0")
		message(FATAL_ERROR "Fixture command failed (${status}): ${output}${error}")
	endif()
endfunction()
run_fixture("${CMAKE_COMMAND}" -S "${fixture}" -B "${testRoot}/tool" -G Ninja -DCMAKE_BUILD_TYPE=Release)
run_fixture("${CMAKE_COMMAND}" --build "${testRoot}/tool" --config Release)
file(READ "${testRoot}/tool/compiler-Release.txt" compiler)
set(source "${fixture}/Main.frag")
set(header "${fixture}/Common.glsl")
set(headers "${header}")
set(cache "${testRoot}/cache with spaces")
set(output "${testRoot}/first preset/Main.frag.spv")
set(flags "-O;-DVALUE=one two")
file(WRITE "${source}" "source one\n")
file(WRITE "${header}" "header one\n")

function(compile_shader expectedCalls)
	execute_process(COMMAND "${CMAKE_COMMAND}"
		"-DMONO_SHADER_CACHE=${cache}" "-DMONO_SHADER_COMPILER=${compiler}"
		"-DMONO_SHADER_SOURCE=${source}" "-DMONO_SHADER_OUTPUT=${output}"
		"-DMONO_SHADER_INCLUDE=${fixture}" "-DMONO_SHADER_FLAGS=${flags}"
		"-DMONO_SHADER_HEADERS=${headers}" -P "${cacheScript}"
		RESULT_VARIABLE status OUTPUT_VARIABLE standardOutput ERROR_VARIABLE error)
	if(ARGN STREQUAL "fail")
		if(status STREQUAL "0" OR NOT error MATCHES "Shader (compilation failed|inputs changed)")
			message(FATAL_ERROR "Expected compiler failure: ${standardOutput}${error}")
		endif()
	elseif(NOT status STREQUAL "0")
		message(FATAL_ERROR "Shader compile failed: ${standardOutput}${error}")
	endif()
	file(STRINGS "${fixture}/calls.txt" calls)
	list(LENGTH calls callCount)
	if(NOT callCount EQUAL expectedCalls)
		message(FATAL_ERROR "Expected ${expectedCalls} compiler calls, got ${callCount}")
	endif()
endfunction()

compile_shader(1)
file(READ "${output}" firstBytes)
set(expected "tool-one\n-O\n-DVALUE=one two\nsource one\nheader one\n")
if(NOT firstBytes STREQUAL expected)
	message(FATAL_ERROR "Compiler argument boundaries or output changed: ${firstBytes}")
endif()
compile_shader(1)
set(output "${testRoot}/second preset/Main.frag.spv")
compile_shader(1)
# Identical compiler binaries in different preset trees share their cache.
set(originalCompiler "${compiler}")
get_filename_component(compilerName "${compiler}" NAME)
file(MAKE_DIRECTORY "${testRoot}/other tool")
set(compiler "${testRoot}/other tool/${compilerName}")
file(COPY_FILE "${originalCompiler}" "${compiler}")
file(CHMOD "${compiler}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
compile_shader(1)
set(compiler "${originalCompiler}")
file(READ "${output}" restoredBytes)
if(NOT restoredBytes STREQUAL firstBytes)
	message(FATAL_ERROR "Restored bytes differ across presets")
endif()

# A timestamp-only checkout must hit and refresh an already present output.
file(TIMESTAMP "${output}" before "%s")
run_fixture("${CMAKE_COMMAND}" -E sleep 1)
file(TOUCH "${source}" "${header}")
compile_shader(1)
file(TIMESTAMP "${output}" after "%s")
if(NOT after GREATER before)
	message(FATAL_ERROR "Cache hit did not refresh the output timestamp")
endif()

file(WRITE "${source}" "source two\n")
compile_shader(2)
file(WRITE "${header}" "header two\n")
compile_shader(3)
set(flags "-O;-DVALUE=changed")
compile_shader(4)

# Change executable bytes at the same path, without trusting its timestamp.
file(READ "${fixture}/Probe.cpp" compilerSource)
string(REPLACE "tool-one" "tool-two" compilerSource "${compilerSource}")
file(WRITE "${fixture}/Probe.cpp" "${compilerSource}")
run_fixture("${CMAKE_COMMAND}" --build "${testRoot}/tool" --config Release)
compile_shader(5)
file(READ "${output}" toolBytes)
if(NOT toolBytes MATCHES "^tool-two")
	message(FATAL_ERROR "Changed compiler did not reach output")
endif()

file(GLOB_RECURSE cacheEntries "${cache}/*.spv")
foreach(entry IN LISTS cacheEntries)
	file(WRITE "${entry}" "corrupt")
endforeach()
compile_shader(6)
file(READ "${output}" repairedBytes)
if(NOT repairedBytes STREQUAL toolBytes)
	message(FATAL_ERROR "Corrupt cache was not repaired")
endif()

set(savedFlags "${flags}")
list(APPEND flags --fail)
compile_shader(7 fail)
compile_shader(8 fail)
file(READ "${output}" afterFailure)
if(NOT afterFailure STREQUAL repairedBytes)
	message(FATAL_ERROR "Failed compiler overwrote the previous output")
endif()
set(flags "${savedFlags}")
compile_shader(8)

# Header names and list membership matter even if header contents are identical.
file(COPY_FILE "${header}" "${fixture}/Other.glsl")
list(APPEND headers "${fixture}/Other.glsl")
compile_shader(9)
file(RENAME "${fixture}/Other.glsl" "${fixture}/Renamed.glsl")
set(headers "${header};${fixture}/Renamed.glsl")
compile_shader(10)

foreach(index RANGE 1 6)
	set(flags "-DVARIANT=${index}")
	math(EXPR expectedCalls "10 + ${index}")
	compile_shader(${expectedCalls})
endforeach()
file(GLOB_RECURSE cacheEntries "${cache}/*.spv")
list(LENGTH cacheEntries retainedCount)
if(NOT retainedCount EQUAL 4)
	message(FATAL_ERROR "Expected four retained variants, got ${retainedCount}")
endif()
compile_shader(16)

# Separate build processes must share one miss without publishing partial bytes.
set(flags --slow)
set(sharedCommand "${CMAKE_COMMAND}"
	"-DMONO_SHADER_CACHE=${cache}" "-DMONO_SHADER_COMPILER=${compiler}"
	"-DMONO_SHADER_SOURCE=${source}" "-DMONO_SHADER_INCLUDE=${fixture}"
	"-DMONO_SHADER_FLAGS=${flags}")
execute_process(
	COMMAND ${sharedCommand} "-DMONO_SHADER_HEADERS=${headers}"
		"-DMONO_SHADER_OUTPUT=${output}" -P "${cacheScript}"
	COMMAND ${sharedCommand} "-DMONO_SHADER_HEADERS=${headers}"
		"-DMONO_SHADER_OUTPUT=${testRoot}/parallel preset/Main.frag.spv" -P "${cacheScript}"
	RESULTS_VARIABLE parallelStatuses OUTPUT_VARIABLE parallelOutput ERROR_VARIABLE parallelError)
if(NOT parallelStatuses STREQUAL "0;0")
	message(FATAL_ERROR "Parallel cache requests failed: ${parallelStatuses}: ${parallelOutput}${parallelError}")
endif()
compile_shader(17)
file(SHA256 "${output}" firstParallelHash)
file(SHA256 "${testRoot}/parallel preset/Main.frag.spv" secondParallelHash)
if(NOT firstParallelHash STREQUAL secondParallelHash)
	message(FATAL_ERROR "Parallel presets received different output bytes")
endif()

# Force an edit and revert while the compiler consumes transient input bytes.
# Content matches before and after; only the stability stamps reveal the race.
file(WRITE "${testRoot}/EditDuringCompile.cmake" [=[
function(await_signal name)
	foreach(attempt RANGE 1 500)
		if(EXISTS "${FIXTURE}/${name}")
			return()
		endif()
		execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.01)
	endforeach()
	message(FATAL_ERROR "Timed out waiting for compiler signal ${name}")
endfunction()
await_signal(started)
file(WRITE "${FIXTURE}/Main.frag" "transient source\n")
file(WRITE "${FIXTURE}/edited" "")
await_signal(read)
file(WRITE "${FIXTURE}/Main.frag" "source two\n")
file(WRITE "${FIXTURE}/reverted" "")
]=])
set(flags --race)
execute_process(
	COMMAND "${CMAKE_COMMAND}"
		"-DMONO_SHADER_CACHE=${cache}" "-DMONO_SHADER_COMPILER=${compiler}"
		"-DMONO_SHADER_SOURCE=${source}" "-DMONO_SHADER_INCLUDE=${fixture}"
		"-DMONO_SHADER_FLAGS=${flags}" "-DMONO_SHADER_HEADERS=${headers}"
		"-DMONO_SHADER_OUTPUT=${output}" -P "${cacheScript}"
	COMMAND "${CMAKE_COMMAND}" "-DFIXTURE=${fixture}" -P "${testRoot}/EditDuringCompile.cmake"
	RESULTS_VARIABLE raceStatuses OUTPUT_VARIABLE raceOutput ERROR_VARIABLE raceError TIMEOUT 30)
if(NOT raceStatuses STREQUAL "0;0")
	message(FATAL_ERROR "Input race fixture failed: ${raceStatuses}: ${raceOutput}${raceError}")
endif()
compile_shader(19)
file(READ "${output}" raceBytes)
if(NOT raceBytes STREQUAL "tool-two\n--race\nsource two\nheader two\n")
	message(FATAL_ERROR "Transient input poisoned the cache: ${raceBytes}")
endif()
set(flags --unstable)
compile_shader(22 fail)
file(READ "${output}" unstableBytes)
if(NOT unstableBytes STREQUAL raceBytes)
	message(FATAL_ERROR "Unstable compilation overwrote previous output")
endif()
file(WRITE "${header}" "header two\n")
set(flags --race)
compile_shader(22)
message(STATUS "shader-cache-check: hits, parity, invalidation, corruption, errors, retention, locking and input races verified")
