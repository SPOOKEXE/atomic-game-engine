# Compiler launcher for ccache. Arguments follow `--` without shell expansion.
# Objects carry no absolute paths (see `-ffile-prefix-map` in CMakeLists.txt),
# so every checkout and preset shares one cache.
set(separatorIndex 5)
math(EXPR compilerIndex "${separatorIndex} + 1")
if(CMAKE_ARGC LESS_EQUAL compilerIndex OR NOT CMAKE_ARGV${separatorIndex} STREQUAL "--")
	message(FATAL_ERROR "CompilerCache.cmake expects: <ccache> <source directory> -- <compiler> [arguments]")
endif()

set(ENV{CCACHE_SLOPPINESS} "pch_defines,time_macros")
set(ENV{CCACHE_BASEDIR} "${CMAKE_ARGV4}")
unset(ENV{CCACHE_HASHDIR})
set(ENV{CCACHE_NOHASHDIR} "true")
set(ENV{CCACHE_DEPEND} "true")

# Lists can merge arguments containing unmatched brackets or trailing backslashes.
# Quote each argument in the CMake call itself, preserving its exact boundary.
function(append_argument argument)
	string(REPLACE "\\" "\\\\" quoted "${argument}")
	string(REPLACE "\"" "\\\"" quoted "${quoted}")
	string(REPLACE "$" "\\$" quoted "${quoted}")
	set(cacheCommand "${cacheCommand} \"${quoted}\"" PARENT_SCOPE)
endfunction()

set(cacheCommand "execute_process(COMMAND")
append_argument("${CMAKE_ARGV3}")
append_argument("${CMAKE_ARGV${compilerIndex}}")
set(previousArgument "")
set(createsPch FALSE)
set(pchOutput "")
math(EXPR firstArgument "${compilerIndex} + 1")
if(CMAKE_ARGC GREATER firstArgument)
	math(EXPR lastArgument "${CMAKE_ARGC} - 1")
	foreach(index RANGE ${firstArgument} ${lastArgument})
		set(argument "${CMAKE_ARGV${index}}")
		if(previousArgument STREQUAL "-x" AND
			(argument STREQUAL "c++-header" OR argument STREQUAL "c-header"))
			set(createsPch TRUE)
		elseif(previousArgument STREQUAL "-o")
			set(pchOutput "${argument}")
		endif()
		set(previousArgument "${argument}")
		append_argument("${argument}")
	endforeach()
endif()

# GCC's -include finds the previous .gch while rebuilding that same output.
# ccache then hashes its own output as an input and rejects every regeneration.
# Ninja has already selected this compile, so discard only its stale PCH output.
if(createsPch AND pchOutput MATCHES "\\.gch$")
	file(REMOVE "${pchOutput}")
endif()

string(APPEND cacheCommand " RESULT_VARIABLE cacheStatus)")
cmake_language(EVAL CODE "${cacheCommand}")
if(NOT cacheStatus STREQUAL "0")
	message(FATAL_ERROR "Compiler cache command failed (${cacheStatus}).")
endif()
