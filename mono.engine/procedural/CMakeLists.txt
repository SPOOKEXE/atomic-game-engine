# L3 shared. Pure canonical requests and bounded placement, without world state.
mono_add_library(procedural
	TIER shared
	DEPS Engine::core Engine::graphcore
)

option(MONO_FUZZ_PROCEDURAL "Build portable procedural parsers with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_PROCEDURAL)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_PROCEDURAL requires Clang with libFuzzer")
	endif()
	# Keep archive extraction separate from the instrumented parser definitions.
	set_target_properties(engine_procedural PROPERTIES UNITY_BUILD OFF)
	add_executable(fuzz_procedural_portable
		fuzz/Portable.cpp
		src/Canonical.cpp src/CanonicalDecode.cpp src/Artifact.cpp src/AuthoredInputs.cpp)
	target_include_directories(fuzz_procedural_portable PRIVATE include)
	target_link_libraries(fuzz_procedural_portable PRIVATE Engine::procedural)
	target_compile_options(fuzz_procedural_portable PRIVATE
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	target_link_options(fuzz_procedural_portable PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_procedural_portable PROPERTIES
		UNITY_BUILD OFF RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
endif()
