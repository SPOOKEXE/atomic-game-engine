set(arguments
	--input "${INPUT}"
	--output-id final
	--output "${OUTPUT}")
if(DEFINED TICK)
	list(APPEND arguments --tick "${TICK}")
endif()
if(DEFINED FRAMES)
	list(APPEND arguments --frames "${FRAMES}")
endif()
execute_process(
	COMMAND "${IMAGEGRAPH}" ${arguments}
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error)
if(result EQUAL 0)
	message(FATAL_ERROR "imagegraph unexpectedly succeeded: ${output}${error}")
endif()
if(NOT error MATCHES "error status=${EXPECTED_STATUS}")
	message(FATAL_ERROR "expected ${EXPECTED_STATUS}, received: ${output}${error}")
endif()
