#include "ImageGraphExportIntent.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>
#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

TEST_SUITE_ID("studio.imagegraph.export_intent")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.scripthost.composer_lua")
namespace {
	using namespace engine::imagegraph;
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
}
TEST_CASE(
	"Pending current-frame export preserves genuine Lua receipts and the target cursor",
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
		// Authoritative playback advances, but the export request is never relabeled.
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
	"Export intent metadata admission is atomic and completed targets never rewind", "[studio][export_intent]"
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
	"Export preparation advances only completed clocks and preserves contiguous history beyond seek bounds",
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
	"Cold export admission warms genuine feedback one completed frame at a time without repeating Lua",
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
	"Range payload admission includes held small controls and rejects overflow before session creation",
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
	Diagnostic diagnostic;
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
