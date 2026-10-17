# L8 shared. Integer reference weather and accepted timeline values carry no device state.
mono_add_library(weather
	TIER shared
	DEPS Engine::core Engine::ecs Engine::parallel Engine::scene
)

option(MONO_FUZZ_WEATHER "Build the weather checkpoint parser with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_WEATHER)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_WEATHER requires Clang with libFuzzer")
	endif()
	# Compile the decoded boundary directly so sanitizer and coverage checks reach it.
	add_executable(fuzz_weather_checkpoint fuzz/Checkpoint.cpp src/Checkpoint.cpp src/Simulation.cpp)
	target_include_directories(fuzz_weather_checkpoint PRIVATE include)
	target_link_libraries(fuzz_weather_checkpoint PRIVATE Engine::weather)
	target_compile_options(fuzz_weather_checkpoint PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	target_link_options(fuzz_weather_checkpoint PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_weather_checkpoint PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
endif()
