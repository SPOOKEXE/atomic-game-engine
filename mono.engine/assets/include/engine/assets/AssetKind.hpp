#pragma once

// Closed asset-kind list carried by the manifest. Kind is selected at publish
// time and is a routing label, not an interpretation of the payload.
// @tier L8 · shared

#include <cstdint>
#include <string_view>

namespace engine::assets {

	// What subsystem an asset belongs to.
	enum class AssetKind : uint8_t {
		// No claim. What an unrecognised extension produces, and what an old
		// manifest's assets read as. Deliverable like anything else — an origin
		// moves bytes it does not interpret — and simply not routable by kind.
		Unknown = 0,

		// Geometry. `ROADMAP.md` v0.9's mesh importing and baking pipeline.
		Mesh = 1,

		// An image, whatever it is eventually sampled as — albedo, normal,
		// lightmap. **Not split by role here**, because role is a property of
		// the material that references it rather than of the bytes, and a
		// manifest that guessed would be wrong for every texture used twice.
		Texture = 2,

		// Sound. `ROADMAP.md` v0.9's "audio in cdn and running audio in-studio
		// and in-game".
		Audio = 3,

		// A material or surface description — what binds textures to a mesh.
		Material = 4,

		// A typeface.
		Font = 5,

		// Source or bytecode. Delivered like anything else; whether it *runs*
		// is `script`'s decision and its sandbox's, not delivery's.
		Script = 6,

		// Moving pictures, for `VideoFrame`.
		Video = 7,

		// Structured data with no subsystem of its own — a level description, a
		// table, a game file. The honest bucket rather than a dumping ground:
		// anything here is delivered and handed over whole.
		Data = 8,
	};

	// Returns a stable, human-readable name for a kind.
	//
	// Lowercase and stable, because it reaches a log line, a studio panel and a
	// command line — all three of which are places `AGENTS.md` rule 4 says a
	// name goes rather than a number.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(AssetKind kind);

	// Parses what Describe wrote.
	//
	// @param text The lowercase name.
	// @return The kind, or `Unknown` for anything else.
	AssetKind KindFromName(std::string_view text);

	// Derives a kind from a name's extension at publish time.
	//
	// @param name The content name, as the manifest holds it.
	// @return The kind.
	AssetKind KindOfName(std::string_view name);
}
