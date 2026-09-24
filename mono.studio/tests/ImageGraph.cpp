#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Layout.hpp>
#include <string>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.audio_capture")
TEST_DEPENDS("engine.imagegraphio.pxcximport")
TEST_DEPENDS("engine.bake.pxcx")
TEST_DEPENDS("engine.scene.imagegraphbinding")

using engine::imagegraph::Colour;
using engine::imagegraph::Document;
using engine::imagegraph::Keyframe;
using engine::imagegraph::Link;
using engine::imagegraph::Node;
using engine::imagegraph::Output;
using engine::imagegraph::Vector2;

namespace {
	struct TemporaryDirectory {
		std::filesystem::path Path;
		~TemporaryDirectory() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	};

	Document Fixture() {
		Document document;
		document.Nodes.push_back(
			{"solid-main",
			 "image.solid",
			 "group-art",
			 {4.123456789, -8.5},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 255}}},
			 {}}
		);
		document.Nodes.push_back({"pass-main", "image.passthrough", "group-art", {9.0, 11.0}, {}, {}});
		document.Nodes.push_back({"future", "vendor.future", "", {}, {{"offset", Vector2{0.5, -1.25}}}, {}});
		document.Links.push_back({"solid-main", "image", "pass-main", "image"});
		document.Links.push_back({"future", "result", "missing-node", "future-input"});
		document.Groups.push_back({"group-art", "Primary group", {}, {}});
		document.Groups.push_back({"group-empty", "Empty group", {}, {}});
		document.Outputs.push_back({"final-image", "pass-main", "image"});
		document.Keyframes.push_back({"solid-main", "colour", 42, Colour{1, 2, 3, 4}, "linear"});
		document.Keyframes.push_back({"solid-main", "offset", 43, Vector2{0.5, -1.25}, "step"});
		return document;
	}
}

TEST_CASE("imagegraph documents round trip through typed canvas IDs", "[studio][imagegraph]") {
	const Document source = Fixture();
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;

	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	CHECK(ids.ToCanvas.at("solid-main") != ids.ToCanvas.at("pass-main"));
	CHECK(canvas.LinkInto(ids.ToCanvas.at("pass-main"), "image") != nullptr);

	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved == source);
	CHECK(engine::imagegraph::Write(saved) == engine::imagegraph::Write(source));
	CHECK(ids.ToDocument.at(ids.ToCanvas.at("solid-main")) == "solid-main");
	CHECK(ids.GroupsToCanvas.contains("group-art"));
	CHECK(ids.EmptyGroups.contains("group-empty"));
}

TEST_CASE("links the canvas cannot type are retained as unmapped durable records", "[studio][imagegraph]") {
	Document source;
	source.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}},
		 {}}
	);
	source.Nodes.push_back({"future", "vendor.future", "", {}, {}, {}});
	source.Links.push_back({"solid", "image", "future", "result"});
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	CHECK(canvas.Links().empty());

	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved.Links == source.Links);
}

TEST_CASE(
	"canvas round trip preserves version two dynamic inputs and nested groups", "[studio][imagegraph]"
) {
	Document source;
	source.FormatVersion = 2;
	source.Nodes.push_back(
		{"array-node",
		 "value.array",
		 "inner-group",
		 {18.0, 26.0},
		 {},
		 {{"item-1", engine::imagegraph::ValueType::Scalar, engine::imagegraph::Value{1.25}}}}
	);
	source.Groups.push_back({"outer-group", "Outer", {}, {}});
	source.Groups.push_back({"inner-group", "Inner", "outer-group", {}});

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));

	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved.FormatVersion == 2);
	CHECK(saved == source);
	CHECK(engine::imagegraph::Write(saved) == engine::imagegraph::Write(source));
}

TEST_CASE("canvas round trip preserves v3 structured dynamic defaults", "[studio][imagegraph]") {
	Document source;
	source.FormatVersion = 3;
	source.Nodes.push_back(
		{"array-node",
		 "value.array",
		 "",
		 {},
		 {},
		 {{"gradient",
		   engine::imagegraph::ValueType::Gradient,
		   engine::imagegraph::Value{engine::imagegraph::Gradient{
			   0, {{0.0, Colour{0, 0, 0, 255}}, {1.0, Colour{255, 255, 255, 255}}}
		   }}},
		  {"area",
		   engine::imagegraph::ValueType::Area,
		   engine::imagegraph::Value{engine::imagegraph::Area{}}},
		  {"curve",
		   engine::imagegraph::ValueType::Curve,
		   engine::imagegraph::Value{
			   engine::imagegraph::Curve{{}, {{1, 2, 3, 4, 5, 6}, {6, 5, 4, 3, 2, 1}}}
		   }},
		  {"vector4",
		   engine::imagegraph::ValueType::Vector4,
		   engine::imagegraph::Value{engine::imagegraph::Vector4{1, 2, 3, 4}}},
		  {"path",
		   engine::imagegraph::ValueType::Path2D,
		   engine::imagegraph::Value{
			   engine::imagegraph::Path2D{true, {{{1, 2, 3, 4, 5, 6}, 7}}, {{0.25, 0.75}}}
		   }}}}
	);

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved == source);
	CHECK(engine::imagegraph::Write(saved) == engine::imagegraph::Write(source));
}

TEST_CASE(
	"Studio promotes legacy timeline edits to v5 and authors source ease handles", "[studio][imagegraph]"
) {
	Document source;
	source.FormatVersion = 3;
	source.Nodes.push_back({"number", "value.number", "", {}, {{"value", 0.0}}, {}});
	source.Outputs.push_back({"out", "number", "number"});
	source.Keyframes = {
		{"number", "value", 0, 0.0, "linear", std::nullopt},
		{"number", "value", 8, 8.0, "linear", std::nullopt},
	};
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphKeyframeInterpolation(source, 0, "source", diagnostic));
	CHECK(source.FormatVersion == 4);
	REQUIRE(source.Tracks.size() == 1);
	CHECK((source.Tracks.front() == engine::imagegraph::AnimationTrack{"number", "value", "hold", -1}));
	CHECK(source.Keyframes[0].Ease.has_value());
	CHECK(source.Keyframes[1].Ease.has_value());

	auto first = *source.Keyframes[0].Ease;
	first.OutType = "bezier";
	first.Out = {1.0 / 3.0, 0.0};
	REQUIRE(studio::SetImageGraphKeyframeEase(source, 0, first, diagnostic));
	auto last = *source.Keyframes[1].Ease;
	last.InType = "bezier";
	last.In = {1.0 / 3.0, 0.0};
	REQUIRE(studio::SetImageGraphKeyframeEase(source, 1, last, diagnostic));
	REQUIRE(
		studio::SetImageGraphTimeline(
			source, engine::imagegraph::TimelineSettings{9, 0, 8, "loop", 30.0}, diagnostic
		)
	);
	CHECK(source.FormatVersion == 5);

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved == source);
	Document parsed;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(saved), parsed, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(parsed == source);

	engine::imagegraph::Plan plan;
	REQUIRE(engine::imagegraph::Compile(saved, plan, diagnostic) == engine::imagegraph::Status::Ok);
	engine::imagegraph::EvaluatedValue sampled;
	REQUIRE(
		engine::imagegraph::EvaluateValue(saved, plan, "out", {4, 0}, sampled, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(std::get<double>(sampled.Data) == Catch::Approx(1.0));

	REQUIRE(studio::SetImageGraphKeyframeInterpolation(saved, 0, "cubic", diagnostic));
	REQUIRE(engine::imagegraph::Compile(saved, plan, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(
		engine::imagegraph::EvaluateValue(saved, plan, "out", {4, 0}, sampled, diagnostic) ==
		engine::imagegraph::Status::UnsupportedExecution
	);
	CHECK(diagnostic.Code == engine::imagegraph::Status::UnsupportedExecution);
}

TEST_CASE("Studio sine driver authoring promotes and round trips scalar keys", "[studio][imagegraph]") {
	Document source;
	source.FormatVersion = 5;
	source.Nodes.push_back({"number", "value.number", "", {}, {{"value", 0.0}}, {}});
	source.Outputs.push_back({"out", "number", "number"});
	source.Keyframes = {
		{"number", "value", 0, 0.0, "linear", std::nullopt},
		{"number", "value", 3, 0.0, "linear", std::nullopt},
	};
	source.Timeline = engine::imagegraph::TimelineSettings{4, 0, 3, "loop", 30.0};
	source.Tracks = {{"number", "value", "hold", -1}};
	engine::imagegraph::Diagnostic diagnostic;
	const engine::imagegraph::KeyframeSineDriver driver{1.0, 0.25, 0.0, 0.0};
	REQUIRE(studio::SetImageGraphKeyframeSineDriver(source, 0, driver, diagnostic));
	CHECK(source.FormatVersion == 6);

	const std::string text = engine::imagegraph::Write(source);
	Document restored;
	REQUIRE(engine::imagegraph::Read(text, restored, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(restored == source);
	engine::imagegraph::Plan plan;
	REQUIRE(engine::imagegraph::Compile(restored, plan, diagnostic) == engine::imagegraph::Status::Ok);
	engine::imagegraph::EvaluatedValue sampled;
	REQUIRE(
		engine::imagegraph::EvaluateValue(restored, plan, "out", {1, 0}, sampled, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(std::get<double>(sampled.Data) == Catch::Approx(0.25));

	const size_t keyCount = source.Keyframes.size();
	CHECK_FALSE(studio::SetImageGraphKeyframeSineDriver(source, keyCount, driver, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	source.Keyframes[0].Data = Vector2{0.0, 0.0};
	CHECK_FALSE(studio::SetImageGraphKeyframeSineDriver(source, 0, driver, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::TypeMismatch);
	source.Keyframes[0].Data = 0.0;
	CHECK_FALSE(
		studio::SetImageGraphKeyframeSineDriver(
			source,
			0,
			engine::imagegraph::KeyframeSineDriver{1.0, 0.25, 0.0, std::numeric_limits<double>::infinity()},
			diagnostic
		)
	);
	CHECK(diagnostic.Code == engine::imagegraph::Status::InvalidValue);
	REQUIRE(studio::SetImageGraphKeyframeSineDriver(source, 0, std::nullopt, diagnostic));
	CHECK_FALSE(source.Keyframes[0].SineDriver.has_value());
}

TEST_CASE("Studio native graph open migrates legacy documents to v6", "[studio][imagegraph]") {
	const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
	TemporaryDirectory temporary{
		std::filesystem::temp_directory_path() / ("atomic-imagegraph-studio-" + std::to_string(nonce))
	};
	const std::filesystem::path assets = temporary.Path / "Assets";
	const std::filesystem::path graphPath = studio::ImageGraphDocumentPath(assets, "legacy_graph");
	std::error_code filesystemError;
	std::filesystem::create_directories(graphPath.parent_path(), filesystemError);
	REQUIRE_FALSE(filesystemError);

	Document legacy;
	legacy.FormatVersion = 3;
	legacy.Nodes.push_back({"number", "value.number", "", {}, {{"value", 0.0}}, {}});
	legacy.Outputs.push_back({"out", "number", "number"});
	legacy.Keyframes = {
		{"number", "value", 0, 0.0, "linear", std::nullopt},
		{"number", "value", 8, 8.0, "linear", std::nullopt},
	};
	const std::string legacyText = engine::imagegraph::Write(legacy);
	{
		std::ofstream file(graphPath, std::ios::binary | std::ios::trunc);
		REQUIRE(file.good());
		file.write(legacyText.data(), static_cast<std::streamsize>(legacyText.size()));
		REQUIRE(file.good());
	}

	Document opened;
	std::string error;
	REQUIRE(studio::ReadImageGraphDocument(assets, "legacy_graph", opened, error));
	CHECK(error.empty());
	CHECK(opened.FormatVersion == 6);
	CHECK(opened.Nodes == legacy.Nodes);
	CHECK(opened.Keyframes == legacy.Keyframes);
	studio::ImageGraphPlayback playback;
	studio::ApplyImageGraphTimeline(opened, playback);
	CHECK(playback.FramesPerSecond == 30.0);

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	REQUIRE(studio::LoadImageGraphCanvas(opened, canvas, ids, error));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, opened, ids, saved, error));
	CHECK(saved.FormatVersion == 6);
	CHECK(saved == opened);
	CHECK(engine::imagegraph::Write(saved).starts_with("imagegraph 6\n"));
}

TEST_CASE(
	"array nodes expose typed dynamic sockets on the canvas and round trip links", "[studio][imagegraph]"
) {
	Document source;
	source.FormatVersion = 2;
	source.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {0.0, 0.0},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{4, 8, 12, 255}}},
		 {}}
	);
	source.Nodes.push_back(
		{"images",
		 "value.array",
		 "",
		 {180.0, 0.0},
		 {},
		 {{"first", engine::imagegraph::ValueType::Image, std::nullopt}}}
	);
	source.Outputs.push_back({"image-list", "images", "array"});

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	const nodegraph::NodeId solid = ids.ToCanvas.at("solid");
	const nodegraph::NodeId array = ids.ToCanvas.at("images");
	CHECK(canvas.Connect(solid, "image", array, "first") == nodegraph::LinkResult::Made);
	CHECK(nodegraph::PortIn(nodegraph::LayoutOf(*canvas.Find(array)), "first", true) != nullptr);

	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, saved, error));
	CHECK(saved.Nodes[1].DynamicInputs == source.Nodes[1].DynamicInputs);
	REQUIRE(saved.Links.size() == 1);
	CHECK((saved.Links[0] == engine::imagegraph::Link{"solid", "image", "images", "first"}));
	CHECK(saved.FormatVersion == 2);
}

TEST_CASE(
	"dynamic input edits retain typed defaults and drop links after a socket type change",
	"[studio][imagegraph]"
) {
	Document document;
	document.FormatVersion = 2;
	document.Nodes.push_back(
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}},
		 {}}
	);
	document.Nodes.push_back(
		{"images",
		 "value.array",
		 "",
		 {},
		 {},
		 {{"item-1", engine::imagegraph::ValueType::Image, std::nullopt}}}
	);
	document.Links.push_back({"source", "image", "images", "item-1"});
	engine::imagegraph::Diagnostic diagnostic;
	const engine::imagegraph::DynamicInput replacement{
		"item-1",
		engine::imagegraph::ValueType::Array,
		engine::imagegraph::Value{
			engine::imagegraph::ArrayValue{engine::imagegraph::ValueType::Integer, {int64_t{4}, int64_t{9}}}
		}
	};
	REQUIRE(studio::SetImageGraphDynamicInput(document, "images", replacement, diagnostic));
	CHECK(document.Links.empty());
	CHECK(document.Nodes[1].DynamicInputs[0] == replacement);
	CHECK_FALSE(
		studio::SetImageGraphDynamicInput(
			document, "images", {"array", engine::imagegraph::ValueType::Image, std::nullopt}, diagnostic
		)
	);
	CHECK(diagnostic.Code == engine::imagegraph::Status::DuplicateId);

	const std::string text = engine::imagegraph::Write(document);
	Document parsed;
	REQUIRE(engine::imagegraph::Read(text, parsed, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(parsed == document);

	const engine::imagegraph::DynamicInput structured{
		"gradient",
		engine::imagegraph::ValueType::Gradient,
		engine::imagegraph::Value{
			engine::imagegraph::Gradient{0, {{0.0, Colour{0, 0, 0, 255}}, {1.0, Colour{255, 255, 255, 255}}}}
		}
	};
	REQUIRE(studio::SetImageGraphDynamicInput(document, "images", structured, diagnostic));
	CHECK(document.FormatVersion == 3);
	const std::string structuredText = engine::imagegraph::Write(document);
	Document structuredParsed;
	REQUIRE(
		engine::imagegraph::Read(structuredText, structuredParsed, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(structuredParsed == document);
}

TEST_CASE("Studio authors nested image group interfaces and junction routes", "[studio][imagegraph]") {
	Document document;
	document.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "inner",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}},
		 {}}
	);
	document.Nodes.push_back({"sink", "image.passthrough", "", {}, {}, {}});
	document.Outputs.push_back({"final", "sink", "image"});
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(studio::AddImageGraphGroup(document, "outer", "Outer", {}, diagnostic));
	REQUIRE(studio::AddImageGraphGroup(document, "inner", "Inner", "outer", diagnostic));
	engine::imagegraph::GroupPort output{"image", "outside", engine::imagegraph::PortDirection::Output};
	engine::imagegraph::Junction outside{
		"outside", "outer", engine::imagegraph::ValueType::Image, std::nullopt
	};
	REQUIRE(studio::AddImageGraphGroupPort(document, "outer", output, outside, diagnostic));
	engine::imagegraph::GroupPort innerOutput{"image", "inside", engine::imagegraph::PortDirection::Output};
	engine::imagegraph::Junction inside{
		"inside", "inner", engine::imagegraph::ValueType::Image, std::nullopt
	};
	REQUIRE(studio::AddImageGraphGroupPort(document, "inner", innerOutput, inside, diagnostic));
	REQUIRE(studio::AddImageGraphRoute(document, {"solid", "image", "inside", "value"}, diagnostic));
	REQUIRE(studio::AddImageGraphRoute(document, {"inside", "value", "outside", "value"}, diagnostic));
	REQUIRE(studio::AddImageGraphRoute(document, {"outside", "value", "sink", "image"}, diagnostic));

	engine::imagegraph::Plan plan;
	REQUIRE(engine::imagegraph::Compile(document, plan, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(document.FormatVersion == 2);
	CHECK(plan.EffectiveLinks.size() == 1);
	CHECK((plan.EffectiveLinks[0] == engine::imagegraph::Link{"solid", "image", "sink", "image"}));
	CHECK(studio::RemoveImageGraphRoute(document, 1, diagnostic));
	CHECK(document.Links.size() == 2);
}

TEST_CASE("preview frame cache stays within eight bounded RGBA images", "[studio][imagegraph]") {
	studio::ImageGraphPreviewCache cache;
	for (uint64_t tick = 0; tick < studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES + 1; tick++) {
		engine::imagegraph::Image image{1, 1, {static_cast<uint8_t>(tick), 0, 0, 255}, tick + 1};
		REQUIRE(cache.Store(7, 0, tick, image));
		CHECK(cache.HeldBytes() <= studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES * 4);
	}
	CHECK(cache.Find(7, 0, 0) == nullptr);
	const engine::imagegraph::Image *latest = cache.Find(7, 0, studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES);
	REQUIRE(latest != nullptr);
	CHECK(latest->Pixels[0] == studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES);

	const engine::imagegraph::Image firstFraction{1, 1, {16, 0, 0, 255}, 16};
	const engine::imagegraph::Image secondFraction{1, 1, {32, 0, 0, 255}, 32};
	REQUIRE(cache.Store(7, 0, 99, firstFraction, 0.25));
	REQUIRE(cache.Store(7, 0, 99, secondFraction, 0.5));
	REQUIRE(cache.Find(7, 0, 99, 0.25) != nullptr);
	CHECK(cache.Find(7, 0, 99, 0.25)->Pixels[0] == 16);
	REQUIRE(cache.Find(7, 0, 99, 0.5) != nullptr);
	CHECK(cache.Find(7, 0, 99, 0.5)->Pixels[0] == 32);
	CHECK_FALSE(cache.Store(7, 0, 100, secondFraction, 1.0));
	CHECK(cache.Find(7, 0, 100, -0.1) == nullptr);

	engine::imagegraph::Image oversized{
		studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION + 1,
		1,
		std::vector<uint8_t>((studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION + 1) * 4, 255),
		0
	};
	CHECK_FALSE(cache.Store(7, 0, 99, oversized));
	cache.Clear();
	CHECK(cache.HeldBytes() == 0);
}

TEST_CASE("fixed tick playback bounds catchup and keeps authored FPS", "[studio][imagegraph]") {
	studio::ImageGraphPlayback playback;
	CHECK(playback.FramesPerSecond == 30.0);
	playback.Playing = true;
	playback.FramesPerSecond = 120.0;
	CHECK(studio::AdvanceImageGraphPlayback(playback, 0.25));
	CHECK(playback.CurrentTick == 8);
	CHECK(playback.Accumulator > 0.0);
	CHECK(studio::AdvanceImageGraphPlayback(playback, 0.0));
	CHECK(playback.CurrentTick == 16);

	playback.StartTick = 5;
	playback.EndTick = 6;
	playback.CurrentTick = 6;
	playback.FramesPerSecond = 24.0;
	playback.Accumulator = 0.0;
	playback.Loop = true;
	CHECK(studio::AdvanceImageGraphPlayback(playback, 1.0 / 24.0));
	CHECK(playback.CurrentTick == 5);

	playback.CurrentTick = 6;
	playback.Loop = false;
	CHECK_FALSE(studio::AdvanceImageGraphPlayback(playback, 1.0 / 24.0));
	CHECK_FALSE(playback.Playing);

	playback.Playing = true;
	playback.StartTick = 0;
	playback.EndTick = 4;
	playback.CurrentTick = 0;
	playback.Subframe = 0.75;
	playback.FramesPerSecond = 1.0;
	playback.Accumulator = 0.0;
	for (size_t update = 0; update < 3; update++) {
		CHECK_FALSE(studio::AdvanceImageGraphPlayback(playback, 0.25));
		CHECK(playback.CurrentTick == 0);
	}
	CHECK(studio::AdvanceImageGraphPlayback(playback, 0.25));
	CHECK(playback.CurrentTick == 1);
	CHECK(playback.Subframe == 0.75);

	playback.EndTick = 1;
	playback.CurrentTick = 0;
	playback.FramesPerSecond = 4.0;
	playback.Accumulator = 0.0;
	CHECK(studio::AdvanceImageGraphPlayback(playback, 1.0));
	CHECK(playback.CurrentTick == 1);
	CHECK(playback.Subframe == 0.0);

	playback.Playing = true;
	playback.CurrentTick = 0;
	playback.StartTick = 0;
	playback.EndTick = 240;
	playback.FramesPerSecond = 240.0;
	playback.Accumulator = 0.0;
	CHECK(studio::AdvanceImageGraphPlayback(playback, 1.0 / 240.0));
	CHECK(playback.CurrentTick == 1);
	CHECK(playback.FramesPerSecond == 240.0);

	playback.Playing = false;
	playback.FramesPerSecond = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(studio::AdvanceImageGraphPlayback(playback, 0.0));
	CHECK(playback.FramesPerSecond == 30.0);
}

TEST_CASE("Studio playback seeks fractional frames within its saved range", "[studio][imagegraph]") {
	studio::ImageGraphPlayback playback;
	CHECK(studio::SetImageGraphPlaybackFrame(playback, 12.375));
	CHECK(playback.CurrentTick == 12);
	CHECK(playback.Subframe == Catch::Approx(0.375));
	CHECK_FALSE(studio::SetImageGraphPlaybackFrame(playback, 12.375));

	playback.StartTick = 20;
	playback.EndTick = 25;
	CHECK(studio::SetImageGraphPlaybackFrame(playback, 24.75));
	CHECK(playback.CurrentTick == 24);
	CHECK(playback.Subframe == Catch::Approx(0.75));
	CHECK(studio::SetImageGraphPlaybackFrame(playback, 25.5));
	CHECK(playback.CurrentTick == 25);
	CHECK(playback.Subframe == 0.0);

	const uint64_t beforeTick = playback.CurrentTick;
	const double beforeSubframe = playback.Subframe;
	CHECK_FALSE(studio::SetImageGraphPlaybackFrame(playback, std::numeric_limits<double>::quiet_NaN()));
	CHECK_FALSE(studio::SetImageGraphPlaybackFrame(playback, std::numeric_limits<double>::infinity()));
	CHECK(playback.CurrentTick == beforeTick);
	CHECK(playback.Subframe == beforeSubframe);
}

TEST_CASE("v4 saved timeline drives bounded pingpong playback", "[studio][imagegraph]") {
	Document document;
	document.FormatVersion = 4;
	document.Timeline = engine::imagegraph::TimelineSettings{8, 2, 5, "pingpong"};
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(engine::imagegraph::Migrate(document, diagnostic) == engine::imagegraph::Status::Ok);
	CHECK(document.FormatVersion == 6);
	REQUIRE(document.Timeline.has_value());
	CHECK(document.Timeline->FramesPerSecond == 30.0);

	studio::ImageGraphPlayback playback;
	studio::ApplyImageGraphTimeline(document, playback);
	CHECK(playback.TotalFrames == 8);
	CHECK(playback.StartTick == 2);
	CHECK(playback.EndTick == 5);
	CHECK(playback.PingPong);
	CHECK_FALSE(playback.Loop);
	CHECK(playback.FramesPerSecond == 30.0);
	CHECK(playback.CurrentTick == 2);

	playback.Playing = true;
	const std::vector<uint64_t> expected{3, 4, 5, 4, 3, 2, 3};
	for (const uint64_t tick : expected) {
		CHECK(studio::AdvanceImageGraphPlayback(playback, 1.0 / playback.FramesPerSecond));
		CHECK(playback.CurrentTick == tick);
	}
}

TEST_CASE(
	"v5 timeline and track policies round trip with integer and fractional evaluation", "[studio][imagegraph]"
) {
	Document source;
	source.FormatVersion = 5;
	source.Nodes.push_back({"number", "value.number", "inner", {12, 24}, {{"value", 0.0}}, {}});
	source.Groups = {{"outer", "Outer", {}, {}}, {"inner", "Inner", "outer", {}}};
	source.Outputs.push_back({"out", "number", "number"});
	source.Keyframes = {
		{"number", "value", 2, 2.0, "source", engine::imagegraph::KeyframeEase{}},
		{"number", "value", 5, 5.0, "source", engine::imagegraph::KeyframeEase{}},
	};
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		studio::SetImageGraphTimeline(
			source, engine::imagegraph::TimelineSettings{8, 2, 5, "pingpong", 240.0}, diagnostic
		)
	);
	REQUIRE(studio::SetImageGraphAnimationTrack(source, "number", "value", "wrap", -1, diagnostic));

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	Document canvasSaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, canvasSaved, error));
	CHECK(canvasSaved == source);
	Document parsed;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(canvasSaved), parsed, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(parsed == source);
	REQUIRE(parsed.Timeline.has_value());
	CHECK(parsed.Timeline->FramesPerSecond == 240.0);

	engine::imagegraph::Plan plan;
	REQUIRE(engine::imagegraph::Compile(parsed, plan, diagnostic) == engine::imagegraph::Status::Ok);
	engine::imagegraph::EvaluatedValue value;
	REQUIRE(
		engine::imagegraph::EvaluateValue(parsed, plan, "out", {0, 0}, value, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(std::get<double>(value.Data) == Catch::Approx(3.2));
	engine::imagegraph::EvaluatedValue integerValue;
	REQUIRE(
		engine::imagegraph::EvaluateValue(parsed, plan, "out", {4, 0}, integerValue, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	engine::imagegraph::EvaluatedValue explicitIntegerValue;
	REQUIRE(
		engine::imagegraph::EvaluateValue(
			parsed, plan, "out", {4, 0, 0.0}, explicitIntegerValue, diagnostic
		) == engine::imagegraph::Status::Ok
	);
	CHECK(explicitIntegerValue == integerValue);
	engine::imagegraph::EvaluatedValue fractionalValue;
	REQUIRE(
		engine::imagegraph::EvaluateValue(parsed, plan, "out", {3, 0, 0.5}, fractionalValue, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(std::get<double>(fractionalValue.Data) == Catch::Approx(3.5));

	const Document beforeInvalidEdit = source;
	CHECK_FALSE(
		studio::SetImageGraphTimeline(
			source, engine::imagegraph::TimelineSettings{5, 2, 4, "loop"}, diagnostic
		)
	);
	CHECK(source == beforeInvalidEdit);
	CHECK_FALSE(
		studio::SetImageGraphTimeline(
			source,
			engine::imagegraph::TimelineSettings{8, 2, 5, "loop", std::numeric_limits<double>::infinity()},
			diagnostic
		)
	);
	CHECK(source == beforeInvalidEdit);
	CHECK_FALSE(studio::SetImageGraphAnimationTrack(source, "number", "value", "loop", 2, diagnostic));
	CHECK(source == beforeInvalidEdit);
	CHECK_FALSE(studio::RemoveImageGraphAnimationTrack(source, "number", "value", diagnostic));
	CHECK(source == beforeInvalidEdit);

	REQUIRE(studio::SetImageGraphKeyframeInterpolation(source, 0, "linear", diagnostic));
	REQUIRE(studio::RemoveImageGraphAnimationTrack(source, "number", "value", diagnostic));
	REQUIRE(studio::RemoveImageGraphTimeline(source, diagnostic));
	CHECK_FALSE(source.Timeline.has_value());
	CHECK(source.Tracks.empty());
	CHECK(source.Groups.size() == 2);
}

TEST_CASE("canvas edits keep durable IDs and give new Solid nodes explicit values", "[studio][imagegraph]") {
	Document document;
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));

	const nodegraph::NodeId first = canvas.Add("image.solid", 20.0f, 30.0f);
	const nodegraph::NodeId second = canvas.Add("image.solid", 90.0f, 30.0f);
	REQUIRE(first != nodegraph::NO_NODE);
	REQUIRE(second != nodegraph::NO_NODE);

	Document added;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, added, error));
	CHECK(added.FormatVersion == 1);
	Document parsed;
	engine::imagegraph::Diagnostic parseDiagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(added), parsed, parseDiagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(parsed == added);
	document = std::move(added);
	REQUIRE(document.Nodes.size() == 2);
	CHECK(document.Nodes[0].Id == "node-1");
	CHECK(document.Nodes[1].Id == "node-2");
	CHECK(
		(document.Nodes[0].Values == std::vector<engine::imagegraph::AuthoredValue>{
										 {"width", int64_t{64}},
										 {"height", int64_t{64}},
										 {"colour", Colour{255, 255, 255, 255}},
										 {"use_mask_dimension", true},
										 {"empty", false},
										 {"mask_alpha_only", false}
									 })
	);

	Document savedAgain;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, savedAgain, error));
	CHECK(savedAgain == document);

	REQUIRE(canvas.Remove(first));
	Document withoutFirst;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, withoutFirst, error));
	document = std::move(withoutFirst);
	const nodegraph::NodeId third = canvas.Add("image.solid", 150.0f, 30.0f);
	REQUIRE(third != nodegraph::NO_NODE);
	Document afterThird;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, afterThird, error));
	document = std::move(afterThird);
	REQUIRE(document.Nodes.size() == 2);
	CHECK(document.Nodes[0].Id == "node-2");
	CHECK(document.Nodes[1].Id == "node-3");
}

TEST_CASE("palette image nodes use the evaluator's working control defaults", "[studio][imagegraph]") {
	Document document;
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	for (const char *type :
		 {"image.solid",
		  "image.gradient",
		  "image.noise_simplex",
		  "image.tile",
		  "image.height_blend",
		  "image.flip",
		  "image.invert",
		  "image.alpha_cutoff",
		  "image.offset",
		  "image.threshold",
		  "image.posterize",
		  "image.transform_3d",
		  "image.audio_recording",
		  "image.audio_volume",
		  "image.blend",
		  "image.passthrough",
		  "value.array",
		  "value.array_get"}) {
		REQUIRE(canvas.Add(type, 20.0f, 30.0f) != nodegraph::NO_NODE);
	}
	REQUIRE(canvas.Nodes().size() == 18);

	Document added;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, added, error));
	CHECK(added.FormatVersion == 6);
	Document parsed;
	engine::imagegraph::Diagnostic parseDiagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(added), parsed, parseDiagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(parsed == added);
	const auto value = [](const Node &node, std::string_view property) -> const engine::imagegraph::Value * {
		const auto found = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &entry) {
			return entry.Port == property;
		});
		return found == node.Values.end() ? nullptr : &found->Data;
	};
	const auto nodeOf = [&](std::string_view type) -> const Node * {
		const auto found = std::find_if(added.Nodes.begin(), added.Nodes.end(), [&](const Node &node) {
			return node.Type == type;
		});
		return found == added.Nodes.end() ? nullptr : &*found;
	};
	const auto posterizeIterator = std::find_if(added.Nodes.begin(), added.Nodes.end(), [](const Node &node) {
		return node.Type == "image.posterize";
	});
	REQUIRE(posterizeIterator != added.Nodes.end());
	const auto defaultPaletteProperty = std::find_if(
		posterizeIterator->Values.begin(), posterizeIterator->Values.end(), [](const auto &entry) {
			return entry.Port == "palette";
		}
	);
	REQUIRE(defaultPaletteProperty != posterizeIterator->Values.end());
	const auto *defaultPalette = std::get_if<engine::imagegraph::ArrayValue>(&defaultPaletteProperty->Data);
	REQUIRE(defaultPalette != nullptr);
	CHECK(defaultPalette->ElementType == engine::imagegraph::ValueType::Colour);
	REQUIRE(defaultPalette->Elements.size() == 1);
	CHECK((std::get<Colour>(defaultPalette->Elements.front()) == Colour{0, 0, 0, 255}));
	const Node *recording = nodeOf("image.audio_recording");
	REQUIRE(recording != nullptr);
	const auto *sourceId = value(*recording, "source_id");
	REQUIRE(sourceId != nullptr);
	CHECK(std::get<std::string>(*sourceId) == "mono");
	CHECK(engine::imagegraph::Limits::MaximumPaletteEntries == 32);
	const engine::imagegraph::ArrayValue palette{
		engine::imagegraph::ValueType::Colour, {Colour{24, 60, 100, 255}, Colour{240, 180, 80, 192}}
	};
	engine::imagegraph::Diagnostic valueDiagnostic;
	REQUIRE(studio::SetImageGraphValue(added, posterizeIterator->Id, "palette", palette, valueDiagnostic));
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(added), parsed, parseDiagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(parsed == added);

	const Node *solid = nodeOf("image.solid");
	REQUIRE(solid != nullptr);
	REQUIRE(value(*solid, "width") != nullptr);
	REQUIRE(value(*solid, "height") != nullptr);
	REQUIRE(value(*solid, "colour") != nullptr);
	REQUIRE(value(*solid, "use_mask_dimension") != nullptr);
	REQUIRE(value(*solid, "empty") != nullptr);
	REQUIRE(value(*solid, "mask_alpha_only") != nullptr);
	CHECK(std::get<int64_t>(*value(*solid, "width")) == 64);
	CHECK(std::get<int64_t>(*value(*solid, "height")) == 64);
	CHECK((std::get<Colour>(*value(*solid, "colour")) == Colour{255, 255, 255, 255}));
	CHECK(std::get<bool>(*value(*solid, "use_mask_dimension")));
	CHECK_FALSE(std::get<bool>(*value(*solid, "empty")));
	CHECK_FALSE(std::get<bool>(*value(*solid, "mask_alpha_only")));

	const Node *gradient = nodeOf("image.gradient");
	REQUIRE(gradient != nullptr);
	CHECK(std::get<int64_t>(*value(*gradient, "width")) == 64);
	CHECK(std::get<int64_t>(*value(*gradient, "height")) == 64);
	CHECK(std::get<engine::imagegraph::Gradient>(*value(*gradient, "gradient")).Keys.size() == 2);
	CHECK(std::get<engine::imagegraph::Curve>(*value(*gradient, "curve")).Anchors.size() == 2);

	const Node *simplex = nodeOf("image.noise_simplex");
	REQUIRE(simplex != nullptr);
	CHECK(std::get<int64_t>(*value(*simplex, "iterations")) == 1);

	const Node *tile = nodeOf("image.tile");
	REQUIRE(tile != nullptr);
	CHECK(std::get<int64_t>(*value(*tile, "width")) == 64);
	CHECK(std::get<int64_t>(*value(*tile, "height")) == 64);

	const Node *transform = nodeOf("image.transform_3d");
	REQUIRE(transform != nullptr);
	CHECK(
		std::get<engine::imagegraph::Vector3>(*value(*transform, "position")) == engine::imagegraph::Vector3{}
	);
	CHECK(
		(std::get<engine::imagegraph::Vector3>(*value(*transform, "scale")) ==
		 engine::imagegraph::Vector3{1.0, 1.0, 1.0})
	);
	CHECK(
		std::get<engine::imagegraph::Quaternion>(*value(*transform, "rotation")) ==
		engine::imagegraph::Quaternion{}
	);
	CHECK(
		std::get<engine::imagegraph::EnumValue>(*value(*transform, "projection")) ==
		engine::imagegraph::EnumValue{1}
	);
	CHECK(
		canvas.Connect(ids.ToCanvas.at(solid->Id), "image", ids.ToCanvas.at(transform->Id), "surface") ==
		nodegraph::LinkResult::Made
	);
	Document connected;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, added, ids, connected, error));
	CHECK((connected.Links == std::vector<Link>{{solid->Id, "image", transform->Id, "surface"}}));

	const Node *heightBlend = nodeOf("image.height_blend");
	REQUIRE(heightBlend != nullptr);
	REQUIRE(value(*heightBlend, "mode") != nullptr);
	REQUIRE(value(*heightBlend, "type") != nullptr);
	REQUIRE(value(*heightBlend, "factor") != nullptr);
	CHECK(std::get<int64_t>(*value(*heightBlend, "mode")) == 0);
	CHECK(std::get<int64_t>(*value(*heightBlend, "type")) == 1);
	CHECK(std::get<double>(*value(*heightBlend, "factor")) == 0.5);

	const Node *flip = nodeOf("image.flip");
	REQUIRE(flip != nullptr);
	REQUIRE(value(*flip, "axis") != nullptr);
	REQUIRE(value(*flip, "channel") != nullptr);
	REQUIRE(value(*flip, "mix") != nullptr);
	REQUIRE(value(*flip, "invert_mask") != nullptr);
	REQUIRE(value(*flip, "mask_feather") != nullptr);
	CHECK(std::get<int64_t>(*value(*flip, "axis")) == 1);
	CHECK(std::get<int64_t>(*value(*flip, "channel")) == 15);
	CHECK(std::get<double>(*value(*flip, "mix")) == 1.0);
	CHECK_FALSE(std::get<bool>(*value(*flip, "invert_mask")));
	CHECK(std::get<double>(*value(*flip, "mask_feather")) == 0.0);

	const Node *invert = nodeOf("image.invert");
	REQUIRE(invert != nullptr);
	REQUIRE(value(*invert, "include_alpha") != nullptr);
	REQUIRE(value(*invert, "channel") != nullptr);
	REQUIRE(value(*invert, "mix") != nullptr);
	REQUIRE(value(*invert, "invert_mask") != nullptr);
	REQUIRE(value(*invert, "mask_feather") != nullptr);
	CHECK_FALSE(std::get<bool>(*value(*invert, "include_alpha")));
	CHECK(std::get<int64_t>(*value(*invert, "channel")) == 15);
	CHECK(std::get<double>(*value(*invert, "mix")) == 1.0);
	CHECK_FALSE(std::get<bool>(*value(*invert, "invert_mask")));
	CHECK(std::get<double>(*value(*invert, "mask_feather")) == 0.0);

	const Node *cutoff = nodeOf("image.alpha_cutoff");
	REQUIRE(cutoff != nullptr);
	REQUIRE(value(*cutoff, "minimum") != nullptr);
	REQUIRE(value(*cutoff, "mix") != nullptr);
	REQUIRE(value(*cutoff, "invert_mask") != nullptr);
	REQUIRE(value(*cutoff, "mask_feather") != nullptr);
	CHECK(std::get<double>(*value(*cutoff, "minimum")) == 0.5);
	CHECK(std::get<double>(*value(*cutoff, "mix")) == 1.0);
	CHECK_FALSE(std::get<bool>(*value(*cutoff, "invert_mask")));
	CHECK(std::get<double>(*value(*cutoff, "mask_feather")) == 0.0);

	const Node *blend = nodeOf("image.blend");
	REQUIRE(blend != nullptr);
	REQUIRE(value(*blend, "output_dimension") != nullptr);
	REQUIRE(value(*blend, "constant_dimension") != nullptr);
	REQUIRE(value(*blend, "blend_mode") != nullptr);
	REQUIRE(value(*blend, "opacity") != nullptr);
	REQUIRE(value(*blend, "mask_feather") != nullptr);
	CHECK(std::get<int64_t>(*value(*blend, "output_dimension")) == 0);
	CHECK(
		(std::get<engine::imagegraph::Vector2>(*value(*blend, "constant_dimension")) ==
		 engine::imagegraph::Vector2{64, 64})
	);
	CHECK(std::get<int64_t>(*value(*blend, "blend_mode")) == 0);
	CHECK(std::get<double>(*value(*blend, "opacity")) == 1.0);
	CHECK(std::get<double>(*value(*blend, "mask_feather")) == 1.0);

	const Node *posterize = nodeOf("image.posterize");
	REQUIRE(posterize != nullptr);
	CHECK(std::get<int64_t>(*value(*posterize, "steps")) == 4);
	CHECK(std::get<double>(*value(*posterize, "gamma")) == 1.0);
	const auto *parsedPalette = std::get_if<engine::imagegraph::ArrayValue>(value(*posterize, "palette"));
	REQUIRE(parsedPalette != nullptr);
	CHECK(parsedPalette->ElementType == engine::imagegraph::ValueType::Colour);
	REQUIRE(parsedPalette->Elements.size() == 2);
	CHECK((std::get<Colour>(parsedPalette->Elements[0]) == Colour{24, 60, 100, 255}));
	CHECK((std::get<Colour>(parsedPalette->Elements[1]) == Colour{240, 180, 80, 192}));

	const Node *arrayGet = nodeOf("value.array_get");
	REQUIRE(arrayGet != nullptr);
	CHECK(std::get<int64_t>(*value(*arrayGet, "index")) == 0);
	CHECK(std::get<int64_t>(*value(*arrayGet, "overflow")) == 0);
}

TEST_CASE("Studio preview resolves recorded audio and displays scalar outputs", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes.push_back(
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}}
	);
	document.Nodes.push_back({"volume", "image.audio_volume", "", {}, {}, {}});
	document.Links.push_back({"capture", "samples", "volume", "samples"});
	document.Outputs.push_back({"loudness", "volume", "loudness"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> captures{{"mono", 0, {0.5, -0.5}}};
	EvaluationRequest request;
	request.Tick = 0;
	request.AudioFrames = std::span<const AudioCaptureFrame>(captures);
	studio::ImageGraphPreviewValue preview;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "loudness", request, preview, diagnostic) ==
		Status::Ok
	);
	const auto *value = std::get_if<EvaluatedValue>(&preview);
	REQUIRE(value != nullptr);
	CHECK(value->Port == "loudness");
	const auto *loudness = std::get_if<double>(&value->Data);
	REQUIRE(loudness != nullptr);
	CHECK(*loudness == Catch::Approx(10.0 * std::log10(0.5)).epsilon(1e-14));

	request.Tick = 1;
	CHECK(
		studio::EvaluateImageGraphPreview(document, plan, "loudness", request, preview, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(diagnostic.Message.find("exact tick") != std::string::npos);
}

TEST_CASE("new canvas IDs do not capture unresolved document references", "[studio][imagegraph]") {
	Document document;
	document.Nodes.push_back(
		{"existing",
		 "image.solid",
		 "group-1",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 4}}},
		 {}}
	);
	document.Links.push_back({"node-1", "future-output", "node-2", "future-input"});
	document.Outputs.push_back({"future-output", "node-3", "image"});
	document.Keyframes.push_back({"node-4", "future-value", 4, int64_t{5}, "step"});

	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	const nodegraph::NodeId first = canvas.Add("image.solid", 30.0f, 40.0f);
	const nodegraph::NodeId second = canvas.Add("image.solid", 70.0f, 40.0f);
	REQUIRE(first != nodegraph::NO_NODE);
	REQUIRE(second != nodegraph::NO_NODE);
	const nodegraph::GroupId group =
		canvas.Group({first, second}, "New group", nodegraph::Colour::Hex(0xFFFFFF));
	REQUIRE(group != nodegraph::NO_GROUP);

	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, saved, error));
	CHECK(saved.Nodes[1].Id == "node-5");
	CHECK(saved.Nodes[2].Id == "node-6");
	CHECK(saved.Nodes[0].GroupId == "group-1");
	CHECK(saved.Nodes[1].GroupId == "group-2");
	CHECK(saved.Links == document.Links);
	CHECK(saved.Outputs == document.Outputs);
	CHECK(saved.Keyframes == document.Keyframes);
	CHECK((saved.Groups == std::vector<engine::imagegraph::Group>{{"group-2", "New group", {}, {}}}));
}

TEST_CASE(
	"image composer preview rejects oversized Solid dimensions before evaluation", "[studio][imagegraph]"
) {
	Document document;
	document.Nodes.push_back(
		{"solid", "image.solid", "", {}, {{"width", int64_t{129}}, {"height", int64_t{1}}}, {}}
	);
	engine::imagegraph::Diagnostic diagnostic;
	CHECK_FALSE(studio::CheckImageComposerPreviewBudget(document, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "solid");
	CHECK(diagnostic.Port == "width");

	document.Nodes[0].Values[0].Data = int64_t{128};
	CHECK(studio::CheckImageComposerPreviewBudget(document, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::Ok);
}

TEST_CASE(
	"Studio authoring workflow edits canvas values timeline keys and output sink rows", "[studio][imagegraph]"
) {
	Document document = Fixture();
	nodegraph::Graph canvas;
	nodegraph::Canvas canvasUi;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	const nodegraph::NodeId solid = ids.ToCanvas.at("solid-main");
	canvasUi.Select(solid);
	CHECK(ids.ToDocument.at(canvasUi.Selection().front()) == "solid-main");

	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphValue(document, "solid-main", "width", int64_t{24}, diagnostic));
	CHECK(std::get<int64_t>(document.Nodes[0].Values[0].Data) == 24);
	const Document beforeInvalidEdit = document;
	CHECK_FALSE(studio::SetImageGraphValue(document, "solid-main", "width", std::string{"24"}, diagnostic));
	CHECK(diagnostic.Code == engine::imagegraph::Status::TypeMismatch);
	CHECK(document == beforeInvalidEdit);

	REQUIRE(studio::SetImageGraphKeyframe(document, "solid-main", "width", 8, "linear", diagnostic));
	REQUIRE(studio::SetImageGraphValue(document, "solid-main", "width", int64_t{32}, diagnostic));
	REQUIRE(studio::SetImageGraphKeyframe(document, "solid-main", "width", 8, "cubic", diagnostic));
	const auto key =
		std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &frame) {
			return frame.NodeId == "solid-main" && frame.Port == "width" && frame.Tick == 8;
		});
	REQUIRE(key != document.Keyframes.end());
	CHECK(std::get<int64_t>(key->Data) == 32);
	CHECK(key->Interpolation == "cubic");
	CHECK(studio::PreviousImageGraphKey(document, 42) == 8);
	CHECK(studio::NextImageGraphKey(document, 8) == 42);
	REQUIRE(studio::SetImageGraphKeyframeInterpolation(document, 2, "step", diagnostic));
	CHECK(document.Keyframes[2].Interpolation == "step");
	REQUIRE(studio::RemoveImageGraphKeyframe(document, "solid-main", "width", 8, diagnostic));
	CHECK_FALSE(studio::RemoveImageGraphKeyframe(document, "solid-main", "width", 8, diagnostic));

	REQUIRE(studio::SetImageGraphOutput(document, "final-image", "solid-main", "image", diagnostic));
	document.Outputs.push_back({"broken-output", "absent", "image"});
	const std::vector<studio::ImageComposerSinkRow> sinks =
		studio::DescribeImageComposerSinks(document, "final-image");
	REQUIRE(sinks.size() == 2);
	CHECK((sinks[0] == studio::ImageComposerSinkRow{"final-image", "solid-main", "image", true, true}));
	CHECK((sinks[1] == studio::ImageComposerSinkRow{"broken-output", "absent", "image", false, false}));
}

TEST_CASE("PXCX projection keeps foreign nodes opaque and archive fields unchanged", "[studio][imagegraph]") {
	engine::bake::PxcxArchive archive;
	archive.OriginalBytes = {std::byte{0x50}, std::byte{0x58}, std::byte{0x43}, std::byte{0x58}};
	archive.MetadataPayload = {std::byte{0x01}, std::byte{0x02}};
	archive.MetadataNumber = 121092;
	archive.MetadataText = "fixture metadata";
	archive.GraphJson = R"({"nodes":[]})";
	archive.Nodes = {{"source", "image.solid", 3.5, -2.0}, {"blur", "vendor.filter", 44.0, 18.0}};
	archive.Links = {{"source", 2, "blur", 3}};
	const engine::bake::PxcxArchive retained = archive;

	studio::PxcxImageGraphProjection projection;
	std::string error;
	REQUIRE(studio::ProjectPxcxImageGraph(archive, projection, error));
	CHECK(error.empty());
	CHECK(archive.OriginalBytes == retained.OriginalBytes);
	CHECK(archive.MetadataPayload == retained.MetadataPayload);
	CHECK(archive.MetadataNumber == retained.MetadataNumber);
	CHECK(archive.MetadataText == retained.MetadataText);
	CHECK(archive.GraphJson == retained.GraphJson);
	REQUIRE(projection.Graph.Nodes.size() == 2);
	CHECK(projection.Graph.Nodes[0].Type == "pxcx.opaque/image.solid");
	CHECK(engine::imagegraph::FindSchema(projection.Graph.Nodes[0].Type) == nullptr);
	CHECK((projection.Graph.Nodes[0].Position == engine::imagegraph::Vector2{3.5, -2.0}));
	REQUIRE(projection.Graph.Links.size() == 1);
	CHECK((projection.Graph.Links[0] == engine::imagegraph::Link{"source", "output-2", "blur", "input-3"}));
	CHECK(projection.Diagnostics.size() == 2);
	CHECK(projection.Diagnostics[0].Code == engine::imagegraph::Status::UnknownNode);

	studio::RegisterPxcxCanvasNodeTypes(archive);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	REQUIRE(studio::LoadImageGraphCanvas(projection.Graph, canvas, ids, error));
	CHECK(canvas.Nodes().size() == 2);
	CHECK(canvas.Links().size() == 1);
	CHECK(ids.ToCanvas.at("source") != ids.ToCanvas.at("blur"));
}

TEST_CASE("Studio PXCX Open adapter maps supported nodes and retains source bytes", "[studio][imagegraph]") {
	const auto value = [](std::string_view json) { return "{\"r\":{\"d\":" + std::string(json) + "}}"; };
	std::vector<std::string> solidInputs(6, value("-4"));
	solidInputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
	solidInputs[1] = value("4278850590");
	solidInputs[2] = value("false");
	solidInputs[3] = R"JSON({"r":{"d":-4},"attri":{"mask_alpha_only":false}})JSON";
	solidInputs[4] = value("true");
	const auto node = [](std::string_view id, std::string_view type, const std::vector<std::string> &inputs) {
		std::string json = "{\"id\":\"" + std::string(id) + "\",\"type\":\"" + std::string(type) +
						   "\",\"x\":1,\"y\":2,\"inputs\":[";
		for (size_t index = 0; index < inputs.size(); index++) {
			if (index != 0) json += ',';
			json += inputs[index];
		}
		return json + "]}";
	};
	const std::string graphJson = "{\"attributes\":{\"surface_dimension\":[8,8]},\"nodes\":[" +
								  node("solid", "Node_Solid", solidInputs) + "," +
								  node("future", "Vendor_Future", {value("0")}) + "]}";
	engine::bake::PxcxArchive seed;
	seed.MetadataNumber = 121092;
	seed.MetadataText = "1.22.10.201";
	seed.GraphJson = graphJson + '\0';
	seed.HasThumbnailBlock = true;
	seed.ThumbnailRgba.resize(engine::bake::PxcxLimits::ThumbnailRgbaBytes);
	for (size_t index = 0; index < seed.ThumbnailRgba.size(); index++)
		seed.ThumbnailRgba[index] = static_cast<uint8_t>(index % 251);
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(seed, bytes, failure));
	engine::bake::PxcxArchive parsed;
	REQUIRE(engine::bake::ReadPxcx(bytes, parsed, failure));

	engine::imagegraphio::PxcxImport imported;
	REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(parsed, imported, failure));
	CHECK(imported.Source.OriginalBytes == bytes);
	CHECK(imported.Source.GraphJson == graphJson + '\0');
	const auto reference = imported.ReferencePreview();
	REQUIRE(reference.has_value());
	CHECK(reference->Width == 256);
	CHECK(reference->Height == 256);
	CHECK(reference->Rgba.size() == engine::bake::PxcxLimits::ThumbnailRgbaBytes);
	CHECK(std::equal(reference->Rgba.begin(), reference->Rgba.end(), seed.ThumbnailRgba.begin()));
	uint64_t thumbnailHash = 14695981039346656037ull;
	for (uint8_t byte : seed.ThumbnailRgba) {
		thumbnailHash ^= byte;
		thumbnailHash *= 1099511628211ull;
	}
	CHECK(reference->Hash == thumbnailHash);
	REQUIRE(imported.Graph.Nodes.size() == 2);
	CHECK(imported.Graph.Nodes[0].Type == "image.solid");
	CHECK(imported.Graph.Nodes[1].Type == "pxcx.opaque/Vendor_Future");
	CHECK_FALSE(imported.Diagnostics.empty());
	engine::imagegraph::Diagnostic migrationDiagnostic;
	REQUIRE(
		engine::imagegraph::Migrate(imported.Graph, migrationDiagnostic) == engine::imagegraph::Status::Ok
	);
	CHECK(imported.Graph.FormatVersion == 6);

	studio::RegisterPxcxCanvasNodeTypes(imported.Source);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string canvasError;
	REQUIRE(studio::LoadImageGraphCanvas(imported.Graph, canvas, ids, canvasError));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, imported.Graph, ids, saved, canvasError));
	CHECK(saved == imported.Graph);
	CHECK(imported.Source.OriginalBytes == bytes);
}

TEST_CASE("image graph binding draft validates safe names and authored outputs", "[studio][imagegraph]") {
	Document document;
	document.Nodes.push_back(
		{"solid",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}},
		 {}}
	);
	document.Outputs.push_back({"final-image", "solid", "image"});
	CHECK(
		studio::ImageGraphDocumentPath(std::filesystem::path("/assets"), "art_graph") ==
		std::filesystem::path("/assets/imagegraphs/art_graph.graph")
	);
	CHECK(studio::ImageGraphDocumentPath(std::filesystem::path("/assets"), "../outside").empty());
	studio::ImageGraphBindingDraft draft{
		.Graph = "art_graph",
		.Output = "final-image",
		.Texture = "player-image",
		.Seed = 90210,
		.FixedTick = 48,
		.TickPolicy = engine::scene::ImageGraphTickPolicy::World,
		.ColorSpace = engine::scene::ImageGraphColorSpace::Linear,
	};
	engine::scene::ImageGraphBinding binding;
	std::string error;
	REQUIRE(studio::BuildImageGraphBinding(draft, document, binding, error));
	CHECK(error.empty());
	CHECK(binding.Graph.Text() == draft.Graph);
	CHECK(binding.Output.Text() == draft.Output);
	CHECK(binding.Texture.Text() == draft.Texture);
	CHECK(binding.Seed == draft.Seed);
	CHECK(binding.FixedTick == draft.FixedTick);
	CHECK(binding.TickPolicy == engine::scene::ImageGraphTickPolicy::World);
	CHECK(binding.ColorSpace == engine::scene::ImageGraphColorSpace::Linear);

	const engine::scene::ImageGraphBinding before = binding;
	draft.Graph = "../outside";
	CHECK_FALSE(studio::BuildImageGraphBinding(draft, document, binding, error));
	CHECK(error.find("graph name") != std::string::npos);
	CHECK(binding.Graph == before.Graph);

	draft.Graph = "art_graph";
	draft.Output = "missing-output";
	CHECK_FALSE(studio::BuildImageGraphBinding(draft, document, binding, error));
	CHECK(error.find("not present") != std::string::npos);
	CHECK(binding.Output == before.Output);

	draft.Output = "final-image";
	draft.FixedTick = engine::imagegraph::Limits::MaximumTick + 1;
	CHECK_FALSE(studio::BuildImageGraphBinding(draft, document, binding, error));
	CHECK(error.find("timeline limit") != std::string::npos);

	draft.FixedTick = 48;
	Document invalid = document;
	invalid.Nodes[0].Values[0].Data = int64_t{0};
	CHECK_FALSE(studio::BuildImageGraphBinding(draft, invalid, binding, error));
	CHECK_FALSE(error.empty());
	CHECK(binding.Graph == before.Graph);
}

TEST_CASE(
	"imagegraph document history restores exact values and clears redo on a new branch",
	"[studio][imagegraph]"
) {
	Document before = Fixture();
	Document after = before;
	after.Nodes[0].Values[0].Data = int64_t{17};
	studio::ImageGraphHistory history(2);

	history.Record(before, after);
	Document current = after;
	REQUIRE(history.CanUndo());
	CHECK(history.Undo(current));
	CHECK(current == before);
	REQUIRE(history.CanRedo());
	CHECK(history.Redo(current));
	CHECK(current == after);

	history.Record(before, after);
	CHECK(history.Undo(current));
	Document branch = current;
	branch.Nodes[0].Values[0].Data = int64_t{23};
	history.Record(current, branch);
	CHECK_FALSE(history.CanRedo());
	CHECK(history.Undo(current));
	CHECK(current == before);
}

TEST_CASE(
	"imagegraph document history refuses a transition outside its byte budget", "[studio][imagegraph]"
) {
	Document before;
	Document after;
	after.Nodes.push_back({"large", "unknown.large", "", {}, {{"payload", std::string(512, 'x')}}, {}});
	const size_t budget = engine::imagegraph::Write(before).size() + 32;
	studio::ImageGraphHistory history(4, budget);
	history.Record(before, after);
	REQUIRE(history.CanUndo());

	Document current = after;
	CHECK_FALSE(history.Undo(current));
	CHECK(current == after);
	CHECK(history.CanUndo());
}
