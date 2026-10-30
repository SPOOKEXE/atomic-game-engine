# Compress debug sections in first-party objects when the compiler, assembler
# and selected linker can all handle them. The debug information stays intact.
include_guard(GLOBAL)

option(MONO_COMPRESS_OBJECT_DEBUG "Compress first-party object debug sections with zstd when supported" ON)
set(MONO_OBJECT_DEBUG_OPTIONS "")

if(MONO_COMPRESS_OBJECT_DEBUG AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
	NOT CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	function(_mono_probe_object_debug)
		include(CheckCXXSourceCompiles)
		get_directory_property(linkOptions LINK_OPTIONS)
		file(TIMESTAMP "${CMAKE_CXX_COMPILER}" compilerTimestamp UTC)
		string(TOUPPER "${CMAKE_BUILD_TYPE}" buildConfiguration)
		set(configFlags "${CMAKE_CXX_FLAGS_${buildConfiguration}}")
		string(SHA256 probeKey
			"${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${compilerTimestamp};${CMAKE_CXX_FLAGS};${configFlags};${CMAKE_SYSROOT};${CMAKE_EXE_LINKER_FLAGS};${linkOptions}")
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
			separate_arguments(compilerArgument NATIVE_COMMAND "${CMAKE_CXX_COMPILER_ARG1}")
			separate_arguments(compilerFlags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${configFlags}")
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
		if(objectCompressed)
			set(MONO_OBJECT_DEBUG_OPTIONS -gz=zstd PARENT_SCOPE)
		endif()
	endfunction()
	_mono_probe_object_debug()
	if(NOT MONO_OBJECT_DEBUG_OPTIONS)
		message(STATUS "object debug compression: unavailable; using uncompressed objects")
	endif()
endif()
