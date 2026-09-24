file(REMOVE_RECURSE "${BUNDLE}")
execute_process(
	COMMAND "${IMAGEGRAPH}" --input "${INPUT}" --output-id final --bundle "${BUNDLE}" --frames 0:2
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT error STREQUAL "")
	message(FATAL_ERROR "bundle export failed: ${result} ${error}")
endif()
if(NOT output MATCHES "format=png-sequence frames=3")
	message(FATAL_ERROR "bundle export did not report three frames: ${output}")
endif()
file(READ "${BUNDLE}/manifest.json" manifest)
if(NOT manifest MATCHES "atomic.imagegraph.sequence.v1" OR NOT manifest MATCHES "\\\"tick\\\":2")
	message(FATAL_ERROR "bundle manifest is incomplete: ${manifest}")
endif()
foreach(tick 00 01 02)
	if(NOT EXISTS "${BUNDLE}/frame.tick-000000000000000000${tick}.png")
		message(FATAL_ERROR "bundle is missing tick ${tick}")
	endif()
endforeach()
