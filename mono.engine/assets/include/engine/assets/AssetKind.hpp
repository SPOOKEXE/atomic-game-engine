#pragma once

// What an asset *is*, as a closed list the manifest carries.
//
// A delivery client asks for "the meshes for this scene" or "every sound this
// world references", and something has to answer. Two designs were available
// and only one of them survives contact with `AGENTS.md` rule 4:
//
// - **Derive it from the name's extension, at the client.** Then the origin and
//   the client each hold an opinion about what `rock.glb` is, and the day one
//   of them learns a new extension they disagree. Two copies of one fact.
// - **Record it in the manifest, once, at publish time.** The publisher derives
//   it from the extension exactly once — `KindOfName` below — and everything
//   downstream reads the answer rather than recomputing it.
//
// The second, for the reason the manifest exists at all: it is *the one place a
// name becomes something else*, so it is also the place a name becomes a kind.
//
// **A kind is a routing label, not a format.** It says which subsystem a blob
// belongs to, and says nothing about what is inside it — there is no mesh
// format in this engine yet, and this deliberately does not invent one. What
// `Mesh` means is "the mesh pipeline's, when there is one"; the import and
// cooking work is `ROADMAP.md` v0.9's and lands beside this rather than under
// it.
//
// **The list is closed and its numbers are part of the format.** A kind crosses
// a manifest, so a value's meaning may not change — appending is safe and
// renumbering is not. `Unknown` is zero so a field nobody wrote reads as "no
// claim" rather than as whichever kind happened to be first.
//
// @tier L8 · shared

#include <cstdint>
#include <string_view>

namespace engine::assets {

	// What subsystem an asset belongs to.
	//
	// @since v0.9
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

	// Derives a kind from a content name's extension.
	//
	// **Called once, by the publisher, and by nothing else.** This is the whole
	// of the extension-to-kind opinion in the engine; every other place reads
	// the manifest. A second caller on a serving or fetching path would be the
	// second copy of the fact this file exists to have only one of.
	//
	// Unrecognised extensions are `Unknown` rather than a guess. An asset whose
	// kind nobody knows still delivers — it is simply not something a
	// kind-filtered request will return, which is the honest outcome.
	//
	// @param name The content name, as the manifest holds it.
	// @return The kind.
	AssetKind KindOfName(std::string_view name);
}
