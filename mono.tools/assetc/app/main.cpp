#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>
#include <engine/imagegraphexport/GraphDirectoryHost.hpp>

#include <algorithm>
#include <assetc/Bake.hpp>
#include <assetc/GraphAuthoredExport.hpp>
#include <assetc/GraphCommandHost.hpp>
#include <assetc/GraphExport.hpp>
#include <assetc/GraphFileHost.hpp>
#include <assetc/GraphVideoHost.hpp>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

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
		"Override imported flipbook FPS, or state static-atlas FPS with --flipbook-side and --flipbook-frames"
	);
	arguments.Value(
		"flipbook-side", "CELLS", "Mark ordinary image atlases as a 1, 2, 4, 8 or 16 cell-wide flipbook"
	);
	arguments.Value("flipbook-frames", "COUNT", "Populated static-atlas flipbook cells, up to side squared");
	arguments.Value("graph-output", "NAME", "Select this named image graph output when baking .graph files");
	arguments.Value("graph-tick", "TICK", "Evaluate .graph files at this fixed tick (default: 0)");
	arguments.Value("graph-seed", "SEED", "Evaluate .graph files with this seed (default: 0)");
	arguments.Value(
		"graph-image", "SOURCE=PATH", "Explicit static image input for image.captured; may be repeated"
	);
	arguments.Value(
		"graph-file-read",
		"NODE=PATH",
		"Grant one exact graph file read, including layered archives; repeatable"
	);
	arguments.Value("graph-directory-read", "NODE=ROOT", "Grant exact directory enumeration; repeatable");
	arguments.Value("graph-file-write", "NODE=PATH", "Grant one exact graph file write; repeatable");
	arguments.Value(
		"graph-file-resource", "NODE:RESOURCE=PATH", "Grant an exact native room dependency; repeatable"
	);
	arguments.Value("graph-video-read", "NODE=PATH", "Grant one exact recorded video file; repeatable");
	arguments.Value("video-decoder", "PATH", "Explicit absolute FFmpeg decoder for video inputs");
	arguments.Value(
		"export-node", "NODE", "Export a pc.export node using its authored controls into --output directory"
	);
	arguments.Value(
		"host-grants", "PATH", "Explicit bounded HTTP/process grants in imagegraph-host-grants format"
	);
	arguments.Value("execute-node", "NODE", "Execute one explicitly granted host file node");
	arguments.Value("export-graph", "PATH", "Export a named imagegraph output instead of baking a tree");
	arguments.Value(
		"export-frames", "FIRST:LAST[:STEP]", "Export an animation over this explicit tick range"
	);
	arguments.Value(
		"export-frame-ms", "N", "Animation frame duration in milliseconds, 1..65535 (default 33)"
	);
	arguments.Value("export-plays", "N", "Animation play count, zero loops forever (default zero)");
	arguments.Value(
		"export-quality", "N", "External still encoder quality or video CRF, 0..100 (default 23)"
	);
	arguments.Value("export-bitrate", "MBPS", "WebM bitrate in megabits per second (default 2)");
	arguments.Value(
		"image-encoder", "PATH", "Explicit absolute ImageMagick executable for external image formats"
	);
	arguments.Value("video-encoder", "PATH", "Explicit absolute FFmpeg executable for video formats");
	arguments.Value(
		"encoder-timeout-ms", "N", "External encoder deadline, 1..600000 milliseconds (default 30000)"
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
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
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

	if (const auto graph = arguments.Get("export-graph")) {
		assetc::GraphExportSettings exportSettings;
		exportSettings.Input = std::filesystem::path(*graph);
		const auto output = arguments.Get("output");
		const auto selected = arguments.Get("graph-output");
		if ((!output || !selected) && !arguments.Has("execute-node")) {
			ENGINE_ERROR("assetc: graph export requires --output and --graph-output");
			return 2;
		}
		if (output) exportSettings.Output = std::filesystem::path(*output);
		if (selected) exportSettings.OutputId = *selected;
		const auto readUnsigned = [](std::string_view text, uint64_t &value) {
			const auto parsedNumber = std::from_chars(text.data(), text.data() + text.size(), value);
			return !text.empty() && parsedNumber.ec == std::errc{} &&
				   parsedNumber.ptr == text.data() + text.size();
		};
		if (arguments.Has("graph-seed")) {
			ENGINE_ERROR("assetc: graph export has no seed override; author seed controls in the graph");
			return 2;
		}
		if (arguments.Has("export-frames") && arguments.Has("graph-tick")) {
			ENGINE_ERROR("assetc: export frames and graph tick are mutually exclusive");
			return 2;
		}
		if (const auto ticks = arguments.Get("export-frames")) {
			exportSettings.Animation = true;
			std::string_view text = *ticks;
			const size_t first = text.find(':');
			const size_t second = first == std::string_view::npos ? first : text.find(':', first + 1);
			if (first == std::string_view::npos ||
				!readUnsigned(text.substr(0, first), exportSettings.Frames.First) ||
				!readUnsigned(
					text.substr(first + 1, second == std::string_view::npos ? second : second - first - 1),
					exportSettings.Frames.Last
				) ||
				(second != std::string_view::npos &&
				 !readUnsigned(text.substr(second + 1), exportSettings.Frames.Step))) {
				ENGINE_ERROR("assetc: export frames require FIRST:LAST[:STEP]");
				return 2;
			}
		} else if (const auto tick = arguments.Get("graph-tick")) {
			if (!readUnsigned(*tick, exportSettings.Frames.First)) {
				ENGINE_ERROR("assetc: graph tick must be unsigned");
				return 2;
			}
			exportSettings.Frames.Last = exportSettings.Frames.First;
		}
		for (const auto &[name, maximum] :
			 {std::pair{"export-frame-ms", uint64_t{65535}},
			  {"export-plays", uint64_t{0xffffffff}},
			  {"export-quality", uint64_t{100}},
			  {"encoder-timeout-ms", uint64_t{600000}}}) {
			if (const auto supplied = arguments.Get(name)) {
				uint64_t number = 0;
				if (!readUnsigned(*supplied, number) || number > maximum ||
					((std::string_view(name) == "export-frame-ms" ||
					  std::string_view(name) == "encoder-timeout-ms") &&
					 number == 0)) {
					ENGINE_ERROR("assetc: invalid {}", name);
					return 2;
				}
				if (std::string_view(name) == "export-frame-ms")
					exportSettings.FrameMilliseconds = static_cast<uint16_t>(number);
				else if (std::string_view(name) == "export-plays")
					exportSettings.Plays = static_cast<uint32_t>(number);
				else if (std::string_view(name) == "export-quality")
					exportSettings.Quality = static_cast<uint32_t>(number);
				else
					exportSettings.EncoderTimeout = std::chrono::milliseconds(number);
			}
		}
		for (const auto assignment : arguments.GetAll("graph-image")) {
			const size_t separator = assignment.find('=');
			if (assignment.size() > 4096 || separator == std::string_view::npos || separator == 0 ||
				separator + 1 == assignment.size()) {
				ENGINE_ERROR("assetc: graph image requires bounded SOURCE=PATH");
				return 2;
			}
			exportSettings.ImageInputs.push_back(
				{std::string(assignment.substr(0, separator)),
				 std::filesystem::path(assignment.substr(separator + 1))}
			);
		}

		if (const auto encoder = arguments.Get("image-encoder"))
			exportSettings.ImageEncoder = std::filesystem::path(*encoder);
		if (const auto encoder = arguments.Get("video-encoder"))
			exportSettings.VideoEncoder = std::filesystem::path(*encoder);
		exportSettings.MegabitsPerSecond = arguments.GetNumber("export-bitrate", 2);
		std::vector<assetc::GraphFileGrant> fileGrants;
		for (const auto &[option, write] :
			 {std::pair{"graph-file-read", false}, std::pair{"graph-file-write", true}}) {
			for (const auto assignment : arguments.GetAll(option)) {
				const size_t separator = assignment.find('=');
				if (assignment.size() > 4096 || separator == std::string_view::npos || separator == 0 ||
					separator + 1 == assignment.size()) {
					ENGINE_ERROR("assetc: file capability requires bounded NODE=PATH");
					return 2;
				}
				fileGrants.push_back(
					{std::string(assignment.substr(0, separator)),
					 std::filesystem::path(assignment.substr(separator + 1)),
					 write}
				);
			}
		}
		for (const auto assignment : arguments.GetAll("graph-file-resource")) {
			const auto colon = assignment.find(':'), separator = assignment.find('=');
			if (assignment.size() > 4096 || colon == std::string_view::npos ||
				separator == std::string_view::npos || colon == 0 || separator <= colon + 1 ||
				separator + 1 == assignment.size()) {
				ENGINE_ERROR("assetc: file resource grant requires bounded NODE:RESOURCE=PATH");
				return 2;
			}
			fileGrants.push_back(
				{std::string(assignment.substr(0, colon)),
				 std::filesystem::path(assignment.substr(separator + 1)),
				 false,
				 std::string(assignment.substr(colon + 1, separator - colon - 1))}
			);
		}
		std::vector<engine::imagegraphexport::GraphDirectoryGrant> directoryGrants;
		for (const auto assignment : arguments.GetAll("graph-directory-read")) {
			const auto separator = assignment.find('=');
			if (assignment.size() > 4096 || separator == std::string_view::npos || !separator ||
				separator + 1 == assignment.size() || directoryGrants.size() == 64) {
				ENGINE_ERROR("assetc: directory grant requires bounded NODE=ROOT");
				return 2;
			}
			directoryGrants.push_back(
				{std::string(assignment.substr(0, separator)),
				 std::filesystem::path(assignment.substr(separator + 1))}
			);
		}
		assetc::GraphFileHost fileHost(fileGrants, exportSettings.Content, directoryGrants);
		engine::imagegraphexport::GraphDirectoryHost directoryHost(
			directoryGrants, fileGrants, exportSettings.Content
		);
		std::vector<assetc::GraphVideoGrant> videoGrants;
		for (const auto assignment : arguments.GetAll("graph-video-read")) {
			const size_t separator = assignment.find('=');
			if (assignment.size() > 4096 || separator == std::string_view::npos || separator == 0 ||
				separator + 1 == assignment.size()) {
				ENGINE_ERROR("assetc: video grant requires bounded NODE=PATH");
				return 2;
			}
			videoGrants.push_back(
				{std::string(assignment.substr(0, separator)),
				 std::filesystem::path(assignment.substr(separator + 1))}
			);
		}
		assetc::GraphVideoSettings videoSettings;
		videoSettings.Grants = videoGrants;
		videoSettings.Content = exportSettings.Content;
		videoSettings.Timeout = exportSettings.EncoderTimeout;
		if (const auto decoder = arguments.Get("video-decoder"))
			videoSettings.Decoder = std::filesystem::path(*decoder);
		assetc::GraphVideoHost videoHost(videoSettings);
		std::vector<assetc::GraphProcessGrant> processGrants;
		std::vector<std::string> clockGrants;
		std::vector<assetc::GraphHttpGrant> httpGrants;
		if (const auto file = arguments.Get("host-grants")) {
			std::error_code grantError;
			const auto bytes = std::filesystem::file_size(std::filesystem::path(*file), grantError);
			if (grantError || bytes > 1048576) {
				ENGINE_ERROR("assetc: host grants are unavailable or exceed one MiB");
				return 2;
			}
			std::string text(static_cast<size_t>(bytes), '\0');
			std::ifstream stream(std::filesystem::path(*file), std::ios::binary);
			stream.read(text.data(), static_cast<std::streamsize>(text.size()));
			std::string failure;
			if (!stream ||
				!assetc::ReadGraphCommandGrants(text, processGrants, httpGrants, failure, &clockGrants)) {
				ENGINE_ERROR("assetc: invalid host grants {}", failure);
				return 2;
			}
		}
		assetc::GraphCommandHost commands(processGrants, httpGrants, exportSettings.Content, clockGrants);
		class Host final : public engine::imagegraph::HostNodeProvider {
			assetc::GraphFileHost &Files;
			assetc::GraphVideoHost &Videos;
			assetc::GraphCommandHost &Commands;
			engine::imagegraphexport::GraphDirectoryHost &Directories;

		  public:
			Host(
				assetc::GraphFileHost &files,
				assetc::GraphVideoHost &videos,
				assetc::GraphCommandHost &commands,
				engine::imagegraphexport::GraphDirectoryHost &directories
			)
				: Files(files), Videos(videos), Commands(commands), Directories(directories) {}

			bool Capture(
				const engine::imagegraph::HostNodeInvocation &invocation,
				engine::imagegraph::HostNodeCapture &output,
				std::string &failure
			) override {
				if (invocation.Authored.Type == "pc.directory_search")
					return Directories.Capture(invocation, output, failure);
				if (invocation.Authored.Type == "pc.image_mp4" || invocation.Authored.Type == "pc.image_gif")
					return Videos.Capture(invocation, output, failure);
				if (invocation.Authored.Type == "pc.shell" || invocation.Authored.Type == "pc.http_request" ||
					invocation.Authored.Type == "pc.http_request_file" ||
					invocation.Authored.Type == "pc.datetime_get")
					return Commands.Capture(invocation, output, failure);
				return Files.Capture(invocation, output, failure);
			}
		} host(fileHost, videoHost, commands, directoryHost);
		if (!directoryGrants.empty() || !fileGrants.empty() || !videoGrants.empty() ||
			!processGrants.empty() || !httpGrants.empty() || !clockGrants.empty())
			exportSettings.HostProvider = &host;
		std::string failure;
		if (const auto node = arguments.Get("execute-node")) {
			engine::imagegraph::HostNodeCapture capture;
			if (!assetc::ExecuteGraphHostNode(exportSettings, *node, capture, failure)) {
				ENGINE_ERROR("assetc: {}", failure);
				return EXIT_FAILURE;
			}
			ENGINE_INFO("assetc: executed host node {}", *node);
			return EXIT_SUCCESS;
		}
		if (const auto node = arguments.Get("export-node")) {
			std::vector<std::filesystem::path> retainedFrames;
			if (!assetc::ExportAuthoredGraphNode(exportSettings, *node, {}, failure, &retainedFrames)) {
				ENGINE_ERROR("assetc: {}", failure);
				return EXIT_FAILURE;
			}
			ENGINE_INFO("assetc: exported authored node {}", *node);
			for (const auto &directory : retainedFrames)
				ENGINE_INFO("assetc: retained export frames {}", directory.string());
			return EXIT_SUCCESS;
		}
		if (!assetc::ExportGraph(exportSettings, failure)) {
			ENGINE_ERROR("assetc: {}", failure);
			return EXIT_FAILURE;
		}
		ENGINE_INFO("assetc: exported {} to {}", exportSettings.OutputId, exportSettings.Output.string());
		return EXIT_SUCCESS;
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
	if (const auto outputName = arguments.Get("graph-output")) settings.GraphOutput = *outputName;
	const int64_t graphTick = arguments.GetInteger("graph-tick", 0);
	const int64_t graphSeed = arguments.GetInteger("graph-seed", 0);
	if (graphTick < 0 || graphSeed < 0) {
		ENGINE_ERROR("assetc: --graph-tick and --graph-seed must be nonnegative");
		return 2;
	}
	settings.GraphTick = static_cast<uint64_t>(graphTick);
	settings.GraphSeed = static_cast<uint64_t>(graphSeed);
	settings.FlipbookFps = static_cast<float>(arguments.GetNumber("flipbook-fps", settings.FlipbookFps));
	const int64_t flipbookSide = arguments.GetInteger("flipbook-side", settings.FlipbookSide);
	const int64_t flipbookFrames = arguments.GetInteger("flipbook-frames", settings.FlipbookFrames);
	if (flipbookSide < 0 || flipbookSide > std::numeric_limits<uint8_t>::max() || flipbookFrames < 0 ||
		flipbookFrames > 256) {
		ENGINE_ERROR(
			"assetc: --flipbook-side must be between 0 and 255 and --flipbook-frames between 0 and 256"
		);
		return 2;
	}
	settings.FlipbookSide = static_cast<uint8_t>(flipbookSide);
	settings.FlipbookFrames = static_cast<uint16_t>(flipbookFrames);

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
