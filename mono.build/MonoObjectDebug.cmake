# Compress object debug sections only when the compiler, assembler and
# applicable linkers can all handle them. The debug information stays intact.
include_guard(GLOBAL)

option(MONO_COMPRESS_OBJECT_DEBUG "Compress first-party object debug sections with zstd when supported" ON)
option(MONO_COMPRESS_VENDOR_OBJECT_DEBUG "Compress selected vendor object debug sections with zstd when supported" ON)
set(MONO_OBJECT_DEBUG_OPTIONS "")
set(MONO_VENDOR_OBJECT_DEBUG_OPTIONS "")

if((MONO_COMPRESS_OBJECT_DEBUG OR MONO_COMPRESS_VENDOR_OBJECT_DEBUG) AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
	NOT CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	function(_mono_probe_object_debug)
		include(CheckCXXSourceCompiles)
		get_directory_property(linkOptions LINK_OPTIONS)
		file(TIMESTAMP "${CMAKE_CXX_COMPILER}" compilerTimestamp UTC)
		string(TOUPPER "${CMAKE_BUILD_TYPE}" buildConfiguration)
		set(configFlags "${CMAKE_CXX_FLAGS_${buildConfiguration}}")
		separate_arguments(compilerArgument NATIVE_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
		separate_arguments(compilerFlags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${configFlags}")
		# GCC can choose a different default linker through -B or COMPILER_PATH.
		execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${compilerArgument} ${compilerFlags} -print-prog-name=ld
			RESULT_VARIABLE linkerQueryStatus OUTPUT_VARIABLE driverLinker OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
		if(linkerQueryStatus STREQUAL "0" AND driverLinker)
			if(NOT IS_ABSOLUTE "${driverLinker}" AND NOT driverLinker MATCHES "/")
				find_program(resolvedLinker NAMES "${driverLinker}" NO_CACHE)
			else()
				get_filename_component(resolvedLinker "${driverLinker}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
			endif()
			if(EXISTS "${resolvedLinker}")
				file(TIMESTAMP "${resolvedLinker}" resolvedLinkerTimestamp UTC)
				file(SIZE "${resolvedLinker}" resolvedLinkerSize)
			endif()
		endif()
		foreach(tool IN ITEMS CMAKE_LINKER MONO_LLD)
			if(EXISTS "${${tool}}")
				file(TIMESTAMP "${${tool}}" "${tool}Timestamp" UTC)
			endif()
		endforeach()
		string(SHA256 probeKey
			"${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${compilerTimestamp};${CMAKE_CXX_COMPILER_ARG1};${CMAKE_CXX_FLAGS};${configFlags};${CMAKE_SYSROOT};${CMAKE_SYSROOT_COMPILE};${CMAKE_SYSROOT_LINK};${CMAKE_EXE_LINKER_FLAGS};${CMAKE_EXE_LINKER_FLAGS_${buildConfiguration}};${linkOptions};${CMAKE_LINKER};${CMAKE_LINKERTimestamp};${MONO_LLD};${MONO_LLDTimestamp};${resolvedLinker};${resolvedLinkerTimestamp};${resolvedLinkerSize}")
		# This must link an executable: a compiler can accept the option while
		# its assembler or selected linker cannot consume compressed DWARF.
		set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
		set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -g -gz=zstd")
		list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${linkOptions})
		check_cxx_source_compiles("int main() { return 0; }" MONO_OBJECT_DEBUG_SUPPORTED_${probeKey})
		# GCC can accept -gz while the assembler silently ignores it. Check an
		# actual object before promising that the build will save space.
		find_program(objectReadelf NAMES readelf llvm-readelf)
		set(objectCompressed FALSE)
		if(MONO_OBJECT_DEBUG_SUPPORTED_${probeKey} AND objectReadelf)
			set(probeSource "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/mono-object-debug-probe.cpp")
			set(probeObject "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/mono-object-debug-probe.o")
			file(WRITE "${probeSource}"
				"struct DebugProbe { int first; int second; double values[16]; };\nint main() { DebugProbe value{}; return value.first; }\n")
			set(sysrootOption "")
			if(CMAKE_SYSROOT_COMPILE)
				set(sysrootOption "--sysroot=${CMAKE_SYSROOT_COMPILE}")
			elseif(CMAKE_SYSROOT)
				set(sysrootOption "--sysroot=${CMAKE_SYSROOT}")
			endif()
			execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${compilerArgument} ${compilerFlags} ${sysrootOption}
				-g -gz=zstd -c "${probeSource}" -o "${probeObject}"
				RESULT_VARIABLE compileStatus OUTPUT_QUIET ERROR_QUIET)
			if(compileStatus STREQUAL "0")
				execute_process(COMMAND "${objectReadelf}" -SW "${probeObject}"
					RESULT_VARIABLE inspectStatus OUTPUT_VARIABLE sections ERROR_QUIET)
				if(inspectStatus STREQUAL "0" AND sections MATCHES "\\.debug_info[^\n]*[ \t]C[ \t]")
					set(objectCompressed TRUE)
				endif()
			endif()
			file(REMOVE "${probeSource}" "${probeObject}")
		endif()
		if(objectCompressed AND MONO_COMPRESS_OBJECT_DEBUG)
			set(MONO_OBJECT_DEBUG_OPTIONS -gz=zstd PARENT_SCOPE)
		endif()
		if(objectCompressed AND MONO_COMPRESS_VENDOR_OBJECT_DEBUG)
			# Vendor executables can use the default linker even when first-party
			# executables select another. Both must consume the compressed object.
			file(WRITE "${probeSource}"
				"struct DebugProbe { int first; int second; double values[16]; };\nDebugProbe value{}; int debug_value() { return value.first; }\n")
			execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${compilerArgument} ${compilerFlags} ${sysrootOption}
				-g -gz=zstd -c "${probeSource}" -o "${probeObject}"
				RESULT_VARIABLE vendorCompileStatus OUTPUT_QUIET ERROR_QUIET)
			if(vendorCompileStatus STREQUAL "0")
				set(vendorProbeKey "${probeKey}")
				set(CMAKE_REQUIRED_LIBRARIES "${probeObject}")
				set(CMAKE_REQUIRED_LINK_OPTIONS "")
				check_cxx_source_compiles("int debug_value(); int main() { return debug_value(); }"
					MONO_VENDOR_OBJECT_DEBUG_DEFAULT_${vendorProbeKey})
				list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${linkOptions})
				check_cxx_source_compiles("int debug_value(); int main() { return debug_value(); }"
					MONO_VENDOR_OBJECT_DEBUG_SELECTED_${vendorProbeKey})
				if(MONO_VENDOR_OBJECT_DEBUG_DEFAULT_${vendorProbeKey} AND
					MONO_VENDOR_OBJECT_DEBUG_SELECTED_${vendorProbeKey})
					set(MONO_VENDOR_OBJECT_DEBUG_OPTIONS -gz=zstd PARENT_SCOPE)
				endif()
			endif()
			file(REMOVE "${probeSource}" "${probeObject}")
		endif()
	endfunction()
	_mono_probe_object_debug()
	if(MONO_COMPRESS_OBJECT_DEBUG AND NOT MONO_OBJECT_DEBUG_OPTIONS)
		message(STATUS "object debug compression: unavailable; using uncompressed objects")
	endif()
	if(MONO_COMPRESS_VENDOR_OBJECT_DEBUG AND NOT MONO_VENDOR_OBJECT_DEBUG_OPTIONS)
		message(STATUS "vendor object debug compression: unavailable; using uncompressed objects")
	endif()
endif()

function(mono_vendor_object_debug target)
	if(NOT MONO_VENDOR_OBJECT_DEBUG_OPTIONS OR NOT TARGET "${target}")
		return()
	endif()
	get_target_property(kind "${target}" TYPE)
	get_target_property(imported "${target}" IMPORTED)
	if(NOT imported AND kind MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY)$")
		target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${MONO_VENDOR_OBJECT_DEBUG_OPTIONS}>")
	endif()
endfunction()
