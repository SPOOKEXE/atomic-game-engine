#include <engine/assets/AssetKind.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <assetc/Bake.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

// Walk, bake, write, report. Every decision that is a decision lives in the
// library; what is left here is parsing a command line and printing rows.
//
//     assetc --input art --output content
//     cdn --publish content --store store --signing-key HEX
//
// The second command has no idea the first ran, which is the contract: this
// produces a directory, and a publisher publishes directories.

int main(int argc, char **argv) {
	engine::core::Log::Initialise("assetc");

	engine::core::Arguments arguments(
		"assetc", "Bake a directory of source art into one a content origin can publish."
	);
	arguments.Value("input", "DIR", "The directory of source art to bake");
	arguments.Value("output", "DIR", "Where the baked tree goes");
	arguments.Value(
		"model-size",
		"METRES",
		"Scale every model so its longest axis measures this (default: 4, 0 to leave alone)"
	);
	arguments.Value(
		"max-texture", "PIXELS", "Shrink any texture wider or taller than this (default: 2048, 0 for none)"
	);
	arguments.Flag("no-copy", "Skip files this cannot bake instead of copying them across");
	arguments.Flag("quiet", "Print the summary only, not a row per asset");

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	assetc::Settings settings;

	const auto input = arguments.Get("input");
	const auto output = arguments.Get("output");
	if (!input || !output) {
		ENGINE_ERROR("assetc: --input and --output are both required");
		std::fputs(arguments.Help().c_str(), stdout);
		return 2;
	}

	settings.Input = std::filesystem::path(*input);
	settings.Output = std::filesystem::path(*output);
	settings.CopyUnknown = !arguments.Has("no-copy");

	settings.ModelSize = static_cast<float>(arguments.GetNumber("model-size", settings.ModelSize));
	settings.MaximumTexture =
		static_cast<uint32_t>(arguments.GetInteger("max-texture", settings.MaximumTexture));

	std::string failure;
	const assetc::Report report = assetc::Bake(settings, failure);
	if (!failure.empty()) {
		ENGINE_ERROR("{}", failure);
		return EXIT_FAILURE;
	}

	const bool quiet = arguments.Has("quiet");
	for (const assetc::Baked &baked : report.Assets) {
		if (!baked.Failure.empty()) {
			// A failure is always printed, however quiet the run: it is the one
			// line somebody has to act on.
			ENGINE_WARN("assetc: {} — {}", baked.Source, baked.Failure);
		} else if (!quiet) {
			ENGINE_INFO(
				"assetc: {} -> {} [{}] {} bytes",
				baked.Source,
				baked.Output,
				engine::assets::Describe(baked.Kind),
				baked.Bytes
			);
		}
	}

	ENGINE_INFO(
		"assetc: {} assets, {} failed — {} bytes in, {} bytes out",
		report.Assets.size(),
		report.Failures,
		report.SourceBytes,
		report.OutputBytes
	);

	// A failed row is a non-zero exit, so a build script notices. The assets
	// that did bake are still written, because a run that threw away four
	// hundred good files over one bad one would be the wrong trade.
	return report.Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
