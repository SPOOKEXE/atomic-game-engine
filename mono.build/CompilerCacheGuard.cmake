# Fails the build when ccache is nearly full.
#
# A full ccache evicts entries to admit new ones, so builds slow down with no
# message: the cache that ran at 5 GiB here cleaned up 7011 times and hit 64% of
# compiles. Stopping the build makes the cause visible. Run as
# `cmake -P CompilerCacheGuard.cmake <ccache>` from the `mono_ccache_guard`
# target, which every compiled target depends on.
#
# Set MONO_CCACHE_FULL_OK=1 in the environment to build anyway.
if(CMAKE_ARGC LESS 4)
	message(FATAL_ERROR "CompilerCacheGuard.cmake expects: <ccache>")
endif()
if("$ENV{MONO_CCACHE_FULL_OK}" STREQUAL "1")
	return()
endif()

execute_process(COMMAND "${CMAKE_ARGV3}" --print-stats
	RESULT_VARIABLE status OUTPUT_VARIABLE stats ERROR_QUIET)
if(NOT status STREQUAL "0")
	return()
endif()
string(REGEX MATCH "(^|\n)cache_size_kibibyte\t([0-9]+)" _ "${stats}")
set(size "${CMAKE_MATCH_2}")
string(REGEX MATCH "(^|\n)max_cache_size_kibibyte\t([0-9]+)" _ "${stats}")
set(limit "${CMAKE_MATCH_2}")
if(NOT size OR NOT limit OR limit EQUAL 0)
	return()
endif()

# ccache trims to about 90% when it passes the limit, so 95% means it is
# already evicting or about to.
math(EXPR percent "${size} * 100 / ${limit}")
if(percent GREATER_EQUAL 95)
	math(EXPR sizeMiB "${size} / 1024")
	math(EXPR limitMiB "${limit} / 1024")
	message(FATAL_ERROR
		"ccache is ${percent}% full (${sizeMiB} of ${limitMiB} MiB) and evicting entries, "
		"which makes every build slower. Something is filling it. Look with `ccache -s`, then "
		"clear it with `ccache -C` or raise the limit with `ccache --max-size=<size>`. "
		"Set MONO_CCACHE_FULL_OK=1 to build anyway.")
endif()
