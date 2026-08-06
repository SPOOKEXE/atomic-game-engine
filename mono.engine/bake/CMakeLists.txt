# L9. Turning somebody else's file into one of ours.
#
# The importers and the node pipeline `ROADMAP.md` v0.9 asks for. `assets` is
# what a baked mesh or texture *is*; this is the only thing in the engine that
# reads a `.glb`, a `.pmx`, a `.png` or a `.bmp`, and it exists so that nothing
# else ever has to.
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
# Both vendors are VENDOR rather than VENDOR_PUBLIC. Crypto++ supplies the
# DEFLATE decoder a PNG's pixels are compressed with and the CRC-32 its chunks
# carry; nlohmann/json parses glTF's JSON chunk. Neither reaches a public header
# here — what this module hands back is an `assets::MeshData` or an
# `assets::TextureData` and nothing that says what produced it.
mono_add_library(bake
	TIER shared
	DEPS
		Engine::core
		Engine::assets
	VENDOR
		Vendor::cryptopp
		Vendor::json
)
