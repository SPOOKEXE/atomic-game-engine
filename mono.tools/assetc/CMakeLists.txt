# Filesystem wrapper around the testable bake library; shared-tier tooling only.
mono_add_library(assetc
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::core
		Engine::assets
		Engine::bake
		Engine::imagegraph
		Engine::imagegraphexport
		Engine::parallel
	VENDOR
		Vendor::json
)

add_executable(assetc app/main.cpp)
target_link_libraries(assetc PRIVATE Tool::assetc Engine::core Engine::assets Engine::bake)
target_compile_options(assetc PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(assetc PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")

if(MONO_BUILD_TESTS)
	add_dependencies(test_assetc assetc)
	target_compile_definitions(test_assetc PRIVATE ASSETC_TEST_EXECUTABLE="$<TARGET_FILE:assetc>")
	add_test(
		NAME assetc_cli_source_arguments
		COMMAND "${CMAKE_COMMAND}"
			-DASSETC=$<TARGET_FILE:assetc>
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/argument.graph
			-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/source-arguments
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/SourceArgumentsCli.cmake")
	set_tests_properties(assetc_cli_source_arguments PROPERTIES TIMEOUT 180)
	add_test(
		NAME assetc_cli_rigid_observations
		COMMAND "${CMAKE_COMMAND}"
			-DASSETC=$<TARGET_FILE:assetc>
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rigid.graph
			-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/rigid
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RigidCli.cmake")
	add_test(
		NAME assetc_cli_builtin_random_observations
		COMMAND "${CMAKE_COMMAND}"
			-DASSETC=$<TARGET_FILE:assetc>
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/palette-random.graph
			-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-tests/builtin-random
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuiltinRandomCli.cmake")
	set_tests_properties(assetc_cli_builtin_random_observations PROPERTIES TIMEOUT 180)
endif()
