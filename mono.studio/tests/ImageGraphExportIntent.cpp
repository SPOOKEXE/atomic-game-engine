#include "ImageGraphExportIntent.hpp"

#include "ImageGraphFontArtifact.hpp"
#include "ImageGraphFontBindings.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("studio.imagegraph.export_intent")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.scripthost.composer_lua")
namespace {
	using namespace engine::imagegraph;
	FontValue InitialTextFont() {
		FontValue value;
		auto &font = value.Data.emplace();
		font.LineHeight = 1;
		font.Frames = {{1, 1, {255, 255, 255, 255}}};
		FontGlyph glyph;
		glyph.Character = 'A';
		glyph.Frame = 0;
		glyph.Width = 1;
		glyph.Height = 1;
		glyph.Advance = 1;
		font.Glyphs = {glyph};
		return value;
	}
	DataReplayState TextReplay(const FontValue &font) {
		StructValue text;
		auto &fields = text.Data.emplace().Fields;
		fields = {{"primary", font}, {"fallback", UndefinedValue{}}};
		DataReplayState replay;
		DataReplayEntry entry;
		entry.NodeId = "text-node";
		entry.Initialized = true;
		entry.Tick = 4;
		entry.Values = {{4, Value{text}}};
		replay.Entries.push_back(std::move(entry));
		return replay;
	}
	struct FontRangeHost final : engine::imagegraphexport::GraphExportSessionHost {
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &failure) override {
			failure = "Text range fixture does not use host captures";
			return false;
		}
		bool Pending() const noexcept override {
			return false;
		}
		void Cancel() noexcept override {}
	};
	struct CountedLua final : ComposerLuaHost {
		std::unique_ptr<ComposerLuaHost> Host = engine::script::MakeComposerLuaHost();
		size_t Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &capture, std::string &failure
		) override {
			++Calls;
			return Host->Capture(invocation, capture, failure);
		}
		bool PcxMessages(
			std::string_view node, std::span<const PcxMessage> messages, std::string &failure
		) override {
			return Host->PcxMessages(node, messages, failure);
		}
		std::vector<ComposerLuaMessage> TakeMessages() override {
			return Host->TakeMessages();
		}
		void Reset() override {
			Host->Reset();
		}
	};
	Document Graph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"lua",
			 "pc.lua_compute",
			 {},
			 {},
			 {{"lua_code", std::string("counter=(counter or 0)+1 print(counter) return counter")},
			  {"function_name", std::string("pendingArgument")},
			  {"execute_on_frame", true}}},
			{"hlsl", "pc.hlsl", {}, {}, {}},
			{"base",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{1, 2, 3, 255}}}},
			{"export", "pc.export", {}, {}, {{"type", EnumValue{0}}, {"export_on_update", true}}}
		};
		doc.Nodes[1].DynamicInputs = {
			{"argument_name_0", ValueType::Text, std::string("gain")},
			{"argument_type_0", ValueType::Enum, EnumValue{0}},
			{"argument_value_0", ValueType::Scalar, 0.0}
		};
		doc.Links = {
			{"lua", "return_value", "hlsl", "argument_value_0"},
			{"lua", "return_value", "export", "sequence_begin"},
			{"hlsl", "surface", "export", "surface"}
		};
		doc.Outputs = {{"out", "hlsl", "surface"}};
		return doc;
	}
} // namespace

namespace {
	// Delayed explicit capability response exercises caller retry ownership. It is
	// independent of the renderer; device completion remains a separate real gate.
	struct DelayedSurface final : engine::imagegraph::HostNodeProvider {
		unsigned Remaining = 3, Attempts = 0, DelayEachFrame = 0;
		std::optional<engine::imagegraph::FrameTime> SeenFrame;
		bool Pending = false;
		std::vector<engine::imagegraph::FrameTime> Frames;
		std::vector<engine::imagegraph::Value> Values;
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			using namespace engine::imagegraph;
			++Attempts;
			const auto frame = GetFrameTime(invocation.Request);
			if (DelayEachFrame && SeenFrame != frame) {
				Remaining = DelayEachFrame;
				SeenFrame = frame;
			}
			Pending = false;
			Frames.push_back(GetFrameTime(invocation.Request));
			const auto value =
				std::find_if(invocation.Inputs.begin(), invocation.Inputs.end(), [](const auto &input) {
					return input.Port == "argument_value_0";
				});
			if (value == invocation.Inputs.end()) {
				failure = "Missing captured Lua argument";
				return false;
			}
			Values.push_back(value->Data);
			if (Remaining) {
				--Remaining;
				Pending = true;
				failure = "Surface capture pending";
				return false;
			}
			uint64_t bytes = 0;
			Diagnostic error;
			HostNodeCapture candidate;
			if (PrepareResolvedHostCapture(
					invocation, invocation.MaximumOperationBytes, candidate, bytes, error
				) != Status::Ok) {
				failure = error.Message;
				return false;
			}
			Image image{1, 1, {17, 31, 49, 255}};
			image.Hash = SurfaceHash(image);
			candidate.Images.push_back({"surface", image});
			output = std::move(candidate);
			return true;
		}
	};
} // namespace
TEST_CASE(
	"Pending current-frame export preserves genuine Lua receipts and the "
	"target cursor",
	"[studio][export_intent]"
) {
	using namespace engine::imagegraph;
	using namespace studio::detail;
	const auto root =
		std::filesystem::temp_directory_path() /
		("pc-export-intent-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	REQUIRE(std::filesystem::create_directory(root));
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};
	auto doc = Graph();
	doc.Nodes.back().Values.insert(
		doc.Nodes.back().Values.end(),
		{{"directory", root.string()},
		 {"file_name", std::string("result")},
		 {"template", std::string("%d%n_%f")}}
	);
	Plan plan;
	Diagnostic error;
	const auto status = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	CountedLua lua;
	DelayedSurface delayed;
	ImageGraphHost host;
	host.Lua = &lua;
	host.Composer = &delayed;
	ImageGraphExportIntent intent;
	ImageGraphComposerExports::Observation observation;
	observation.Revision = 4;
	observation.InputRevision = 7;
	REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {12, .25}));
	const engine::core::Name owner("export-world");
	const ImageGraphExportIntent::Target targets[] = {
		{"export", root.string(), {}, {}}, {"second", {}, {}, {}}
	};
	std::string failure;
	REQUIRE(intent.Begin(ImageGraphExportIntent::Kind::Authored, owner, observation, targets, true, failure));
	EvaluationRequest request;
	ImageGraphExportProvider provider(host, intent, request);
	request.HostProvider = &provider;
	REQUIRE(SetFrameTime(request, GetImageGraphFrame(intent.Current->Observation.Playback)));
	unsigned Publications = 0;
	for (uint64_t playback = 13; playback <= 16; ++playback) {
		REQUIRE(intent.Matches(owner, 4, 7));
		REQUIRE(intent.Selected()->NodeId == "export");
		EvaluationSnapshot inputs;
		intent.Observations.BeginAttempt();
		const auto captured = EvaluateNodeInputs(doc, plan, "export", request, inputs, error);
		if (delayed.Pending) {
			CHECK(captured != Status::Ok);
			CHECK_FALSE(intent.Finish(ImageGraphExportIntent::Result::Pending));
			CHECK(intent.Current->Cursor == 0);
			CHECK(intent.Pending);
		} else {
			INFO(error.Message);
			REQUIRE(captured == Status::Ok);
			REQUIRE(inputs.Images().size() == 1);
			CHECK(inputs.Images()[0].Data.Pixels == std::vector<uint8_t>{17, 31, 49, 255});
			engine::imagegraphexport::GraphExportSettings grants;
			grants.Input = root / "unsaved.graph";
			grants.Output = root;
			const auto exported = engine::imagegraphexport::ExportAuthoredGraphNode(
				doc, plan, request, grants, "export", inputs, failure
			);
			INFO(failure);
			REQUIRE(exported);
			++Publications;
			CHECK_FALSE(intent.Finish(ImageGraphExportIntent::Result::Complete));
			CHECK(intent.Current->Cursor == 1);
		}
		// Authoritative playback advances, but the export request is never
		// relabeled.
		CHECK(GetFrameTime(request) == FrameTime{12, .25});
		CHECK(playback > request.Tick);
		CHECK(lua.Calls == 1);
	}
	CHECK(Publications == 1);
	size_t pngFiles = 0;
	for (const auto &item : std::filesystem::directory_iterator(root))
		if (item.path().extension() == ".png") {
			++pngFiles;
			CHECK(item.file_size() > 0);
		}
	CHECK(pngFiles == 1);
	CHECK_FALSE(std::filesystem::exists(root / "unsaved.graph"));
	CHECK(intent.Selected()->NodeId == "second");
	CHECK(delayed.Attempts == 4);
	for (const auto frame : delayed.Frames)
		CHECK(frame == FrameTime{12, .25});
	for (const auto &value : delayed.Values)
		CHECK(value == Value{1.0});
	CHECK(lua.TakeMessages().size() == 1);
	CHECK(intent.Finish(ImageGraphExportIntent::Result::Failed));
	// Audio/input reload invalidates the intent before any current spans bind.
	CHECK_FALSE(intent.Matches(owner, 4, 8));
	intent.Clear();
	CHECK_FALSE(intent.Current);
	CHECK_FALSE(intent.Observations.Active);
}
TEST_CASE(
	"Export intent metadata admission is atomic and completed targets "
	"never rewind",
	"[studio][export_intent]"
) {
	using namespace studio::detail;
	ImageGraphExportIntent intent;
	ImageGraphComposerExports::Observation observation;
	observation.Revision = 1;
	observation.InputRevision = 2;
	const auto owner = engine::core::Name("export-world");
	std::vector<ImageGraphExportIntent::Target> targets(64);
	for (size_t i = 0; i < targets.size(); ++i) {
		targets[i].NodeId = "node" + std::to_string(i);
		targets[i].Root = std::string(2048, 'r');
	}
	std::string failure;
	CHECK_FALSE(
		intent.Begin(ImageGraphExportIntent::Kind::Authored, owner, observation, targets, true, failure, 4096)
	);
	CHECK_FALSE(intent.Current);
	REQUIRE(intent.Begin(ImageGraphExportIntent::Kind::Authored, owner, observation, targets, true, failure));
	for (size_t i = 0; i < targets.size(); ++i) {
		CHECK(intent.Current->Cursor == i);
		CHECK_FALSE(intent.Finish(ImageGraphExportIntent::Result::Pending));
		CHECK(intent.Current->Cursor == i);
		CHECK(intent.Finish(ImageGraphExportIntent::Result::Complete) == (i + 1 == targets.size()));
	}
	CHECK_FALSE(intent.Selected());
	CHECK_FALSE(intent.Finish(ImageGraphExportIntent::Result::Complete));
}

TEST_CASE(
	"Export preparation advances only completed clocks and preserves "
	"contiguous history beyond seek bounds",
	"[studio][export_intent]"
) {
	using namespace engine::imagegraph;
	using namespace studio::detail;
	ImageGraphExportPreparation cursor;
	std::string failure;
	REQUIRE(cursor.Begin({}, {3, .5}, true, failure));
	CHECK(cursor.Current == std::optional(FrameTime{}));
	// An outstanding producer does not publish the current tick.
	CHECK(cursor.Current == std::optional(FrameTime{}));
	CHECK_FALSE(cursor.Complete());
	CHECK(cursor.Current == std::optional(FrameTime{1}));
	CHECK_FALSE(cursor.Complete());
	CHECK(cursor.Current == std::optional(FrameTime{2}));
	CHECK_FALSE(cursor.Complete());
	CHECK(cursor.Current == std::optional(FrameTime{3, .5}));
	CHECK(cursor.Complete());
	cursor.Clear();
	REQUIRE(cursor.Begin(FrameTime{4999}, {5000}, true, failure));
	CHECK(cursor.Current == std::optional(FrameTime{5000}));
	CHECK(cursor.Complete());
	cursor.Clear();
	CHECK_FALSE(cursor.Begin({}, {5000}, true, failure));
	CHECK_FALSE(cursor.Current);
	REQUIRE(cursor.Begin(FrameTime{2, .75}, {2, .5}, true, failure));
	CHECK(cursor.Current == std::optional(FrameTime{}));
	cursor.Clear();
	REQUIRE(cursor.Begin(FrameTime{3}, {2, .5, true}, false, failure));
	CHECK(cursor.Current == std::optional(FrameTime{2, .5, true}));
	CHECK(cursor.Complete());
}

TEST_CASE(
	"Cold export admission warms genuine feedback one completed frame at "
	"a time without repeating Lua",
	"[studio][export_intent]"
) {
	using namespace engine::imagegraph;
	using namespace studio::detail;
	auto doc = Graph();
	doc.Project = ProjectSettings{};
	doc.Project->SurfaceWidth = doc.Project->SurfaceHeight = 1;
	doc.Nodes[2] = {"base", "image.captured", "", {}, {{"source_id", std::string("feedback:out")}}};
	doc.Nodes[1].DynamicInputs.insert(
		doc.Nodes[1].DynamicInputs.end(),
		{{"argument_name_1", ValueType::Text, std::string("source")},
		 {"argument_type_1", ValueType::Enum, EnumValue{7}},
		 {"argument_value_1", ValueType::Image, std::nullopt}}
	);
	doc.Links.push_back({"base", "image", "hlsl", "argument_value_1"});
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message << " " << error.NodeId << "/" << error.Port);
	REQUIRE(compiled == Status::Ok);
	CountedLua lua;
	DelayedSurface delayed;
	delayed.DelayEachFrame = 1;
	ImageGraphHost host;
	host.Lua = &lua;
	host.Composer = &delayed;
	ImageGraphExportIntent intent;
	ImageGraphComposerExports::Observation observation;
	observation.Revision = 5;
	observation.InputRevision = 9;
	REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {3}));
	const ImageGraphExportIntent::Target target{"export", {}, {}, {}};
	std::string failure;
	REQUIRE(intent.Begin(
		ImageGraphExportIntent::Kind::Authored,
		engine::core::Name("warmup-owner"),
		observation,
		std::span(&target, 1),
		false,
		failure
	));
	CapturedFeedbackHost feedback;
	REQUIRE(intent.Preparation.Begin(feedback.PreparedFrame(5, 9), {3}, true, failure));
	for (uint64_t tick = 0; tick <= 3; ++tick) {
		REQUIRE(intent.Preparation.Current == std::optional(FrameTime{tick}));
		for (unsigned attempt = 0; attempt < 2; ++attempt) {
			EvaluationRequest request;
			REQUIRE(SetFrameTime(request, *intent.Preparation.Current));
			ImageGraphExportProvider provider(host, intent, request);
			provider.PendingFlag = &delayed.Pending;
			request.HostProvider = &provider;
			intent.Observations.BeginAttempt();
			const auto before = feedback.PreparedFrame(5, 9);
			const auto prepared = feedback.Prepare(
				doc, plan, 5, 9, request, error, Limits::MaximumEvaluationBytes, "out", "export"
			);
			if (attempt == 0) {
				CHECK_FALSE(prepared);
				CHECK(provider.Pending());
				CHECK(feedback.PreparedFrame(5, 9) == before);
				CHECK(intent.Preparation.Current == std::optional(FrameTime{tick}));
			} else {
				INFO(error.Message);
				REQUIRE(prepared);
				CHECK_FALSE(provider.Pending());
				CHECK(feedback.PreparedFrame(5, 9) == std::optional(FrameTime{tick}));
				CHECK(intent.Preparation.Complete() == (tick == 3));
				intent.Observations.Clear();
			}
			CHECK(lua.Calls == tick + 1);
		}
	}
	CHECK(delayed.Attempts == 8);
	CHECK(lua.TakeMessages().size() == 4);
	REQUIRE(feedback.Snapshot().Images().size() == 1);
	CHECK(feedback.Snapshot().Images()[0].Data.Pixels == std::vector<uint8_t>{17, 31, 49, 255});
	for (size_t attempt = 0; attempt < delayed.Frames.size(); ++attempt) {
		CHECK(delayed.Frames[attempt] == FrameTime{attempt / 2});
		CHECK(delayed.Values[attempt] == Value{double(attempt / 2 + 1)});
	}
}

TEST_CASE(
	"Range payload admission includes held small controls and rejects "
	"overflow before session creation",
	"[export_intent]"
) {
	using studio::detail::ImageGraphExportIntent;
	std::string failure;
	const std::array<uint64_t, 7> held{128, 256, 512, 1024, 2048, 4096, 8192};
	uint64_t exact = 1024;
	for (const auto bytes : held)
		exact += bytes;
	CHECK(ImageGraphExportIntent::AdmitRangePayload(held, 1024, failure, exact));
	CHECK_FALSE(ImageGraphExportIntent::AdmitRangePayload(held, 1024, failure, exact - 1));
	const std::array<uint64_t, 2> overflow{1, UINT64_MAX};
	CHECK_FALSE(ImageGraphExportIntent::AdmitRangePayload(overflow, 1, failure, UINT64_MAX));
	CHECK_FALSE(ImageGraphExportIntent::AdmitRangePayload({}, 2, failure, 1));
}

TEST_CASE("Terminal export observations consume only their queued event", "[export_intent]") {
	using namespace studio::detail;
	ImageGraphComposerExports queue;
	ImageGraphComposerExports::Observation observation;
	observation.Revision = 4;
	observation.InputRevision = 8;
	std::string failure;
	REQUIRE(queue.Admit(observation, failure));
	const auto emptyExportEvent = queue.Pending.front();
	// The actual zero-export document has no target to retain.
	const Document noExports;
	CHECK(std::none_of(noExports.Nodes.begin(), noExports.Nodes.end(), [](const auto &node) {
		return node.Type == "pc.export";
	}));
	CHECK(queue.CompleteFront(emptyExportEvent));
	REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {1}));
	REQUIRE(queue.Admit(observation, failure));
	const auto admissionEvent = queue.Pending.front();
	ImageGraphExportIntent intent;
	const ImageGraphExportIntent::Target target{"export", {}, {}, {}};
	CHECK_FALSE(intent.Begin(
		ImageGraphExportIntent::Kind::Authored,
		engine::core::Name("queue-owner"),
		admissionEvent,
		std::span(&target, 1),
		true,
		failure,
		1
	));
	CHECK(queue.CompleteFront(admissionEvent));
	REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {2}));
	REQUIRE(queue.Admit(observation, failure));
	const auto prepareEvent = queue.Pending.front();
	Document doc = Graph();
	Plan plan;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(Compile(doc, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	CHECK_FALSE(host.Prepare(
		doc, plan, 4, 8, request, diagnostic, Limits::MaximumEvaluationBytes, "missing-output", "export"
	));
	CHECK(queue.CompleteFront(prepareEvent));
	REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {3}));
	REQUIRE(queue.Admit(observation, failure));
	const auto next = queue.Pending.front().Sequence;
	CHECK_FALSE(queue.CompleteFront(emptyExportEvent));
	CHECK_FALSE(queue.CompleteFront(admissionEvent));
	CHECK_FALSE(queue.CompleteFront(prepareEvent));
	REQUIRE(queue.Pending.size() == 1);
	CHECK(queue.Pending.front().Sequence == next);
}

TEST_CASE("Pending export keeps its event until terminal cancellation", "[export_intent]") {
	using namespace studio::detail;
	ImageGraphComposerExports queue;
	ImageGraphComposerExports::Observation observation;
	observation.Revision = 1;
	observation.InputRevision = 2;
	std::string failure;
	REQUIRE(queue.Admit(observation, failure));
	ImageGraphExportIntent intent;
	const ImageGraphExportIntent::Target target{"export", {}, {}, {}};
	REQUIRE(intent.Begin(
		ImageGraphExportIntent::Kind::Authored,
		engine::core::Name("pending-queue-owner"),
		queue.Pending.front(),
		std::span(&target, 1),
		true,
		failure
	));
	CHECK_FALSE(intent.Finish(ImageGraphExportIntent::Result::Pending));
	REQUIRE(queue.Pending.size() == 1);
	REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {1}));
	REQUIRE(queue.Admit(observation, failure));
	const auto next = queue.Pending.back().Sequence;
	REQUIRE(intent.Current);
	const auto canceled = intent.Current->Observation;
	CHECK(queue.CompleteFront(canceled));
	intent.Clear();
	REQUIRE(queue.Pending.size() == 1);
	CHECK(queue.Pending.front().Sequence == next);
	CHECK_FALSE(queue.CompleteFront(canceled));
	CHECK(queue.Pending.front().Sequence == next);
}

TEST_CASE("Export warmup admits at most 4096 inclusive frames", "[export_intent]") {
	studio::detail::ImageGraphExportPreparation preparation;
	std::string failure;
	REQUIRE(preparation.Begin({}, {4095}, true, failure));
	size_t count = 1;
	while (!preparation.Complete())
		++count;
	CHECK(count == 4096);
	preparation.Clear();
	CHECK_FALSE(preparation.Begin({}, {4096}, true, failure));
	CHECK_FALSE(preparation.Current);
	REQUIRE(preparation.Begin(FrameTime{0}, {4096}, true, failure));
	count = 1;
	while (!preparation.Complete())
		++count;
	CHECK(count == 4096);
}

TEST_CASE("Export intent freezes owned font context and exact grants", "[export_intent][font]") {
	using engine::imagegraphfont::GraphFontConfiguration;
	using studio::detail::ImageGraphExportIntent;
	GraphFontConfiguration fonts;
	fonts.Context.AliasMapKnown = true;
	fonts.Context.DefaultFontPath = "assets/fonts/regular.ttf";
	fonts.Context.Aliases.emplace_back("SourceSans", "assets/fonts/source-sans.ttf");
	fonts.ReadGrants.push_back({"text-node", "assets/fonts/regular.ttf", false});
	auto observation = studio::detail::ImageGraphComposerExports::Observation{};
	observation.Playback.CurrentTick = 3;
	observation.Playback.Playing = true;
	const ImageGraphExportIntent::Target target{"export", {}, {}, {}};
	ImageGraphExportIntent intent;
	std::string failure;
	REQUIRE(intent.Begin(
		ImageGraphExportIntent::Kind::Authored,
		engine::core::Name("font-export-owner"),
		observation,
		std::span(&target, 1),
		false,
		failure,
		ImageGraphExportIntent::MaximumBytes,
		studio::detail::ImageGraphExportEvent::Save,
		&fonts
	));
	fonts.Context.DefaultFontPath = "assets/fonts/replacement.ttf";
	fonts.ReadGrants.clear();
	observation.Playback.CurrentTick = 8;
	observation.Playback.Playing = false;
	REQUIRE(intent.Current);
	CHECK(intent.Current->Observation.Playback.CurrentTick == 3);
	CHECK(intent.Current->Observation.Playback.Playing);
	REQUIRE(intent.Current->FontConfiguration);
	CHECK(intent.Current->FontConfiguration->Context.DefaultFontPath == "assets/fonts/regular.ttf");
	REQUIRE(intent.Current->FontConfiguration->ReadGrants.size() == 1);
	CHECK(intent.Current->FontConfiguration->ReadGrants.front().NodeId == "text-node");
	CHECK_FALSE(intent.Current->FontConfiguration->ReadGrants.front().Write);
}

TEST_CASE("Studio font bindings retain a held playback context", "[export_intent][font]") {
	using engine::assets::ContentPolicy;
	using engine::imagegraph::EvaluationRequest;
	using engine::imagegraph::Limits;
	using engine::imagegraph::SourceFontContext;
	using engine::imagegraphfont::GraphFontConfiguration;
	using engine::imagegraphfont::GraphFontInputs;
	GraphFontConfiguration configuration;
	configuration.Context.AliasMapKnown = true;
	GraphFontInputs inputs;
	Diagnostic diagnostic;
	REQUIRE(inputs.Replace(configuration, ContentPolicy{}, Limits::MaximumEvaluationBytes, diagnostic));
	SourceFontContext heldContext;
	const uint64_t sourceBytes =
		engine::imagegraph::SourceFontContextRetainedBytes(configuration.Context).value();
	const uint64_t heldBytes = engine::imagegraph::SourceFontContextRetainedBytes(heldContext).value();
	const uint64_t bindBytes = inputs.RetainedBytes() + heldBytes + sourceBytes * 2;
	EvaluationRequest request;
	CHECK_FALSE(inputs.Bind(true, heldContext, request, bindBytes - 1, diagnostic));
	REQUIRE(inputs.Bind(true, heldContext, request, bindBytes, diagnostic));
	CHECK(request.SourceFonts == &heldContext);
	CHECK(heldContext.Playing == true);
	CHECK(request.FontProvider != nullptr);
	CHECK(request.FontObservations.empty());
	CHECK(request.SourceFontHostResidentBytes == inputs.RetainedBytes());
}

TEST_CASE("Studio refuses a font-bound operation without changing its request", "[export_intent][font]") {
	using engine::imagegraph::EvaluationRequest;
	using engine::imagegraph::SourceFontContext;
	using engine::imagegraphfont::GraphFontConfiguration;
	using engine::imagegraphfont::GraphFontInputs;
	GraphFontConfiguration configuration;
	GraphFontInputs inputs;
	Diagnostic diagnostic;
	REQUIRE(inputs.Replace(
		configuration,
		engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
		engine::imagegraph::Limits::MaximumEvaluationBytes,
		diagnostic
	));
	SourceFontContext heldContext;
	EvaluationRequest request;
	const auto original = &heldContext;
	request.SourceFonts = original;
	CHECK_FALSE(
		studio::detail::BindImageGraphFontInputs(inputs, true, true, heldContext, request, 1, diagnostic)
	);
	CHECK(request.SourceFonts == original);
	CHECK(request.FontProvider == nullptr);
	CHECK(request.FontObservations.empty());
}

TEST_CASE(
	"Studio font artifact staging preserves an existing file on write failure", "[export_intent][font]"
) {
	const auto root = std::filesystem::temp_directory_path() /
					  ("atomic-studio-font-artifact-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code error;
	REQUIRE(std::filesystem::create_directory(root, error));
	struct RemoveDirectory {
		std::filesystem::path Path;
		~RemoveDirectory() {
			std::error_code ignored;
			std::filesystem::remove_all(Path, ignored);
		}
	} cleanup{root};
	const auto destination = root / "inputs.json";
	{
		std::ofstream file(destination, std::ios::binary);
		REQUIRE(file.write("previous artifact", 17));
	}
	Diagnostic diagnostic;
	CHECK_FALSE(
		studio::detail::PublishImageGraphFontArtifact(
			destination,
			[](const std::filesystem::path &staged) {
				std::ofstream file(staged, std::ios::binary);
				return bool(file.write("partial", 7)) && false;
			},
			diagnostic
		)
	);
	std::ifstream file(destination, std::ios::binary);
	const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	CHECK(bytes == "previous artifact");
	CHECK(!studio::detail::PublishImageGraphFontArtifact(
		destination,
		[](const std::filesystem::path &staged) {
			std::ofstream file(staged, std::ios::binary);
			return bool(file.write("complete replacement", 20));
		},
		diagnostic,
		[](const std::filesystem::path &, const std::filesystem::path &, std::error_code &error) {
			error = std::make_error_code(std::errc::permission_denied);
			return false;
		}
	));
	std::ifstream unchanged(destination, std::ios::binary);
	const std::string afterFailedPublish{
		std::istreambuf_iterator<char>(unchanged), std::istreambuf_iterator<char>()
	};
	CHECK(afterFailedPublish == "previous artifact");
	CHECK(
		studio::detail::PublishImageGraphFontArtifact(
			destination,
			[](const std::filesystem::path &staged) {
				std::ofstream file(staged, std::ios::binary);
				return bool(file.write("replacement", 11));
			},
			diagnostic
		)
	);
	std::ifstream replaced(destination, std::ios::binary);
	const std::string replacement{std::istreambuf_iterator<char>(replaced), std::istreambuf_iterator<char>()};
	CHECK(replacement == "replacement");
}

TEST_CASE(
	"Studio freezes only matching Text fonts into held-playing export inputs", "[export_intent][font]"
) {
	using engine::imagegraphfont::GraphFontConfiguration;
	using studio::detail::FreezeImageGraphInitialTextFontsFromReplay;
	Document document;
	document.Nodes = {{"text-node", "pc.text", {}, {}, {}}, {"atlas-node", "pc.text_atlas", {}, {}, {}}};
	const auto font = InitialTextFont();
	const auto replay = TextReplay(font);
	GraphFontConfiguration configuration;
	configuration.Context.AliasMapKnown = true;
	Diagnostic diagnostic;
	const FrameTime heldFrame{4, 0, false};
	const auto bytes = engine::imagegraphfont::GraphFontConfigurationRetainedBytes(configuration);
	REQUIRE(bytes);
	const uint64_t replayBytes = RetainedDataReplayBytes(replay);
	const uint64_t seedBytes = sizeof(SourceFontInitialTextState) + std::string("text-node").size() + 1 +
							   SourceFontValueRetainedBytes(font).value() +
							   2 * sizeof(SourceFontInitialTextState);
	const uint64_t validationWorkspace = replay.Entries.size() * sizeof(size_t);
	const uint64_t requiredBytes = replayBytes + 2 * bytes.value() + seedBytes + validationWorkspace;
	GraphFontConfiguration byteShort;
	CHECK_FALSE(FreezeImageGraphInitialTextFontsFromReplay(
		document, replay, heldFrame, byteShort, requiredBytes - 1, diagnostic
	));
	CHECK(byteShort.Context.InitialTextFonts.empty());
	REQUIRE(FreezeImageGraphInitialTextFontsFromReplay(
		document, replay, heldFrame, configuration, requiredBytes, diagnostic
	));
	REQUIRE(configuration.Context.InitialTextFonts.size() == 1);
	CHECK(configuration.Context.InitialTextFonts.front().NodeId == "text-node");
	CHECK(configuration.Context.InitialTextFonts.front().Primary == font);
	CHECK_FALSE(configuration.Context.InitialTextFonts.front().Fallback);
	const uint64_t frozenBytes =
		engine::imagegraphfont::GraphFontConfigurationRetainedBytes(configuration).value();
	REQUIRE(
		studio::detail::ImageGraphFontConfigurationFitsBudget(
			configuration, bytes.value(), bytes.value() + frozenBytes, diagnostic
		)
	);
	CHECK_FALSE(
		studio::detail::ImageGraphFontConfigurationFitsBudget(
			configuration, bytes.value(), bytes.value() + frozenBytes - 1, diagnostic
		)
	);

	auto observation = studio::detail::ImageGraphComposerExports::Observation{};
	observation.Playback.CurrentTick = heldFrame.Tick;
	observation.Playback.Playing = true;
	const studio::detail::ImageGraphExportIntent::Target target{"export-node", {}, {}, {}};
	studio::detail::ImageGraphExportIntent intent;
	std::string failure;
	REQUIRE(intent.Begin(
		studio::detail::ImageGraphExportIntent::Kind::Authored,
		engine::core::Name("text-font-export-owner"),
		observation,
		std::span(&target, 1),
		false,
		failure,
		studio::detail::ImageGraphExportIntent::MaximumBytes,
		studio::detail::ImageGraphExportEvent::Save,
		&configuration
	));
	REQUIRE(intent.Current->FontConfiguration);
	CHECK(intent.Current->Observation.Playback.Playing);
	CHECK(
		intent.Current->FontConfiguration->Context.InitialTextFonts == configuration.Context.InitialTextFonts
	);

	const auto unchanged = configuration;
	CHECK_FALSE(FreezeImageGraphInitialTextFontsFromReplay(
		document,
		replay,
		FrameTime{5, 0, false},
		configuration,
		engine::imagegraph::Limits::MaximumEvaluationBytes,
		diagnostic
	));
	CHECK(configuration.Context.InitialTextFonts == unchanged.Context.InitialTextFonts);
	CHECK_FALSE(FreezeImageGraphInitialTextFontsFromReplay(
		document, replay, heldFrame, configuration, bytes.value(), diagnostic
	));
	CHECK(configuration.Context.InitialTextFonts == unchanged.Context.InitialTextFonts);

	engine::imagegraph::CapturedFeedbackHost unprepared;
	CHECK_FALSE(
		studio::detail::FreezePreparedImageGraphInitialTextFonts(
			document,
			unprepared,
			1,
			1,
			heldFrame,
			configuration,
			engine::imagegraph::Limits::MaximumEvaluationBytes,
			diagnostic
		)
	);
	auto duplicate = replay;
	duplicate.Entries.push_back(duplicate.Entries.front());
	CHECK_FALSE(FreezeImageGraphInitialTextFontsFromReplay(
		document,
		duplicate,
		heldFrame,
		configuration,
		engine::imagegraph::Limits::MaximumEvaluationBytes,
		diagnostic
	));
	CHECK(configuration.Context.InitialTextFonts == unchanged.Context.InitialTextFonts);
	auto malformed = replay;
	malformed.Entries.front().Values.back().Data = double{1};
	CHECK_FALSE(FreezeImageGraphInitialTextFontsFromReplay(
		document,
		malformed,
		heldFrame,
		configuration,
		engine::imagegraph::Limits::MaximumEvaluationBytes,
		diagnostic
	));
	CHECK(configuration.Context.InitialTextFonts == unchanged.Context.InitialTextFonts);
	auto malformedFont = replay;
	auto &malformedState = std::get<StructValue>(malformedFont.Entries.front().Values.back().Data);
	std::get<FontValue>(malformedState.Data->Fields.front().second).Data->Glyphs.front().Character = 0x110000;
	GraphFontConfiguration rejectedFont;
	CHECK_FALSE(FreezeImageGraphInitialTextFontsFromReplay(
		document,
		malformedFont,
		heldFrame,
		rejectedFont,
		engine::imagegraph::Limits::MaximumEvaluationBytes,
		diagnostic
	));
	CHECK(rejectedFont.Context.InitialTextFonts.empty());

	auto emptyFontReplay = replay;
	auto &emptyState = std::get<StructValue>(emptyFontReplay.Entries.front().Values.back().Data);
	emptyState.Data->Fields.front().second = UndefinedValue{};
	GraphFontConfiguration globalFont;
	globalFont.Context.InitialFont = font;
	REQUIRE(FreezeImageGraphInitialTextFontsFromReplay(
		document,
		emptyFontReplay,
		heldFrame,
		globalFont,
		engine::imagegraph::Limits::MaximumEvaluationBytes,
		diagnostic
	));
	REQUIRE(globalFont.Context.InitialTextFonts.size() == 1);
	CHECK_FALSE(globalFont.Context.InitialTextFonts.front().Primary);
	CHECK_FALSE(globalFont.Context.InitialTextFonts.front().Fallback);
}

TEST_CASE("Studio bounds Text font replay matching work before selection", "[export_intent][font]") {
	using studio::detail::ImageGraphTextFontFreezeWorkWithinBounds;
	Document document;
	DataReplayState replay;
	constexpr size_t entryCount = 3000;
	document.Nodes.reserve(entryCount);
	replay.Entries.reserve(entryCount);
	for (size_t index = 0; index < entryCount; ++index) {
		auto id = "text-" + std::to_string(index);
		document.Nodes.push_back({id, "pc.text", {}, {}, {}});
		DataReplayEntry entry;
		entry.NodeId = std::move(id);
		replay.Entries.push_back(std::move(entry));
	}
	engine::imagegraphfont::GraphFontConfiguration configuration;
	Diagnostic diagnostic;
	CHECK_FALSE(ImageGraphTextFontFreezeWorkWithinBounds(document, replay, configuration, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
}

TEST_CASE("frozen Text font seeds survive a fresh held-playing range export", "[export_intent][font]") {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	using engine::imagegraphfont::GraphFontConfiguration;
	using engine::imagegraphfont::GraphFontInputs;
	using studio::detail::FreezeImageGraphInitialTextFontsFromReplay;
	const auto root = std::filesystem::temp_directory_path() /
					  ("atomic-studio-text-font-range-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::error_code filesystemError;
	std::filesystem::create_directories(root, filesystemError);
	REQUIRE_FALSE(filesystemError);
	struct RemoveDirectory {
		std::filesystem::path Path;
		~RemoveDirectory() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	} cleanup{root};

	Document document;
	document.FormatVersion = 9;
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
	document.Nodes = {
		{"text-node", "pc.text", {}, {}, {{"text", std::string{"A"}}}},
		{"export-node",
		 "pc.export",
		 {},
		 {},
		 {{"directory", root.string()},
		  {"file_name", std::string{"frame"}},
		  {"template", std::string{"%d%n%f"}},
		  {"type", EnumValue{1}},
		  {"format", EnumValue{0}}}}
	};
	document.Links = {{"text-node", "surface_out", "export-node", "surface"}};
	document.Outputs = {{"preview", "export-node", "preview"}};
	const auto font = InitialTextFont();
	const auto replay = TextReplay(font);
	GraphFontConfiguration configuration;
	configuration.Context.AliasMapKnown = true;
	Diagnostic diagnostic;
	REQUIRE(FreezeImageGraphInitialTextFontsFromReplay(
		document, replay, FrameTime{4, 0, false}, configuration, Limits::MaximumEvaluationBytes, diagnostic
	));
	GraphFontInputs fonts;
	REQUIRE(fonts.Replace(
		configuration,
		engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
		Limits::MaximumEvaluationBytes,
		diagnostic
	));
	SourceFontContext heldContext;
	EvaluationRequest request;
	REQUIRE(fonts.Bind(true, heldContext, request, Limits::MaximumEvaluationBytes, diagnostic));
	request.HostProvider = nullptr;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot prepared;
	const auto preparedStatus =
		EvaluateNodeInputs(document, plan, "export-node", request, prepared, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(preparedStatus == Status::Ok);
	GraphExportSettings grants;
	grants.Input = root / "source.graph";
	grants.Output = root;
	grants.OutputId = "preview";
	GraphExportGeneration generation{1, 2, 3, 4, 5};
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(document, prepared, request, grants, "export-node", generation, failure));
	FontRangeHost host;
	GraphExportProgress progress = GraphExportProgress::Progress;
	while (progress == GraphExportProgress::Progress || progress == GraphExportProgress::Pending)
		progress = session.Resume(request, generation, host, failure);
	INFO(failure);
	CHECK(progress == GraphExportProgress::Complete);
	CHECK(std::filesystem::exists(root / "frame1.png"));
	CHECK(std::filesystem::exists(root / "frame2.png"));
}
