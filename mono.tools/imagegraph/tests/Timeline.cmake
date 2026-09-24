set(arguments
	--input "${INPUT}"
	--output-id final
	--output "${OUTPUT_DIR}/timeline.png"
	--frames 0:2)
execute_process(
	COMMAND "${IMAGEGRAPH}" ${arguments}
	RESULT_VARIABLE first_result
	OUTPUT_VARIABLE first_output
	ERROR_VARIABLE first_error)
if(NOT first_result EQUAL 0)
	message(FATAL_ERROR "first timeline render failed: ${first_error}")
endif()
set(frames
	"${OUTPUT_DIR}/timeline.tick-00000000000000000000.png"
	"${OUTPUT_DIR}/timeline.tick-00000000000000000001.png"
	"${OUTPUT_DIR}/timeline.tick-00000000000000000002.png")
set(first_hashes)
foreach(frame IN LISTS frames)
	if(NOT EXISTS "${frame}")
		message(FATAL_ERROR "timeline output is missing a frame: ${frame}")
	endif()
	file(SHA256 "${frame}" frame_hash)
	list(APPEND first_hashes "${frame_hash}")
endforeach()
execute_process(
	COMMAND "${IMAGEGRAPH}" ${arguments}
	RESULT_VARIABLE second_result
	OUTPUT_VARIABLE second_output
	ERROR_VARIABLE second_error)
if(NOT second_result EQUAL 0)
	message(FATAL_ERROR "second timeline render failed: ${second_error}")
endif()
if(NOT first_output STREQUAL second_output)
	message(FATAL_ERROR "repeated timeline renders printed different records")
endif()
set(frame_index 0)
foreach(frame IN LISTS frames)
	file(SHA256 "${frame}" second_hash)
	list(GET first_hashes ${frame_index} first_hash)
	if(NOT first_hash STREQUAL second_hash)
		message(FATAL_ERROR "repeated timeline render changed PNG bytes: ${frame}")
	endif()
	math(EXPR frame_index "${frame_index} + 1")
endforeach()
if(NOT first_output MATCHES "tick=0" OR NOT first_output MATCHES "tick=1" OR
   NOT first_output MATCHES "tick=2")
	message(FATAL_ERROR "timeline output did not report each tick: ${first_output}")
endif()
string(REGEX MATCH "hash=([^ ]+) tick=0" tick_zero_record "${first_output}")
set(tick_zero_hash "${CMAKE_MATCH_1}")
string(REGEX MATCH "hash=([^ ]+) tick=1" tick_one_record "${first_output}")
set(tick_one_hash "${CMAKE_MATCH_1}")
string(REGEX MATCH "hash=([^ ]+) tick=2" tick_two_record "${first_output}")
set(tick_two_hash "${CMAKE_MATCH_1}")
if(tick_zero_hash STREQUAL "" OR tick_one_hash STREQUAL "" OR tick_two_hash STREQUAL "")
	message(FATAL_ERROR "could not read per-tick hashes: ${first_output}")
endif()
if(NOT tick_zero_hash STREQUAL tick_one_hash OR tick_one_hash STREQUAL tick_two_hash)
	message(FATAL_ERROR "step keyframe hashes do not match expected interpolation behavior: ${first_output}")
endif()
execute_process(
	COMMAND "${IMAGEGRAPH}"
		--input "${INPUT}"
		--output-id final
		--output "${OUTPUT_DIR}/stepped.png"
		--frames 0:3:2
	RESULT_VARIABLE stepped_result
	OUTPUT_VARIABLE stepped_output
	ERROR_VARIABLE stepped_error)
if(NOT stepped_result EQUAL 0)
	message(FATAL_ERROR "stepped timeline render failed: ${stepped_error}")
endif()
if(NOT EXISTS "${OUTPUT_DIR}/stepped.tick-00000000000000000000.png" OR
   NOT EXISTS "${OUTPUT_DIR}/stepped.tick-00000000000000000002.png" OR
   EXISTS "${OUTPUT_DIR}/stepped.tick-00000000000000000001.png")
	message(FATAL_ERROR "stepped timeline did not write exactly the requested ticks")
endif()
if(NOT stepped_output MATCHES "tick=0" OR NOT stepped_output MATCHES "tick=2" OR
   stepped_output MATCHES "tick=1")
	message(FATAL_ERROR "stepped timeline reported unexpected ticks: ${stepped_output}")
endif()
