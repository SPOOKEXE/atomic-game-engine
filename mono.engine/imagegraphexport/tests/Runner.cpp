#include "../../imagegraphphysics/tests/RigidGraphFixture.hpp"

#include <engine/bake/Image.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/RigidReplay.hpp>
#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/imagegraphexport/Runner.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
TEST_SUITE_ID("engine.imagegraphexport.runner")
using engine::imagegraphfont::GraphFontConfiguration;
using engine::imagegraphfont::GraphFontInputs;
using engine::imagegraphfont::WriteGraphFontConfiguration;
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraphphysics.rigid-replay")
TEST_DEPENDS("engine.imagegraphphysics.rigid_graph")
TEST_DEPENDS("engine.bake.image")
TEST_CASE(
	"Live runner reads immutable graph observations without opening the context file", "[imagegraph][export]"
) {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-live-runner-test";
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	Document document;
	document.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}}
	);
	document.Outputs.push_back({"preview", "solid", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 2;
	request.Subframe = .5;
	request.NegativeFrame = true;
	std::vector<std::string> args{
		"imagegraph",
		"--input",
		(root / "does-not-exist.graph").string(),
		"--output-id",
		"preview",
		"--output",
		(root / "image.png").string()
	};
	std::vector<char *> argv;
	for (auto &arg : args)
		argv.push_back(arg.data());
	std::ostringstream output, errors;
	REQUIRE(
		engine::imagegraphexport::runner::RunWithDocument(
			static_cast<int>(argv.size()), argv.data(), output, errors, document, plan, request
		) == 0
	);
	CHECK(std::filesystem::exists(root / "image.png"));
	CHECK(output.str().find("frame=-2.5") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(root / "does-not-exist.graph"));
	CHECK(document.Nodes[0].Values[0].Data == Value{int64_t{1}});
}

TEST_CASE(
	"Runner binds native rigid replay and preserves explicit live observations", "[imagegraph][export][rigid]"
) {
	using namespace engine::imagegraph;
	struct Provider final : HostNodeProvider {
		std::vector<std::pair<bool, bool>> Observations;
		SourceRigidProvider *Expected = nullptr;
		bool Capture(const HostNodeInvocation &in, HostNodeCapture &out, std::string &failure) override {
			if (!in.Request.RigidProvider || (Expected && in.Request.RigidProvider != Expected)) {
				failure = "rigid capability was not retained";
				return false;
			}
			SourceRigidHistory history;
			history.World.Dimension = {32, 32};
			history.Frames.resize(1);
			SourceRigidBody body;
			body.Id = "body";
			body.Position = {16, 16};
			body.Size = {2, 2};
			history.Frames[0].Events.push_back({{"body", 0}, body});
			SourceRigidSnapshot snapshot;
			Diagnostic diagnostic;
			if (in.Request.RigidProvider->Replay(
					history, 0, {}, in.MaximumOperationBytes, snapshot, diagnostic
				) != Status::Ok ||
				snapshot.Bodies.size() != 1) {
				failure = diagnostic.Message;
				return false;
			}
			Observations.emplace_back(in.Request.RigidPlaying, in.Request.RigidFrameProgress);
			out = {};
			out.Authored = in.Authored;
			out.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
			out.Tick = in.Request.Tick;
			out.Subframe = in.Request.Subframe;
			out.NegativeFrame = in.Request.NegativeFrame;
			out.Outputs = {{"path", std::string{"captured"}}, {"dimension", Vector2{1, 1}}};
			out.Images = {{"surface_out", Image{1, 1, {255, 0, 0, 255}}}};
			return true;
		}
	} provider;
	const auto root = std::filesystem::temp_directory_path() / "atomic-rigid-runner-test";
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	Document document;
	document.Nodes = {{"input", "pc.image", "", {}, {{"path", std::string{"captured"}}}}};
	document.Outputs = {{"preview", "input", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto input = root / "input.graph";
	{
		std::ofstream file(input);
		file << Write(document);
	}
	std::vector<std::string> args{
		"imagegraph",
		"--input",
		input.string(),
		"--output-id",
		"preview",
		"--output",
		(root / "image.png").string()
	};
	const auto run = [&](const EvaluationRequest *live) {
		std::vector<char *> argv;
		for (auto &arg : args)
			argv.push_back(arg.data());
		std::ostringstream output, errors;
		const int result =
			live ? engine::imagegraphexport::runner::RunWithDocument(
					   static_cast<int>(argv.size()), argv.data(), output, errors, document, plan, *live
				   )
				 : engine::imagegraphexport::runner::RunWithHostInputs(
					   static_cast<int>(argv.size()), argv.data(), output, errors, {}, {}, &provider
				   );
		INFO(errors.str());
		REQUIRE(result == 0);
	};
	run(nullptr);
	CHECK(provider.Observations.back() == std::pair{false, false});
	args.insert(args.end(), {"--rigid-playing", "--rigid-frame-progress"});
	run(nullptr);
	CHECK(provider.Observations.back() == std::pair{true, true});
	engine::imagegraphphysics::RigidProvider explicitProvider;
	provider.Expected = &explicitProvider;
	EvaluationRequest live;
	live.RigidProvider = &explicitProvider;
	live.RigidPlaying = true;
	live.HostProvider = &provider;
	run(&live);
	CHECK(provider.Observations.back() == std::pair{true, false});
	CHECK(live.RigidProvider == &explicitProvider);
	CHECK_FALSE(live.RigidFrameProgress);
	HostNodeCapture capture;
	std::string failure;
	RigidReplayState prior;
	live.RigidReplay = &prior;
	live.RigidAuthoringRevision = 42;
	REQUIRE(engine::imagegraphexport::ExecuteGraphHostNode(document, plan, live, "input", capture, failure));
	CHECK(provider.Observations.back() == std::pair{true, false});
	CHECK(live.RigidReplay == &prior);
	CHECK(live.RigidAuthoringRevision == 42);
	provider.Expected = nullptr;
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = input;
	settings.HostProvider = &provider;
	settings.RigidPlaying = settings.RigidFrameProgress = true;
	REQUIRE(engine::imagegraphexport::ExecuteGraphHostNode(settings, "input", capture, failure));
	CHECK(provider.Observations.back() == std::pair{true, true});
}

TEST_CASE(
	"File runner exports real rigid actors with paused played and repeatable seek observations",
	"[imagegraph][export][rigid]"
) {
	using namespace engine::imagegraph;
	const auto root = std::filesystem::temp_directory_path() / "atomic-rigid-actor-runner-test";
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	const auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	const auto input = root / "actor.graph";
	{
		std::ofstream file(input);
		file << Write(document);
		REQUIRE(bool(file));
	}
	const auto decode = [&](const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		std::string bytes{std::istreambuf_iterator<char>(file), {}};
		engine::assets::TextureData image;
		std::string failure;
		const auto read = engine::bake::ReadImage(
			{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()}, image, failure
		);
		INFO(failure);
		REQUIRE(read);
		REQUIRE(image.Width == 32);
		REQUIRE(image.Height == 32);
		REQUIRE(image.Pixels.size() == 32 * 32 * 4);
		return image.Pixels;
	};
	const auto run = [&](const std::string &name, const std::vector<std::string> &clock, bool playing) {
		std::vector<std::string> args{
			"imagegraph",
			"--input",
			input.string(),
			"--output-id",
			"image",
			"--output",
			(root / name).string()
		};
		args.insert(args.end(), clock.begin(), clock.end());
		if (playing) args.insert(args.end(), {"--rigid-playing", "--rigid-frame-progress"});
		std::vector<char *> argv;
		for (auto &arg : args)
			argv.push_back(arg.data());
		std::ostringstream output, errors;
		const auto result =
			engine::imagegraphexport::runner::Run(static_cast<int>(argv.size()), argv.data(), output, errors);
		INFO(errors.str());
		REQUIRE(result == 0);
	};
	run("paused.png", {"--tick", "0"}, false);
	const auto paused = decode(root / "paused.png");
	run("played.png", {"--frames", "0:12"}, true);
	const auto first = decode(root / "played.tick-00000000000000000000.png");
	const auto later = decode(root / "played.tick-00000000000000000012.png");
	CHECK(paused != first);
	CHECK(first != later);
	CHECK(std::any_of(first.begin(), first.end(), [](std::byte value) { return value != std::byte{}; }));
	run("repeat.png", {"--tick", "12"}, true);
	CHECK(decode(root / "repeat.png") == later);
	run("seek.png", {"--tick", "0"}, true);
	CHECK(decode(root / "seek.png") == first);
	engine::imagegraphexport::GraphExportSettings settings;
	settings.Input = input;
	settings.Output = root / "shared-export.png";
	settings.OutputId = "image";
	settings.Frames = {12, 12, 1};
	settings.RigidPlaying = settings.RigidFrameProgress = true;
	std::string failure;
	const bool exported = engine::imagegraphexport::ExportGraph(settings, failure);
	INFO(failure);
	REQUIRE(exported);
	CHECK(decode(settings.Output) == later);
}

TEST_CASE(
	"Manual export consumes real rigid same-frame inputs without advancing its journal",
	"[imagegraph][export][rigid]"
) {
	using namespace engine::imagegraph;
	const auto directory = std::filesystem::temp_directory_path() / "atomic-prepared-rigid-export";
	std::filesystem::create_directories(directory);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{directory};
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	document.Nodes.push_back(
		{"export",
		 "pc.export",
		 "rigid",
		 {},
		 {{"directory", directory.string()},
		  {"file_name", std::string{"rigid"}},
		  {"template", std::string{"%d%n"}},
		  {"type", EnumValue{0}}}}
	);
	document.Links.push_back({"render", "surface_out", "export", "surface"});
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	engine::imagegraphphysics::RigidProvider rigidProvider;
	EvaluationRequest request;
	request.RigidProvider = &rigidProvider;
	request.RigidPlaying = request.RigidFrameProgress = true;
	request.RigidAuthoringRevision = 19;
	StatefulEvaluationResult result;
	for (uint64_t tick = 0; tick <= 12; ++tick) {
		request.Tick = tick;
		request.RigidReplay = tick ? &result.Rigid : nullptr;
		const auto evaluated = EvaluateStateful(document, plan, "image", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
	}
	const auto prior = result.Rigid;
	const auto image = std::get<Image>(result.Output);
	struct ExportCapture final : HostNodeProvider {
		const RigidReplayState &Prior;
		const Image &Expected;
		size_t Calls = 0;
		ExportCapture(const RigidReplayState &prior, const Image &image) : Prior(prior), Expected(image) {}
		bool Capture(const HostNodeInvocation &in, HostNodeCapture &capture, std::string &failure) override {
			if (!in.Request.RigidReplay || *in.Request.RigidReplay != Prior ||
				in.Request.RigidFrameProgress || in.Images.size() != 1 || in.Images[0].Port != "surface" ||
				!in.Images[0].Data || SurfaceHash(*in.Images[0].Data) != SurfaceHash(Expected)) {
				failure = "same-frame manual export changed the rigid journal or resolved surface";
				return false;
			}
			++Calls;
			capture = {};
			capture.Authored = in.Authored;
			capture.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
			return true;
		}
	} provider{prior, image};
	request.HostProvider = &provider;
	request.RigidReplay = &prior;
	request.RigidFrameProgress = false;
	for (size_t repeat = 0; repeat < 2; ++repeat) {
		HostNodeCapture capture;
		std::string failure;
		const bool captured = engine::imagegraphexport::ExecuteGraphHostNode(
			document, plan, request, "export", capture, failure
		);
		INFO(failure);
		REQUIRE(captured);
	}
	CHECK(provider.Calls == 2);
	CHECK(request.RigidReplay == &prior);
	CHECK(prior == result.Rigid);
	StatefulInputEvaluationResult prepared;
	const auto status = EvaluateStatefulNodeInputs(document, plan, "export", request, prepared, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	engine::imagegraphexport::GraphExportSettings grants;
	grants.Input = directory / "unused-context.graph";
	grants.Output = directory;
	std::string failure;
	const bool published = engine::imagegraphexport::ExportAuthoredGraphNode(
		document, plan, request, grants, "export", prepared.Inputs, failure
	);
	INFO(failure);
	REQUIRE(published);
	std::ifstream stream(directory / "rigid.png", std::ios::binary);
	const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
	engine::assets::TextureData decoded;
	REQUIRE(
		engine::bake::ReadImage(
			{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()}, decoded, failure
		)
	);
	CHECK(decoded.Width == image.Width);
	CHECK(decoded.Height == image.Height);
	CHECK(
		std::equal(
			decoded.Pixels.begin(),
			decoded.Pixels.end(),
			image.Pixels.begin(),
			image.Pixels.end(),
			[](std::byte left, uint8_t right) { return std::to_integer<uint8_t>(left) == right; }
		)
	);
	CHECK(request.RigidReplay == &prior);
	CHECK(prior == result.Rigid);
}

namespace {
	engine::imagegraph::FontValue RunnerTestFont() {
		using namespace engine::imagegraph;
		FontValue value;
		auto &font = value.Data.emplace();
		font.Characters = FontCharacterProfile::UnicodeScalar;
		font.Frames.push_back({1, 1, {255, 255, 255, 255}});
		font.LineHeight = 1;
		font.MissingAdvance = 1;
		font.SpaceAdvance = 1;
		FontGlyph glyph;
		glyph.Character = 'A';
		glyph.Frame = 0;
		glyph.Advance = glyph.Width = glyph.Height = 1;
		font.Glyphs.push_back(glyph);
		return value;
	}

	engine::imagegraph::Document RunnerFontText(std::string fontPath = {}, uint32_t pixelSize = 16) {
		using namespace engine::imagegraph;
		Document document;
		document.FormatVersion = 9;
		document.Nodes.push_back(
			{"text",
			 "pc.text",
			 "",
			 {},
			 {{"text", std::string{"A"}},
			  {"font", std::move(fontPath)},
			  {"size", int64_t{pixelSize}},
			  {"interpolate", EnumValue{1}},
			  {"oversample", EnumValue{3}}}}
		);
		document.Outputs.push_back({"image", "text", "surface_out"});
		return document;
	}

	std::vector<char *> RunnerArgv(std::vector<std::string> &arguments) {
		std::vector<char *> result;
		result.reserve(arguments.size());
		for (auto &argument : arguments)
			result.push_back(argument.data());
		return result;
	}
}

TEST_CASE(
	"Runner imports an explicit font artifact and binds it on each ranged frame", "[imagegraph][font]"
) {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	const auto root = std::filesystem::temp_directory_path() / "atomic-imagegraph-font-runner-test";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	const auto fontPath = root / "font.bdf";
	constexpr std::string_view bdf = R"(STARTFONT 2.1
FONT -engine-test-medium-r-normal--10-100-72-72-c-80-iso10646-1
SIZE 10 72 72
FONTBOUNDINGBOX 5 7 -1 -2
STARTPROPERTIES 4
FONT_ASCENT 8
FONT_DESCENT 2
CHARSET_REGISTRY "ISO10646"
CHARSET_ENCODING "1"
ENDPROPERTIES
CHARS 2
STARTCHAR A
ENCODING 65
SWIDTH 800 0
DWIDTH 8 0
BBX 5 7 -1 -2
BITMAP
70
88
88
F8
88
88
88
ENDCHAR
STARTCHAR space
ENCODING 32
SWIDTH 400 0
DWIDTH 4 0
BBX 0 0 0 0
BITMAP
ENDCHAR
ENDFONT
)";
	{
		std::ofstream font(fontPath, std::ios::binary);
		font.write(bdf.data(), static_cast<std::streamsize>(bdf.size()));
		auto document = RunnerFontText(fontPath.string(), 10);
		std::ofstream graph(root / "font.graph", std::ios::binary);
		graph << Write(document);
	}
	const auto document = RunnerFontText(fontPath.string(), 10);
	GraphFontConfiguration configuration;
	configuration.Context.AliasMapKnown = true;
	configuration.Context.DefaultFontPath = std::string{};
	configuration.Context.Playing = false;
	configuration.ReadGrants.push_back({"text", fontPath, false, "font"});
	std::string artifact;
	Diagnostic diagnostic;
	REQUIRE(WriteGraphFontConfiguration(configuration, artifact, Limits::MaximumEvaluationBytes, diagnostic));
	{
		std::ofstream file(root / "fonts.inputs", std::ios::binary);
		file.write(artifact.data(), static_cast<std::streamsize>(artifact.size()));
	}
	std::vector<std::string> arguments{
		"imagegraph",
		"--input",
		(root / "font.graph").string(),
		"--font-inputs",
		(root / "fonts.inputs").string(),
		"--output-id",
		"image",
		"--output",
		(root / "frame.png").string(),
		"--frames",
		"0:1"
	};
	auto argv = RunnerArgv(arguments);
	std::ostringstream output, errors;
	const int result = runner::Run(static_cast<int>(argv.size()), argv.data(), output, errors);
	INFO(errors.str());
	REQUIRE(result == 0);
	CHECK(std::filesystem::is_regular_file(root / "frame.tick-00000000000000000000.png"));
	CHECK(std::filesystem::is_regular_file(root / "frame.tick-00000000000000000001.png"));
	GraphFontInputs explicitInputs;
	REQUIRE(explicitInputs.Replace(
		configuration,
		engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
		Limits::MaximumEvaluationBytes,
		diagnostic
	));
	std::vector<std::string> hostArguments{
		"imagegraph",
		"--input",
		(root / "font.graph").string(),
		"--output-id",
		"image",
		"--output",
		(root / "owned.png").string()
	};
	auto hostArgv = RunnerArgv(hostArguments);
	std::ostringstream hostOutput, hostErrors;
	const int hostResult = runner::RunWithHostInputs(
		static_cast<int>(hostArgv.size()),
		hostArgv.data(),
		hostOutput,
		hostErrors,
		{},
		{},
		nullptr,
		explicitInputs
	);
	INFO(hostErrors.str());
	REQUIRE(hostResult == 0);
	CHECK(std::filesystem::is_regular_file(root / "owned.png"));
	GraphFontConfiguration noGrant;
	noGrant.Context.AliasMapKnown = true;
	noGrant.Context.DefaultFontPath = std::string{};
	noGrant.Context.Playing = false;
	std::string noGrantArtifact;
	REQUIRE(
		WriteGraphFontConfiguration(noGrant, noGrantArtifact, Limits::MaximumEvaluationBytes, diagnostic)
	);
	const auto ungrantedGraph = RunnerFontText("/not-granted/font.ttf");
	{
		std::ofstream graph(root / "ungranted.graph", std::ios::binary);
		graph << Write(ungrantedGraph);
		std::ofstream prior(root / "refused.png", std::ios::binary);
		prior << "previous output";
		std::ofstream fontFile(root / "no-grant.inputs", std::ios::binary);
		fontFile.write(noGrantArtifact.data(), static_cast<std::streamsize>(noGrantArtifact.size()));
	}
	std::vector<std::string> deniedArguments{
		"imagegraph",
		"--input",
		(root / "ungranted.graph").string(),
		"--font-inputs",
		(root / "no-grant.inputs").string(),
		"--output-id",
		"image",
		"--output",
		(root / "refused.png").string()
	};
	auto deniedArgv = RunnerArgv(deniedArguments);
	std::ostringstream deniedOutput, deniedErrors;
	CHECK(
		runner::Run(static_cast<int>(deniedArgv.size()), deniedArgv.data(), deniedOutput, deniedErrors) == 1
	);
	std::ifstream prior(root / "refused.png", std::ios::binary);
	CHECK(std::string{std::istreambuf_iterator<char>(prior), {}} == "previous output");
}

TEST_CASE(
	"Live runner preserves caller-owned font observations and provider over a frame range",
	"[imagegraph][font]"
) {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	const auto root = std::filesystem::temp_directory_path() / "atomic-live-font-runner-test";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	const auto document = RunnerFontText("/caller/font.ttf");
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	SourceFontContext context;
	context.AliasMapKnown = true;
	context.DefaultFontPath = std::string{};
	context.Playing = false;
	struct Provider final : SourceFontProvider {
		std::vector<SourceFontRequest> Requests;
		bool Observe(
			const SourceFontRequest &request, uint64_t, SourceFontObservation &output, std::string &
		) override {
			Requests.push_back(request);
			output.Request = request;
			output.Font = RunnerTestFont();
			return true;
		}
	} provider;
	SourceFontObservation unrelated;
	unrelated.Request.Authored = {"other", "pc.text", "", {}, {}};
	unrelated.Request.Context = context;
	unrelated.Request.Role = "font";
	unrelated.Request.ResolvedPath = "/other/font.ttf";
	unrelated.Request.Characters = {'Z'};
	unrelated.Font = RunnerTestFont();
	unrelated.Font->Data->Glyphs.front().Character = 'Z';
	REQUIRE(SourceFontObservationRetainedBytes(unrelated));
	const std::array observations{unrelated};
	EvaluationRequest request;
	request.SourceFonts = &context;
	request.FontObservations = observations;
	request.FontProvider = &provider;
	std::vector<std::string> arguments{
		"imagegraph",
		"--input",
		(root / "unused.graph").string(),
		"--output-id",
		"image",
		"--output",
		(root / "frame.png").string(),
		"--frames",
		"0:1"
	};
	auto argv = RunnerArgv(arguments);
	std::ostringstream output, errors;
	const int result = engine::imagegraphexport::runner::RunWithDocument(
		static_cast<int>(argv.size()), argv.data(), output, errors, document, plan, request
	);
	INFO(errors.str());
	REQUIRE(result == 0);
	REQUIRE(!provider.Requests.empty());
	CHECK(std::all_of(provider.Requests.begin(), provider.Requests.end(), [&](const auto &seen) {
		return seen.Context == context && seen.ResolvedPath == "/caller/font.ttf";
	}));
	CHECK(std::any_of(provider.Requests.begin(), provider.Requests.end(), [](const auto &seen) {
		return seen.Tick == 0;
	}));
	CHECK(std::any_of(provider.Requests.begin(), provider.Requests.end(), [](const auto &seen) {
		return seen.Tick == 1;
	}));
	CHECK(context.Playing == false);
	CHECK(request.FontObservations.data() == observations.data());
	CHECK(request.FontProvider == &provider);
	GraphFontConfiguration conflicting;
	conflicting.Context.AliasMapKnown = true;
	std::string artifact;
	REQUIRE(WriteGraphFontConfiguration(conflicting, artifact, Limits::MaximumEvaluationBytes, diagnostic));
	{
		std::ofstream file(root / "conflicting.inputs", std::ios::binary);
		file.write(artifact.data(), static_cast<std::streamsize>(artifact.size()));
	}
	arguments.insert(arguments.end(), {"--font-inputs", (root / "conflicting.inputs").string()});
	argv = RunnerArgv(arguments);
	std::ostringstream conflictOutput, conflictErrors;
	CHECK(
		engine::imagegraphexport::runner::RunWithDocument(
			static_cast<int>(argv.size()),
			argv.data(),
			conflictOutput,
			conflictErrors,
			document,
			plan,
			request
		) == 2
	);
	CHECK(conflictErrors.str().find("conflicts with caller-owned font inputs") != std::string::npos);
	request.SourceFonts = nullptr;
	request.FontObservations = {};
	request.FontProvider = nullptr;
	request.SourceFontHostResidentBytes = 1;
	std::ostringstream residentConflictOutput, residentConflictErrors;
	CHECK(
		engine::imagegraphexport::runner::RunWithDocument(
			static_cast<int>(argv.size()),
			argv.data(),
			residentConflictOutput,
			residentConflictErrors,
			document,
			plan,
			request
		) == 2
	);
	CHECK(residentConflictErrors.str().find("conflicts with caller-owned font inputs") != std::string::npos);
}
