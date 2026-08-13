#pragma once

// Filesystem-facing wrapper around the testable bake pipeline. Unknown files are
// copied by default, and baked names replace source extensions deterministically.

#include <engine/assets/AssetKind.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace assetc {

	// What to bake, and how.
	//
	// @since v0.9
	struct Settings {
		// The directory of source art to walk, recursively.
		std::filesystem::path Input;

		// Where the baked tree goes. Created if it is not there.
		//
		// **Empty means nothing is written and every row carries its bytes
		// instead** — see `Baked::Payload`. That is one branch at the point of
		// emission rather than a second entry point, because everything that
		// makes a bake *correct* is in the walk above it: a model's texture
		// references are rewritten through `BakedName`, a texture is resized
		// against its decoded dimensions, a material's colour map is rewritten
		// the same way. A separate in-memory baker would be a second place to
		// spell all of that, and the one that drifted would be the one nothing
		// published from.
		std::filesystem::path Output;

		// The size, in metres, every imported model's longest axis is scaled
		// to. Zero leaves models at whatever scale they were authored in.
		//
		// Nonzero default avoids mixing format-specific unit scales.
		float ModelSize = 4.0f;

		// The largest texture dimension to keep. Anything wider or taller is
		// box-filtered down, preserving its aspect ratio. Zero keeps every
		// texture at its authored size.
		//
		uint32_t MaximumTexture = 2048;

		// The frame rate to stamp on every imported flipbook, overriding what
		// the source said. Zero keeps what the source said.
		//
		// **An override and not a default**, which is why zero means "leave it
		// alone" rather than "use twelve": a GIF states a delay per frame and
		// the decoder averages those into a rate, so the common case needs
		// nothing said here. This exists for the case where the source is wrong
		// — an exporter that wrote 100ms on every frame of something drawn at
		// 24fps, which is a thing exporters do — and for re-timing an animation
		// without re-exporting it.
		//
		// It applies to every flipbook in the run, because `assetc` bakes a
		// tree and has no per-file switches. Re-timing one animation means
		// baking it on its own.
		//
		// @since v0.10
		float FlipbookFps = 0.0f;

		// Finds the source a model's texture reference means, when the tree
		// alone cannot say.
		//
		// **Because a flattened store destroys the relationship a model file
		// relies on.** A `.pmx` names its sheets the way it was authored —
		// `tex/体.png`, relative to the folder the model sat in — and that works
		// perfectly while the bake walks an art tree with the `tex/` folder still
		// beside the model. `cdn::ImportFile` renames every file to
		// `<hash><extension>` in one flat directory, so after an import the
		// model is `<hash>.pmx`, the sheet is a different `<hash>.png`, and
		// nothing in the folder records that the two belong together.
		//
		// The lexical join then produced `tex/体.atex` — a name no manifest
		// carries, written into the mesh without a word. Every PMX character in
		// this repository's own store baked and published with dangling sheet
		// references, and the symptom was models that arrive, draw, and are
		// untextured: the mesh is right, the geometry is right, and the one
		// string joining it to its pixels points at nothing.
		//
		// **The import log is the only surviving link**, so the resolver lives
		// with whoever owns the log rather than here — `cdn::StoreTextureResolver`
		// builds one. This module stays ignorant of `mono.cdn`, which the tier
		// check would refuse anyway.
		//
		// Unset means "the tree is the truth", which is what a plain
		// `assetc --input ART` run wants and what every bake did before v0.10.
		//
		// @param model     The model being baked, relative to `Input`.
		// @param reference The texture as the model spelled it.
		// @param out       Set to the *source* name, relative to `Input` — not
		//        the baked one. `BakedName` is applied by the caller so the
		//        naming rule stays in one place.
		// @return `false` to fall back to resolving against the tree.
		// @since v0.10
		std::function<bool(std::string_view model, std::string_view reference, std::string &out)>
			ResolveTexture;

		// Whether to copy files the baker does not understand.
		//
		// The output tree is also the publisher's input.
		bool CopyUnknown = true;

		// Bake only this one source, as a path relative to `Input`.
		//
		// **For the editor, which bakes what somebody just picked.** A studio
		// showing the raw folder has to turn one file into something a runtime
		// reads *now* — re-walking a store of six thousand assets to do it would
		// take minutes, and a picker that hung for minutes is one nobody uses.
		//
		// **A filter on the existing walk rather than a second entry point**,
		// because everything that makes a bake correct is in that walk: a
		// material's colour map is rewritten through `BakedName`, a model's
		// textures likewise, and a second path would be a second chance to
		// spell that rule differently. What it costs is a directory scan to find
		// one file, which is microseconds against decoding it.
		//
		// Empty bakes the whole tree, which is what every other caller wants.
		//
		// @since v0.10
		std::string Only;
	};

	// What one source file became.
	//
	// @since v0.9
	struct Baked {
		// Where it came from, relative to the input directory.
		std::string Source;

		// What was written, relative to the output directory.
		std::string Output;

		// What it is.
		engine::assets::AssetKind Kind = engine::assets::AssetKind::Unknown;

		// How many bytes were written.
		uint64_t Bytes = 0;

		// Empty when it baked, and why not when it did not.
		//
		// File failures remain rows; run-start failures use `failure`.
		std::string Failure;

		// What was baked, when `Settings::Output` is empty.
		//
		// **For a caller that wants the asset and not a file.** The editor's
		// raw folders are the case: a person points at their art directory and
		// expects to see it in the viewport, and writing a baked copy beside it
		// — or into somebody's content store — is a side effect they did not
		// ask for and would have to clean up. Kept here rather than returned
		// separately so a row still says what it is, what it weighs and why it
		// failed, in one place.
		//
		// Empty on a run with an output directory: the bytes are the file, and
		// holding a second copy of a two-hundred-megabyte tree in memory to say
		// so would be a bake that runs out of memory on success.
		//
		// @since v0.14
		std::vector<std::byte> Payload;
	};

	// What a whole run did.
	//
	// @since v0.9
	struct Report {
		// One row per asset the run produced, failures included — a row carries
		// its own failure so a caller can name what did not bake rather than
		// subtract two counts.
		std::vector<Baked> Assets;

		// How many rows carry a failure.
		size_t Failures = 0;

		// Source and output totals for completed rows.
		//@{
		uint64_t SourceBytes = 0;
		uint64_t OutputBytes = 0;
		//@}

		// How many model texture references named something that is not in the
		// input tree.
		//
		// **Counted rather than left to the log**, because this is the failure
		// that produces a model which loads, draws, and is silently untextured —
		// and a warning per submesh in a run of two thousand assets is a line
		// nobody scrolls back to. A non-zero number here is the one thing that
		// says "this bake produced references that cannot resolve", and
		// `contentimport` reports it beside the failure count.
		//
		// @since v0.10
		size_t DanglingTextures = 0;
	};

	// The baked name for a source path.
	//
	// Exported because the texture rewriting depends on it being the same
	// function in both places: a model reports `tex/skin.png` and the baked
	// texture is whatever this returns for it. Two spellings of the rule is a
	// model whose textures resolve to nothing.
	//
	// @param path A path relative to the input directory, with any separators.
	// @return The output path with forward slashes, or the input unchanged when
	//         it is not something this bakes.
	std::string BakedName(std::string_view path);

	// Bakes one tree into another.
	//
	// @param settings What to bake and how.
	// @param failure  Set when the run could not start at all — a missing input
	//                 directory, an output that cannot be created. A file that
	//                 failed on its own is a row in the report instead.
	// @return What happened, one row per source file.
	Report Bake(const Settings &settings, std::string &failure);
}
