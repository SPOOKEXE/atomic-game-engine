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

	set(bench_selection_fixture_dir "${MONO_STAGE_ROOT}/tests/fixtures")
	file(MAKE_DIRECTORY "${bench_selection_fixture_dir}")
	add_executable(bench_selection_fixture
		"${CMAKE_SOURCE_DIR}/mono.build/benchmain/BenchMain.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/BenchCases.cpp")
	target_link_libraries(bench_selection_fixture PRIVATE Engine::suites)
	target_include_directories(bench_selection_fixture PRIVATE
		"${CMAKE_SOURCE_DIR}/mono.build/benchmain/include")
	target_compile_options(bench_selection_fixture PRIVATE ${MONO_COMPILE_OPTIONS})
	target_compile_definitions(test_benchrunner PRIVATE
		BENCH_SELECTION_FIXTURE_PATH="$<TARGET_FILE:bench_selection_fixture>")
	set_target_properties(bench_selection_fixture PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${bench_selection_fixture_dir}")
	add_dependencies(test_benchrunner bench_selection_fixture)
endif()
