# L9. Turning somebody else's file into one of ours.
#
# The importers and the node pipeline `ROADMAP.md` v0.9 asks for. `assets` is
# what a baked mesh or texture *is*; this is the only thing in the engine that
# reads a `.glb`, a `.pmx`, a `.png`, a `.bmp` or an `.svg`, and it exists so
# that nothing else ever has to.
#
# **Nothing a shipped game links may link this.** `Texture.hpp` states the rule
# and this module is where it becomes checkable: a parser for a foreign format
# is the largest attack surface a content pipeline has, and a client that
# carried one would be paying for a Huffman tree on the frame a texture streamed
# in. The row in `mono.tools/architecture/expected_graph.json` is the standing
# proof — `client`, `server` and `cdn` must not name `bake`, and the day one
# does, that diff is the review.
#
# `shared` rather than a tool-tier idea because two things bake: the `assetc`
# CLI and the studio's import. A second copy of a glTF reader is how a format
# acquires a dialect, which is the same argument `assets` makes about the
# manifest.
#
# All three vendors are VENDOR rather than VENDOR_PUBLIC. Crypto++ supplies the
# DEFLATE decoder a PNG's pixels are compressed with and the CRC-32 its chunks
# carry; nlohmann/json parses glTF's JSON chunk; zstd inflates the chunks of a
# `.rbxm` that a recent Studio wrote. None reaches a public header here — what
# this module hands back is an `assets::MeshData`, an `assets::TextureData` or a
# `RobloxModel`, and nothing that says what produced it.
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

		# L9, `shared`. The node vocabulary and the document format, split out at
		# v0.13 so that `Engine::game` can read a pipeline without linking the
		# decoders below. `D00102`, and `bakegraph/CMakeLists.txt` carries the
		# argument. What is left here is everything that touches a payload.
		Engine::bakegraph
	VENDOR
		Vendor::cryptopp
		Vendor::json
		Vendor::zstd
)
