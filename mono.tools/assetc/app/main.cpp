#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <assetc/Bake.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

// The CLI parses options and reports the bake library's rows.

int main(int argc, char **argv) {
	engine::core::Log::Initialise("assetc");

	// **Only the handle verb.** `assetc` decodes and writes; publishing is
	// `cdn`'s, and a baker carrying `cdn.publish.*` would be offering settings
	// nothing here reads.
	engine::core::Config::DeclareEngineFlags();
	engine::assets::DeclareContentFlags(engine::assets::ContentVerb::Handle);

	engine::core::Arguments arguments(
		"assetc", "Bake a directory of source art into one a content origin can publish."
	);
	engine::core::Config::DeclareOptions(arguments);
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
	arguments.Value(
		"flipbook-fps",
		"FPS",
		"Override the frame rate of every imported flipbook (default: 0, keep what the GIF said)"
	);
	arguments.Flag("no-mipmaps", "Skip the mip chain, leaving every texture one level");
	arguments.Flag("no-copy", "Skip files this cannot bake instead of copying them across");
	arguments.Flag("quiet", "Print the summary only, not a row per asset");

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.VersionRequested) {
		std::fputs(arguments.VersionLine().c_str(), stdout);
		return 0;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	const engine::core::ConfigReport configured = engine::core::Config::Apply(arguments);
	if (!configured.Ok) {
		std::fprintf(stderr, "%s\n", configured.Error.c_str());
		return 2;
	}
	if (engine::core::Config::ListingWanted(arguments)) {
		std::fputs(engine::core::Flags::Listing().c_str(), stdout);
		return 0;
	}

	// **Constructed after the settings are frozen**, because `Settings::Content`
	// defaults from the process policy and a `Settings` built before `Apply`
	// would carry the answer from halfway through startup.
	assetc::Settings settings;
	if (const std::string refused = settings.Content.RefusedText(); !refused.empty()) {
		ENGINE_INFO("assetc: not baking - {}", refused);
	}

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
	settings.Mipmaps = !arguments.Has("no-mipmaps");

	settings.ModelSize = static_cast<float>(arguments.GetNumber("model-size", settings.ModelSize));
	settings.MaximumTexture =
		static_cast<uint32_t>(arguments.GetInteger("max-texture", settings.MaximumTexture));
	settings.FlipbookFps = static_cast<float>(arguments.GetNumber("flipbook-fps", settings.FlipbookFps));

	std::string failure;
	const assetc::Report report = assetc::Bake(settings, failure);
	if (!failure.empty()) {
		ENGINE_ERROR("{}", failure);
		return EXIT_FAILURE;
	}

	const bool quiet = arguments.Has("quiet");
	for (const assetc::Baked &baked : report.Assets) {
		if (!baked.Failure.empty()) {
			// Failures remain visible in quiet mode.
			ENGINE_WARN("assetc: {} - {}", baked.Source, baked.Failure);
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
		"assetc: {} assets, {} failed - {} bytes in, {} bytes out",
		report.Assets.size(),
		report.Failures,
		report.SourceBytes,
		report.OutputBytes
	);

	// Preserve successful outputs, but make any failed row fail the command.
	return report.Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
