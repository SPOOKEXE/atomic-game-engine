# A device-free command line renderer over the engine-owned imagegraph API.
mono_add_library(imagegraph_runner
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::core
		Engine::imagegraph
)

add_executable(imagegraph app/main.cpp)
target_link_libraries(imagegraph PRIVATE Tool::imagegraph_runner Engine::core)
target_compile_options(imagegraph PRIVATE ${MONO_COMPILE_OPTIONS})
set_target_properties(imagegraph PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")

if(MONO_BUILD_TESTS)
	set(_imagegraph_test_output "${CMAKE_CURRENT_BINARY_DIR}/cli-tests")
	file(MAKE_DIRECTORY "${_imagegraph_test_output}")
	add_test(
		NAME imagegraph_cli_solid
		COMMAND $<TARGET_FILE:imagegraph>
			--input "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solid.graph"
			--output-id final
			--output "${_imagegraph_test_output}/solid.png")
	set_tests_properties(imagegraph_cli_solid PROPERTIES
		PASS_REGULAR_EXPRESSION "ok output_id=\"final\" width=2 height=1 format=rgba8 hash=0x4c71c14abed3596d")

	add_test(
		NAME imagegraph_cli_passthrough
		COMMAND $<TARGET_FILE:imagegraph>
			--input "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/passthrough.graph"
			--output-id final
			--output "${_imagegraph_test_output}/passthrough.png")
	set_tests_properties(imagegraph_cli_passthrough PROPERTIES
		PASS_REGULAR_EXPRESSION "ok output_id=\"final\" width=2 height=1 format=rgba8 hash=0x4c71c14abed3596d")

	add_test(
		NAME imagegraph_cli_typed_parameter
		COMMAND $<TARGET_FILE:imagegraph>
			--input "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solid.graph"
			--output-id final
			--output "${_imagegraph_test_output}/typed-parameter.png"
			--set "solid.colour=colour:1,2,3,255")
	set_tests_properties(imagegraph_cli_typed_parameter PROPERTIES
		PASS_REGULAR_EXPRESSION "ok output_id=\"final\" width=2 height=1 format=rgba8 hash=0x77096ba2893f9e11")

	add_test(
		NAME imagegraph_cli_array_manifest
		COMMAND $<TARGET_FILE:imagegraph>
			--input "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/image-array.graph"
			--output-id all
			--output "${_imagegraph_test_output}/array.json")
	set_tests_properties(imagegraph_cli_array_manifest PROPERTIES
		PASS_REGULAR_EXPRESSION "format=image-array images=2 tick=0")

	add_test(
		NAME imagegraph_cli_array_graph_budget
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/over-limit.graph
			-DOUTPUT=${_imagegraph_test_output}/over-limit-array.json
			-DEXPECTED_STATUS=LimitExceeded
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")

	add_test(
		NAME imagegraph_cli_deterministic_repeat
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solid.graph
			-DOUTPUT_DIR=${_imagegraph_test_output}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Deterministic.cmake")

	add_test(
		NAME imagegraph_cli_malformed
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/malformed.graph
			-DOUTPUT=${_imagegraph_test_output}/malformed.png
			-DEXPECTED_STATUS=Malformed
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")

	add_test(
		NAME imagegraph_cli_limits
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/over-limit.graph
			-DOUTPUT=${_imagegraph_test_output}/over-limit.png
			-DEXPECTED_STATUS=LimitExceeded
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")

	add_test(
		NAME imagegraph_cli_tick
		COMMAND $<TARGET_FILE:imagegraph>
			--input "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animated.graph"
			--output-id final
			--output "${_imagegraph_test_output}/tick.png"
			--tick 1)
	set_tests_properties(imagegraph_cli_tick PROPERTIES
		PASS_REGULAR_EXPRESSION "tick=1")

	add_test(
		NAME imagegraph_cli_frame_range
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animated.graph
			-DOUTPUT_DIR=${_imagegraph_test_output}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Timeline.cmake")

	add_test(
		NAME imagegraph_cli_bundle
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/animated.graph
			-DBUNDLE=${_imagegraph_test_output}/frame-bundle
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Bundle.cmake")

	add_test(
		NAME imagegraph_cli_tick_limit
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solid.graph
			-DOUTPUT=${_imagegraph_test_output}/tick-limit.png
			-DTICK=10000001
			-DEXPECTED_STATUS=LimitExceeded
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")

	add_test(
		NAME imagegraph_cli_frame_range_invalid
		COMMAND "${CMAKE_COMMAND}"
			-DIMAGEGRAPH=$<TARGET_FILE:imagegraph>
			-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/solid.graph
			-DOUTPUT=${_imagegraph_test_output}/frames-invalid.png
			-DFRAMES=0:2:0
			-DEXPECTED_STATUS=InvalidValue
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ExpectFailure.cmake")
endif()
