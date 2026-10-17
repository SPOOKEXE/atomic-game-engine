# Selective microbenchmarks reuse Tool::testrunner discovery and its skip cache.
# Demo impact reports aggregate runtime observations and enforce cost ceilings.

if(MONO_BUILD_BENCH)
	add_executable(benchrunner app/main.cpp)
	target_link_libraries(benchrunner PRIVATE Tool::testrunner Engine::core Vendor::json)
	target_compile_options(benchrunner PRIVATE ${MONO_COMPILE_OPTIONS})

	set_target_properties(benchrunner PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
endif()

if(MONO_BUILD_TESTS AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
	mono_add_tests(benchrunner DEPS Tool::testrunner Engine::core)
	target_link_libraries(test_benchrunner PRIVATE Vendor::json)
	target_include_directories(test_benchrunner PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app")
endif()
