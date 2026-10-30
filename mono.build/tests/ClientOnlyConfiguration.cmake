# Configure the real client-only product without building or running it.
if(NOT DEFINED MONO_TEST_ROOT)
	message(FATAL_ERROR "Set MONO_TEST_ROOT to a directory under the selected build tree")
endif()
get_filename_component(sourceRoot "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(testRoot "${MONO_TEST_ROOT}/client-only-configuration-test" ABSOLUTE)

execute_process(
	COMMAND "${CMAKE_COMMAND}" -S "${sourceRoot}" -B "${testRoot}" -G Ninja
		-DMONO_BUILD_CLIENT=ON -DMONO_BUILD_SERVER=OFF -DMONO_BUILD_CDN=OFF
		-DMONO_BUILD_TESTS=OFF -DMONO_BUILD_BENCH=OFF -DMONO_TRACY=OFF
	RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT status STREQUAL "0")
	message(FATAL_ERROR "Client-only configuration failed:\n${output}${error}")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}" --build "${testRoot}" --target help
	RESULT_VARIABLE status OUTPUT_VARIABLE targets ERROR_VARIABLE error
)
if(NOT status STREQUAL "0")
	message(FATAL_ERROR "Client-only target listing failed:\n${error}")
endif()
string(REPLACE "\r\n" "\n" targets "${targets}")
string(PREPEND targets "\n")
foreach(required IN ITEMS cdn_lib contentimport)
	string(FIND "${targets}" "\n${required}: phony\n" found)
	if(found EQUAL -1)
		message(FATAL_ERROR "Client-only configuration lacks ${required}")
	endif()
endforeach()
string(FIND "${targets}" "\ncdn: phony\n" cdnProgram)
if(NOT cdnProgram EQUAL -1)
	message(FATAL_ERROR "Client-only configuration unexpectedly stages the CDN executable")
endif()
message(STATUS "client-only-configuration-check: publishing tool and CDN library are configured without the CDN program")
