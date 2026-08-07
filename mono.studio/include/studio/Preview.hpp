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

	// The longest side a material preview's colour map is resampled to.
	//
	// **A material preview is the one that would otherwise grow without a
	// bound.** A mesh preview is capped by its triangle count and a thumbnail is
	// resampled to `THUMBNAIL_SIDE`, but a material's sheet is whatever the
	// publisher baked — this repository's own seed content is 1K and 2K — and
	// nothing evicts a preview once it is uploaded. Hovering thirty 2K sheets at
	// full size is half a gigabyte of device memory for pictures of spheres.
	//
	// 256 rather than `THUMBNAIL_SIDE`, because this one is *sampled* rather than
	// blitted: it wraps a sphere at the hover panel's size, so the texels that
	// survive are the ones under a curved surface at a glancing angle, and a
	// 64-pixel sheet reads as a smear where a thumbnail at 64 reads as a picture.
	constexpr uint32_t PREVIEW_MATERIAL_SIDE = 256;

	// Whether a kind's preview is a render rather than a bitmap.
	//
	// **One answer, because the question is asked in two places.** The hover
	// panel decides whether to drive the preview slot, and a list row decides
	// whether to paint what that slot holds — and the two have to agree exactly,
	// or a row draws a dash beside a hover panel showing the thing. That is the
	// duplicate-policy failure this repository keeps recording, so the condition
	// lives here rather than being spelled in `HoverPreview.cpp` and `Assets.cpp`.
	//
	// **A mesh and a material, and nothing else.** Both are references with no
	// picture of their own: a mesh is geometry that has to be drawn, and a
	// material is a texture reference whose picture is the engine's sphere
	// wearing it. Everything else in a store — a texture, a GIF, audio, a script
	// — either has pixels already or has none to have.
	constexpr bool PreviewIsRendered(engine::assets::AssetKind kind) {
		return kind == engine::assets::AssetKind::Mesh ||
			   kind == engine::assets::AssetKind::Material;
	}
}
