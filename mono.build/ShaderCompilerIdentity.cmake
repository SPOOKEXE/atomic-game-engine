# Writes the SHA256 of the shader compiler to MONO_SHADER_COMPILER_ID.
#
# Run once after glslc links, so ShaderCache.cmake reads 64 bytes per shader
# instead of hashing the ~100 MB binary twice. The file is rewritten only when
# the hash changes, so a relink that produced the same bytes leaves every
# shader step up to date.
cmake_minimum_required(VERSION 3.24)
foreach(required IN ITEMS COMPILER COMPILER_ID)
	if(NOT DEFINED MONO_SHADER_${required} OR MONO_SHADER_${required} STREQUAL "")
		message(FATAL_ERROR "Set MONO_SHADER_${required}")
	endif()
endforeach()
file(SHA256 "${MONO_SHADER_COMPILER}" hash)
if(EXISTS "${MONO_SHADER_COMPILER_ID}")
	file(READ "${MONO_SHADER_COMPILER_ID}" previous)
	if(previous STREQUAL hash)
		return()
	endif()
endif()
file(WRITE "${MONO_SHADER_COMPILER_ID}.tmp" "${hash}")
file(RENAME "${MONO_SHADER_COMPILER_ID}.tmp" "${MONO_SHADER_COMPILER_ID}")
