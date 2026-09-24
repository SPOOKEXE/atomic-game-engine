set(first "${OUTPUT_DIR}/repeat-a.png")
set(second "${OUTPUT_DIR}/repeat-b.png")
execute_process(
	COMMAND "${IMAGEGRAPH}" --input "${INPUT}" --output-id final --output "${first}"
	RESULT_VARIABLE first_result
	OUTPUT_VARIABLE first_output
	ERROR_VARIABLE first_error)
if(NOT first_result EQUAL 0)
	message(FATAL_ERROR "first imagegraph run failed: ${first_error}")
endif()
execute_process(
	COMMAND "${IMAGEGRAPH}" --input "${INPUT}" --output-id final --output "${second}"
	RESULT_VARIABLE second_result
	OUTPUT_VARIABLE second_output
	ERROR_VARIABLE second_error)
if(NOT second_result EQUAL 0)
	message(FATAL_ERROR "second imagegraph run failed: ${second_error}")
endif()
string(REGEX REPLACE " file=\"[^\"]+\"" "" first_record "${first_output}")
string(REGEX REPLACE " file=\"[^\"]+\"" "" second_record "${second_output}")
if(NOT first_record STREQUAL second_record)
	message(FATAL_ERROR "repeated imagegraph runs printed different result records")
endif()
file(SHA256 "${first}" first_hash)
file(SHA256 "${second}" second_hash)
if(NOT first_hash STREQUAL second_hash)
	message(FATAL_ERROR "repeated imagegraph runs wrote different PNG bytes")
endif()
