#pragma once

// What an asset looks like, and why it sometimes does not.
//
// **Three outcomes, not two, and that is the correction this file exists for.**
// The first version of the previews drew one empty box for everything that had
// no picture, with a comment defending it: "whether this is 'not yet' or 'never'
// is deliberately not distinguished". That reasoning was wrong, and
// `explorer-plus` shows why — it separates *refused* from *unreadable* and says
// which:
//
// > "Preview disabled due to model size" / "Preview unavailable"
//
// The difference is actionable. A file refused for being enormous is one
// somebody can decimate, split or ignore knowingly; a file that would not decode
// is a pipeline problem. One blank square for both tells them neither, and
// worse, makes a working editor look broken on a store full of `.pmx` files it
// was never going to preview.
//
// **A budget is checked before the work, not discovered during it.** Also from
// the same reference: `objectIsTooLarge` walks the tree counting parts and stops
// at the limit rather than cloning first and regretting it. Here that is the
// source file's size for an image and the mesh's triangle count for geometry —
// both known before a byte is decoded or uploaded.
//
// @tier client

#include <engine/assets/AssetKind.hpp>

#include <cstdint>
#include <string>

namespace studio {

	// How a preview turned out.
	//
	// @since v0.10
	enum class PreviewState : uint8_t {
		// Queued, or being built. The caller draws its placeholder.
		Pending,

		// There is a picture.
		Ready,

		// Refused on purpose: past a budget this editor will not spend.
		//
		// **Named apart from `Unavailable` because it is the one a person can
		// act on.** A 90-megabyte source or a two-million-triangle mesh is a
		// deliberate refusal, and saying so is the difference between "the
		// editor is broken" and "that model is enormous".
		TooLarge,

		// Tried and could not: not an image, not a mesh, or a file that would
		// not decode. A `.pmx` in a store that has not been baked is this, and
		// it is the common case rather than an error.
		Unavailable,
	};

	// A sentence to put in the empty box.
	//
	// @param state What happened.
	// @param kind  What the asset claims to be, so the wording can name it.
	// @return A view valid for the lifetime of the process, or nullptr when
	//         there is nothing worth saying — `Ready` has a picture instead and
	//         `Pending` must not flash text that is about to be replaced.
	const char *DescribePreview(PreviewState state, engine::assets::AssetKind kind);

	// The largest source file a preview will read.
	//
	// **A preview is not worth an arbitrary allocation.** The decoders bound
	// what they will *produce*; this bounds what is read off disk to feed one,
	// so a hundred-megabyte source dropped in the store does not become a
	// hundred-megabyte read on the frame its row scrolled past.
	constexpr uint64_t PREVIEW_MAXIMUM_SOURCE_BYTES = 64ull * 1024u * 1024u;

	// The largest mesh a preview will upload, in triangles.
	//
	// **Checked after decoding and before uploading**, which is the only place
	// it can be: a `.amesh` states its own counts in a header the reader has
	// already validated, so the number is free — and the thing being avoided is
	// a GPU upload and a draw, not the decode.
	//
	// A quarter of a million is well past any prop and short of the character
	// models this repository's own seed content contains, which is deliberate:
	// the budget should refuse something, or it is not a budget.
	constexpr uint32_t PREVIEW_MAXIMUM_TRIANGLES = 250000;
}
