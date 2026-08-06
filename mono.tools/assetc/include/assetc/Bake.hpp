#pragma once

// Turning a directory of source art into a directory a publisher can serve.
//
// `engine::bake` is the pipeline and has no filesystem by design; this is the
// half that does. It walks a tree, decides what each file becomes, wires the
// graph, runs it, and writes what comes out — and every one of those decisions
// is a decision, which is why it is a library with a suite rather than a
// `main.cpp`.
//
// **The output directory is `cdn --publish`'s input.** That is the whole
// contract: `assetc --input art --output content` then `cdn --publish content`,
// and the second command has no idea the first ran. Anything the baker does not
// understand is copied across unchanged, so a `.wav` beside a `.pmx` still ends
// up published — a baker that dropped what it could not bake would silently
// halve somebody's game.
//
// **A baked asset's name is its source's path with the extension replaced.**
// `characters/miku.pmx` becomes `characters/miku.amesh` and `tex/skin.png`
// becomes `tex/skin.atex`. That rule is what lets a model's texture references
// be rewritten without a lookup table: the importer reports the path the model
// spells, and the same substitution applied to it names the baked texture.

#include <engine/assets/AssetKind.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
		std::filesystem::path Output;

		// The size, in metres, every imported model's longest axis is scaled
		// to. Zero leaves models at whatever scale they were authored in.
		//
		// **Not zero by default**, because the formats disagree by an order of
		// magnitude — a PMX character is about twenty units tall and a glTF one
		// about two — so a tree baked without this produces a scene where one
		// model fills the sky. Four metres is a little above a person, which is
		// the scale a test scene wants.
		float ModelSize = 4.0f;

		// The largest texture dimension to keep. Anything wider or taller is
		// box-filtered down, preserving its aspect ratio. Zero keeps every
		// texture at its authored size.
		//
		// Present because a character pack routinely carries several 4096-pixel
		// sheets, and a scene with four of those is a hundred megabytes of
		// video memory before anything else is loaded.
		uint32_t MaximumTexture = 2048;

		// Whether to copy files the baker does not understand.
		//
		// On by default: the output tree is what gets published, and a baker
		// that dropped every sound and script would produce a publishable
		// directory that is missing half the game.
		bool CopyUnknown = true;
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
		// **A failure is a row rather than an abort.** One unreadable file in a
		// directory of four hundred should cost that file, not the run — and
		// the reason has to reach a person, because "it did not import" without
		// the reason is something somebody has to bisect a build to act on.
		std::string Failure;
	};

	// What a whole run did.
	//
	// @since v0.9
	struct Report {
		std::vector<Baked> Assets;

		// How many rows carry a failure.
		size_t Failures = 0;

		// How many source bytes went in and how many baked bytes came out.
		//
		// Both, because the interesting number is the ratio: a mesh usually
		// grows — an interleaved float layout is bigger than a packed one —
		// and a texture usually shrinks, and a publisher wants to know which
		// way the tree went before compressing it.
		uint64_t SourceBytes = 0;
		uint64_t OutputBytes = 0;
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
