# Native Linux links can use LLD without changing the compiler or object files.
include_guard(GLOBAL)

set(MONO_LINKER "auto" CACHE STRING "First-party linker: auto, system or lld")
set_property(CACHE MONO_LINKER PROPERTY STRINGS auto system lld)
set(MONO_LINK_JOBS "2" CACHE STRING "Maximum concurrent first-party Ninja links")
option(MONO_THIN_ARCHIVES "Reference build objects in first-party archives (disable for standalone .a files)" ON)
option(MONO_COMPRESS_DEBUG "Compress linked debug sections with zstd when supported" ON)

function(_mono_compress_debug linker options)
	if(NOT MONO_COMPRESS_DEBUG OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
		CMAKE_CROSSCOMPILING OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		return()
	endif()
	include(CheckLinkerFlag)
	file(TIMESTAMP "${linker}" linkerTimestamp UTC)
	file(TIMESTAMP "${CMAKE_CXX_COMPILER}" compilerTimestamp UTC)
	set(compressionOption "LINKER:--compress-debug-sections=zstd")
	string(SHA256 compressionCheck
		"${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${compilerTimestamp};${linker};${linkerTimestamp};${options};${compressionOption};${CMAKE_EXE_LINKER_FLAGS}")
	# Exercise real debug sections: some linkers lack the zstd library.
	set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -g")
	check_linker_flag(CXX "${options};${compressionOption}" MONO_COMPRESS_DEBUG_SUPPORTED_${compressionCheck})
	if(MONO_COMPRESS_DEBUG_SUPPORTED_${compressionCheck})
		# Full Studio DWARF fell from 1242 to 573 MiB for 0.4 s extra link time.
		# This link option keeps all locals and types and does not change objects.
		add_link_options("${compressionOption}")
	endif()
endfunction()

# Archives stay inside the build tree; shipped programs contain the linked code.
# Keep vendor archives and unmeasured toolchains on their existing rules.
set(MONO_ARCHIVE_OPTIONS "")
if(MONO_THIN_ARCHIVES AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
	NOT CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	execute_process(COMMAND "${CMAKE_AR}" --help
		RESULT_VARIABLE archiveStatus OUTPUT_VARIABLE archiveHelp ERROR_VARIABLE archiveError)
	if(archiveStatus STREQUAL "0" AND "${archiveHelp}${archiveError}" MATCHES "--thin")
		set(MONO_ARCHIVE_OPTIONS --thin)
	endif()
endif()

if(NOT MONO_LINKER MATCHES "^(auto|system|lld)$")
	message(FATAL_ERROR "MONO_LINKER is '${MONO_LINKER}'. It is auto, system or lld.")
endif()
if(NOT MONO_LINK_JOBS MATCHES "^[1-9][0-9]*$")
	message(FATAL_ERROR "MONO_LINK_JOBS must be a positive integer.")
endif()

# LLD used 6.4 GB for the dev Studio test link. Bound simultaneous links even
# though each link has its own workers; callers can supply their own pool.
if(CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED CMAKE_JOB_POOL_LINK)
	# Setting the property hides CMAKE_JOB_POOLS, including caller compile pools.
	get_property(hasJobPools GLOBAL PROPERTY JOB_POOLS SET)
	if(NOT hasJobPools AND DEFINED CMAKE_JOB_POOLS)
		set_property(GLOBAL PROPERTY JOB_POOLS "${CMAKE_JOB_POOLS}")
	endif()
	set_property(GLOBAL APPEND PROPERTY JOB_POOLS mono_link=${MONO_LINK_JOBS})
	set(CMAKE_JOB_POOL_LINK mono_link)
endif()

if(MONO_LINKER STREQUAL "system")
	message(STATUS "first-party linker: system")
	_mono_compress_debug("${CMAKE_LINKER}" "")
	return()
endif()

# Keep platform and cross toolchain choices intact until they are measured.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING OR
	NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	if(MONO_LINKER STREQUAL "lld")
		message(FATAL_ERROR "MONO_LINKER=lld requires a native GCC/Linux build.")
	endif()
	message(STATUS "first-party linker: system (auto supports native GCC/Linux)")
	return()
endif()

# Ubuntu can install only a versioned PATH entry (ld.lld-21). GCC needs the
# unversioned ld.lld beside the actual LLVM tools, found here without pinning a version.
file(GLOB llvmLinkers "/usr/lib/llvm-*/bin/ld.lld")
list(SORT llvmLinkers COMPARE NATURAL ORDER DESCENDING)
set(llvmDirectories "")
foreach(linker IN LISTS llvmLinkers)
	get_filename_component(linkerDirectory "${linker}" DIRECTORY)
	list(APPEND llvmDirectories "${linkerDirectory}")
endforeach()
find_program(MONO_LLD NAMES ld.lld HINTS ${llvmDirectories}
	NO_CACHE DOC "LLVM ELF linker used by MONO_LINKER")

if(MONO_LLD AND EXISTS "${MONO_LLD}")
	get_filename_component(linkerDirectory "${MONO_LLD}" DIRECTORY)
	file(TIMESTAMP "${MONO_LLD}" linkerTimestamp UTC)
	# -B is a link-only driver option. Applying it during compilation would also
	# change GCC's header and assembler search paths.
	# Keep hot, unlikely and startup sections grouped rather than interleaved by object.
	set(lldOptions
		"-B${linkerDirectory}" -fuse-ld=lld "LINKER:--threads=4" "LINKER:-z,keep-text-section-prefix"
	)
	include(CheckLinkerFlag)
	# Recheck when the selected linker, compiler version or options change.
	string(SHA256 linkerCheck "${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${linkerTimestamp};${lldOptions}")
	check_linker_flag(CXX "${lldOptions}" MONO_LLD_SUPPORTED_${linkerCheck})
	if(MONO_LLD_SUPPORTED_${linkerCheck})
		# Four workers kept the dev Studio test link at 1.74 s versus BFD's 9.70 s,
		# with 5.54 CPU-seconds versus unbounded LLD's 11.09 on this 24-thread host.
		add_link_options(${lldOptions})
		message(STATUS "first-party linker: ${MONO_LLD} (4 threads)")
		_mono_compress_debug("${MONO_LLD}" "${lldOptions}")
		return()
	endif()
endif()

if(MONO_LINKER STREQUAL "lld")
	message(FATAL_ERROR "MONO_LINKER=lld requested, but no working LLD was found. Set MONO_LLD to ld.lld.")
endif()
message(STATUS "first-party linker: system (LLD unavailable)")
_mono_compress_debug("${CMAKE_LINKER}" "")
