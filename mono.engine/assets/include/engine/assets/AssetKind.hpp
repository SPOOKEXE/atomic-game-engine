#pragma once

// Closed asset-kind list carried by the manifest. Kind is selected at publish
// time and is a routing label, not an interpretation of the payload.
//
// **The table that decides it lives in `ContentForm.hpp`**, which is the finer
// answer to the same question: a form is the format and a kind is the
// subsystem, several forms map to one kind, and both used to be derived from
// two lists that had to agree.
//
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

		// A compiled shader module, for a pipeline node that names one.
		//
		// **Its own kind rather than `Script` or `Data`.** `Script` is source a
		// VM may run in a sandbox and `Data` is bytes handed over whole; a
		// shader is neither — it is compiled ahead of time, it is handed to a
		// GPU rather than to an interpreter, and whether it is *safe* is a
		// question about a driver rather than about a sandbox. Routing it as
		// either would put it through the wrong subsystem's door.
		//
		// **What arrives is SPIR-V.** Source extensions route here too, because
		// what somebody publishes is what they wrote — `bake` is what turns one
		// into the other, exactly as it does for a mesh.
		//
		// @since v0.11
		Shader = 9,
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

	// Whether a runtime can read this name's bytes as they are.
	//
	// **The extension table holds both sides of every format and this is the
	// question it cannot answer.** `.pmx` and `.amesh` are both `Mesh`; `.png`
	// and `.atex` are both `Texture` — which is right for *routing*, because a
	// publisher pointed at a source tree and one pointed at a baked tree must
	// classify the same way. It is wrong for anything asking "will this load",
	// and until v0.10 nothing asked: the local store published `raw/` directly,
	// so every content picker in the editor offered PNGs and PMX files that
	// could never draw, and choosing one produced a part that silently kept its
	// old appearance.
	//
	// **A source is not "unreadable", it is unbaked**, and the two look the same
	// from here. What this answers is whether `assetc` has already been over it.
	//
	// Formats with no baked form of their own — audio, scripts, fonts, data —
	// are readable as they are, which is why this is not simply a list of three
	// extensions.
	//
	// @param name The content name, as the manifest holds it.
	// @return `false` for a source form a baker still has to convert.
	// @since v0.10
	bool IsRuntimeReadable(std::string_view name);

	// Derives a kind from a name's extension at publish time.
	//
	// @param name The content name, as the manifest holds it.
	// @return The kind.
	AssetKind KindOfName(std::string_view name);
}
