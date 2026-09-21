# L8. What a game is made of, as far as the bytes are concerned.
#
# Content addressing, chunking, the hash tree and the manifest - the format both
# halves of content delivery share. The running game uses these to fetch and
# verify; `mono.cdn` uses the same functions to build, sign and serve. Writing
# the format twice is how a format acquires a dialect, and the bug that produces
# is a client rejecting content the origin considers valid.
#
# **This module deliberately does not depend on `scene`.** That is the whole
# reason the content origin can link it alone and remain a program with no
# simulation in it.
#
# Both vendors are VENDOR rather than VENDOR_PUBLIC, so neither `blake3.h` nor
# any Crypto++ header reaches a public header and no consumer of this module
# inherits one. `ContentHash`, `PublicKey` and `SignatureBytes` are plain byte
# arrays of our own and say nothing about what produced them.
#
# Crypto++ supplies Ed25519 - through its flat `Donna` API rather than the
# PK_Signer machinery, because one signature per published manifest needs none
# of that - and the constant-time compare. BLAKE3 supplies the content hash.
mono_add_library(assets
	TIER shared
	DEPS
		Engine::core
	VENDOR
		Vendor::blake3
		Vendor::cryptopp
)

option(MONO_FUZZ_SHADER "Build the cooked shader parser with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_SHADER)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_SHADER requires Clang with libFuzzer")
	endif()
	# Compile the actual parser and hashing boundary with coverage and sanitizers.
	# Linking the ordinary assets archive alone would leave the parser uninstrumented.
	add_executable(fuzz_shader fuzz/Shader.cpp src/Shader.cpp src/ContentHash.cpp)
	target_include_directories(fuzz_shader PRIVATE include)
	target_link_libraries(fuzz_shader PRIVATE Engine::core Vendor::blake3)
	target_compile_options(fuzz_shader PRIVATE
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	target_link_options(fuzz_shader PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_shader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
endif()
