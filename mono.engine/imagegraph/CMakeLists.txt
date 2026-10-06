mono_add_library(imagegraph
	TIER shared
	DEPS
		Engine::core
		Engine::audiocodec
)

option(MONO_FUZZ_IMAGEGRAPH "Build native frame cache parsing with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_IMAGEGRAPH)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_IMAGEGRAPH requires Clang with libFuzzer")
	endif()
	# grug instrument actual packet parser; ordinary archive supplies its shared value helpers.
	add_executable(fuzz_imagegraph_frame_cache fuzz/FrameCache.cpp src/NativeFrameCacheReceipt.cpp)
	target_include_directories(fuzz_imagegraph_frame_cache PRIVATE include src)
	target_link_libraries(fuzz_imagegraph_frame_cache PRIVATE Engine::imagegraph)
	target_compile_options(fuzz_imagegraph_frame_cache PRIVATE
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	target_link_options(fuzz_imagegraph_frame_cache PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_imagegraph_frame_cache PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
endif()
