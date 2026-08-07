#pragma once

// Filesystem-facing wrapper around the testable bake pipeline. Unknown files are
// copied by default, and baked names replace source extensions deterministically.

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
	};

	// What a whole run did.
	//
	// @since v0.9
	struct Report {
		std::vector<Baked> Assets;

		// How many rows carry a failure.
		size_t Failures = 0;

		// Source and output totals for completed rows.
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
