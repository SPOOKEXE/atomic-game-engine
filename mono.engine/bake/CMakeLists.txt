# L9. Turning somebody else's file into one of ours.
#
# grug keep publishing formats here: model files, BMP, GIF, SVG and Roblox.
# PNG/JPEG pixels come from bounded L8 imagecodec, also used by runtime image
# buffers under the user-approved decoder exception. assets owns cooked bytes.
# Shipped programs still cannot link bake; script only links imagecodec.
# expected_graph.json checks that separation.
#
# `shared` rather than a tool-tier idea because two things bake: the `assetc`
# CLI and the studio's import. A second copy of a glTF reader is how a format
# acquires a dialect, which is the same argument `assets` makes about the
# manifest.
#
# grug vendors stay private. JSON parses glTF and zstd inflates Roblox chunks.
# imagecodec owns PNG CRC and DEFLATE. no vendor type reaches these public headers.
#
# **The `.rbxm` reader inflates two framings and vendors one of them**, which is
# a line worth having in the file that declares the dependency: an LZ4 block is
# a hundred lines with no framing, no checksum and no dictionary, and a submodule
# for it would be larger than the decompressor. A Zstandard frame is entropy
# coding and a real library, and `delivery` already links this one.
mono_add_library(bake
	TIER shared
	DEPS
		Engine::core
		Engine::assets
		Engine::imagecodec

		# L9, `shared`. The node vocabulary and the document format, split out at
		# v0.13 so that `Engine::game` can read a pipeline without linking the
		# decoders below. `D00102`, and `bakegraph/CMakeLists.txt` carries the
		# argument. What is left here is everything that touches a payload.
		Engine::bakegraph

		# Checked 2D image evaluation stays separate from foreign source decoders.
		Engine::imagegraph
	VENDOR
		Vendor::json
		Vendor::roblox_files
		Vendor::zstd
)

option(MONO_FUZZ_BAKE "Build hostile bake importers with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_BAKE)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_BAKE requires Clang with libFuzzer")
	endif()

	# Compile the importer into this standalone target so libFuzzer instruments
	# its parser. This creates no dependency from a shipped runtime module to bake.
	set(bake_fuzz_options -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	add_executable(fuzz_bake_svg fuzz/Svg.cpp src/Svg.cpp)
	target_include_directories(fuzz_bake_svg PRIVATE src)
	target_link_libraries(fuzz_bake_svg PRIVATE Engine::core Engine::assets)
	target_compile_options(fuzz_bake_svg PRIVATE ${bake_fuzz_options})
	target_link_options(fuzz_bake_svg PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_bake_svg PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")

endif()
