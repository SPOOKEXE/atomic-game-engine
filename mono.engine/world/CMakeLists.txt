# L4. Universe and worlds: who owns which store, and when each one ticks.
#
# No vendor and no product. This module holds simulation *structure* - a world
# is a store plus a scheduler plus a clock, and a universe is the set of them -
# so both programs run worlds out of the same code and neither owns any of it.
#
# Deliberately does not depend on: render, input, or anything client-tier. A
# world that could reach a renderer would be a world that cannot run headless,
# and running headless is what the server is.
mono_add_library(world
	TIER shared
	DEPS
		Engine::core
		Engine::parallel
		Engine::ecs
)

option(MONO_FUZZ_PRESENTATION "Build the presentation message parser with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_PRESENTATION)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_PRESENTATION requires Clang with libFuzzer")
	endif()
	# Instrument the actual parser, not only a harness linked to the ordinary archive.
	add_executable(fuzz_presentation fuzz/PresentationBus.cpp src/PresentationBus.cpp)
	target_include_directories(fuzz_presentation PRIVATE include)
	target_link_libraries(fuzz_presentation PRIVATE Engine::core)
	target_compile_options(fuzz_presentation PRIVATE
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	target_link_options(fuzz_presentation PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_presentation PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
endif()
