# Compile and run the real batching helper at its source-count boundary.
if(NOT DEFINED MONO_TEST_ROOT)
	message(FATAL_ERROR "Set MONO_TEST_ROOT to a directory under the selected build tree")
endif()
get_filename_component(sourceRoot "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(testRoot "${MONO_TEST_ROOT}/header-batching-test" ABSOLUTE)
set(fixture "${testRoot}/source with spaces")
file(MAKE_DIRECTORY "${fixture}")
file(WRITE "${fixture}/value.hpp" [=[
#pragma once
#include <array>
#include <numeric>
inline int Value() {
	const std::array<int, 3> values{1, 2, 3};
	return std::accumulate(values.begin(), values.end(), 0);
}
]=])
file(WRITE "${fixture}/one.cpp" "#include \"value.hpp\"\nint One() { return Value(); }\n")
file(WRITE "${fixture}/two.cpp" "#include \"value.hpp\"\nint Two() { return Value() * 2; }\n")
file(WRITE "${fixture}/three.cpp" "#include \"value.hpp\"\nint Three() { return Value() * 3; }\n")
file(WRITE "${fixture}/main.cpp" [=[
int One();
int Two();
int Three();
int main() { return One() == 6 && Two() == 12 && Three() == 18 ? 0 : 1; }
]=])
file(WRITE "${fixture}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(HeaderBatchingProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include("${MONO_SOURCE_ROOT}/mono.build/MonoLibrary.cmake")
set(MONO_UNITY_BATCH 8)
foreach(count RANGE 1 3)
	set(sources "${CMAKE_CURRENT_SOURCE_DIR}/one.cpp")
	if(count GREATER 1)
		list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/two.cpp")
	endif()
	if(count GREATER 2)
		list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/three.cpp")
	endif()
	add_library(batch${count} STATIC ${sources})
	_mono_batch_headers(batch${count} "${sources}" TRUE)
	get_target_property(pch batch${count} PRECOMPILE_HEADERS)
	get_target_property(unity batch${count} UNITY_BUILD)
	if(MONO_UNITY_BUILD)
		if(NOT unity OR pch)
			message(FATAL_ERROR "Unity did not take precedence for ${count} sources")
		endif()
	elseif(MONO_PCH AND count GREATER 2)
		if(NOT pch)
			message(FATAL_ERROR "PCH missing for ${count} sources")
		endif()
	elseif(pch)
		message(FATAL_ERROR "Unwanted PCH for ${count} sources")
	endif()
endforeach()
add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE batch3)

# Test and benchmark callers disallow unity even when the preset requests it.
set(sources "${CMAKE_CURRENT_SOURCE_DIR}/one.cpp;${CMAKE_CURRENT_SOURCE_DIR}/two.cpp;${CMAKE_CURRENT_SOURCE_DIR}/three.cpp")
add_library(nonunity STATIC ${sources})
_mono_batch_headers(nonunity "${sources}" FALSE)
get_target_property(pch nonunity PRECOMPILE_HEADERS)
get_target_property(unity nonunity UNITY_BUILD)
if(unity OR (MONO_PCH AND NOT pch) OR (NOT MONO_PCH AND pch))
	message(FATAL_ERROR "Unity exclusion or MONO_PCH override ignored")
endif()
]=])

function(check_batching name)
	set(probeBuild "${testRoot}/${name}")
	execute_process(COMMAND "${CMAKE_COMMAND}" -S "${fixture}" -B "${probeBuild}" -G Ninja
		"-DMONO_SOURCE_ROOT=${sourceRoot}" ${ARGN}
		RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT status STREQUAL "0")
		message(FATAL_ERROR "${name} configure failed:\n${output}${error}")
	endif()
	execute_process(COMMAND "${CMAKE_COMMAND}" --build "${probeBuild}" --parallel 2
		RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT status STREQUAL "0")
		message(FATAL_ERROR "${name} compile failed:\n${output}${error}")
	endif()
	if(NOT CMAKE_HOST_WIN32)
		execute_process(COMMAND "${probeBuild}/probe" RESULT_VARIABLE status)
		if(NOT status STREQUAL "0")
			message(FATAL_ERROR "${name} header semantics failed: ${status}")
		endif()
	endif()
endfunction()

check_batching(pch -DMONO_PCH=ON -DMONO_UNITY_BUILD=OFF)
check_batching(plain -DMONO_PCH=OFF -DMONO_UNITY_BUILD=OFF)
check_batching(unity -DMONO_PCH=ON -DMONO_UNITY_BUILD=ON)
check_batching(unity-no-pch -DMONO_PCH=OFF -DMONO_UNITY_BUILD=ON)
message(STATUS "header-batching-check: small targets, PCH, unity and overrides verified")
