#include "../src/ImageGraphPreview.hpp"
#include "../src/ImageGraphSourceTimelineTransition.hpp"
#include "../src/KeyframeKindEditor.hpp"

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Layout.hpp>
#include <string>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.catalogue")
TEST_DEPENDS("engine.imagegraph.audio_capture")
TEST_DEPENDS("engine.imagegraph.wav_clip")
TEST_DEPENDS("engine.imagegraph.timeline.v8")
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

TEST_CASE("Studio native graph open migrates legacy documents to v9", "[studio][imagegraph]") {
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
	CHECK(opened.FormatVersion == 9);
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
	CHECK(saved.FormatVersion == 9);
	CHECK(saved == opened);
	CHECK(engine::imagegraph::Write(saved).starts_with("imagegraph 9\n"));
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

TEST_CASE("preview frame cache stays within eight bounded typed images", "[studio][imagegraph]") {
	studio::ImageGraphPreviewCache cache;
	for (uint64_t tick = 0; tick < studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES + 1; tick++) {
		engine::imagegraph::Image image{1, 1, {static_cast<uint8_t>(tick), 0, 0, 255}, tick + 1};
		REQUIRE(cache.Store(7, 0, tick, image));
		CHECK(cache.HeldBytes() <= studio::IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES);
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

TEST_CASE("preview cache retains float pixels and refuses nonfinite replacement", "[studio][imagegraph]") {
	studio::ImageGraphPreviewCache cache;
	engine::imagegraph::Image hdr{
		1, 1, std::vector<uint8_t>(16), 0, engine::imagegraph::SurfaceFormat::RGBA32Float
	};
	REQUIRE(engine::imagegraph::StoreSurfacePixel(hdr, 0, 0, {-2, 4, .5, 1}));
	hdr.Hash = engine::imagegraph::SurfaceHash(hdr);
	REQUIRE(cache.Store(9, 2, 7, hdr));
	CHECK(cache.HeldBytes() >= hdr.Pixels.size());
	CHECK(cache.HeldBytes() <= studio::IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES);
	const auto *held = cache.Find(9, 2, 7);
	REQUIRE(held != nullptr);
	engine::imagegraph::SurfacePixel value{};
	REQUIRE(engine::imagegraph::LoadSurfacePixel(*held, 0, 0, value));
	CHECK((value == engine::imagegraph::SurfacePixel{-2, 4, .5, 1}));

	engine::imagegraph::Image invalid = hdr;
	invalid.Pixels[0] = 0;
	invalid.Pixels[1] = 0;
	invalid.Pixels[2] = 0xc0;
	invalid.Pixels[3] = 0x7f;
	CHECK_FALSE(cache.Store(9, 2, 7, invalid));
	CHECK(cache.Find(9, 2, 7) != nullptr);
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
	CHECK(document.FormatVersion == 9);
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

TEST_CASE("source step caller publishes normalized bounds with one undo", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	Document authored;
	TimelineSettings timeline{
		2,
		0,
		1,
		"loop",
		30.0,
		SourceAuthoringFrameBounds{
			{SourceFrameBoundPresence::Missing, {}}, {SourceFrameBoundPresence::Explicit, {12, 0.0, false}}
		}
	};
	Diagnostic diagnostic;
	REQUIRE(ProjectSourceTimelineWindow(timeline, diagnostic) == Status::Ok);
	REQUIRE(studio::SetImageGraphTimeline(authored, timeline, diagnostic));
	studio::ImageGraphPlayback playback;
	studio::ApplyImageGraphTimeline(authored, playback);
	const Document before = authored;
	const auto playbackBefore = playback;
	studio::detail::PreparedSourceTimelineStep prepared;
	REQUIRE(studio::detail::PrepareSourceTimelineStep(authored, playback, prepared, diagnostic));
	REQUIRE(prepared.AuthoredTimeline.has_value());
	CHECK(prepared.BoundsChanged);
	CHECK(prepared.Playback.SourceBounds->End.Value.Tick == 2);
	CHECK(authored == before);
	CHECK(GetImageGraphFrame(playback) == GetImageGraphFrame(playbackBefore));
	CHECK(playback.SourceBounds == playbackBefore.SourceBounds);
	studio::ImageGraphHistory history(4);
	bool documentChanged = false;
	REQUIRE(
		studio::detail::CommitSourceTimelineStep(
			authored, history, playback, std::move(prepared), documentChanged, diagnostic
		)
	);
	CHECK(documentChanged);
	CHECK(playback.SourceBounds->End.Value.Tick == 2);
	REQUIRE(authored.Timeline.has_value());
	CHECK(authored.Timeline->SourceBounds->End.Value.Tick == 2);
	CHECK(history.Undo(authored));
	CHECK(authored == before);
	CHECK(history.Redo(authored));
	CHECK(authored.Timeline->SourceBounds->End.Value.Tick == 2);
	studio::ImageGraphHistory noHistory(0, 0);
	studio::detail::PreparedSourceTimelineStep steadyStep;
	REQUIRE(studio::detail::PrepareSourceTimelineStep(authored, playback, steadyStep, diagnostic));
	CHECK_FALSE(steadyStep.AuthoredTimeline.has_value());
	CHECK_FALSE(steadyStep.BoundsChanged);
	const Document normalized = authored;
	const auto normalizedPlayback = playback;
	REQUIRE(
		studio::detail::CommitSourceTimelineStep(
			authored, noHistory, playback, std::move(steadyStep), documentChanged, diagnostic
		)
	);
	CHECK_FALSE(documentChanged);
	CHECK(authored == normalized);
	CHECK(GetImageGraphFrame(playback) == GetImageGraphFrame(normalizedPlayback));
	CHECK(playback.SourceBounds == normalizedPlayback.SourceBounds);

	Document refused = before;
	studio::ImageGraphPlayback refusedPlayback = playbackBefore;
	studio::detail::PreparedSourceTimelineStep refusedStep;
	REQUIRE(studio::detail::PrepareSourceTimelineStep(refused, refusedPlayback, refusedStep, diagnostic));
	documentChanged = true;
	CHECK_FALSE(
		studio::detail::CommitSourceTimelineStep(
			refused, noHistory, refusedPlayback, std::move(refusedStep), documentChanged, diagnostic
		)
	);
	CHECK_FALSE(documentChanged);
	CHECK(refused == before);
	CHECK(GetImageGraphFrame(refusedPlayback) == GetImageGraphFrame(playbackBefore));
	CHECK(refusedPlayback.SourceBounds == playbackBefore.SourceBounds);

	studio::ImageGraphPlayback nativePlayback;
	studio::detail::PreparedSourceTimelineStep nativeStep;
	REQUIRE(studio::detail::PrepareSourceTimelineStep(authored, nativePlayback, nativeStep, diagnostic));
	CHECK_FALSE(nativeStep.AuthoredTimeline.has_value());
	const Document authoredUnchanged = authored;
	documentChanged = true;
	REQUIRE(
		studio::detail::CommitSourceTimelineStep(
			authored, noHistory, nativePlayback, std::move(nativeStep), documentChanged, diagnostic
		)
	);
	CHECK_FALSE(documentChanged);
	CHECK(authored == authoredUnchanged);
}

TEST_CASE(
	"source timeline projection retains endpoints outside the playback window", "[studio][imagegraph]"
) {
	using namespace engine::imagegraph;
	TimelineSettings timeline{
		2,
		0,
		1,
		"loop",
		30.0,
		SourceAuthoringFrameBounds{
			{SourceFrameBoundPresence::Explicit, {0, 0.5, false}},
			{SourceFrameBoundPresence::Explicit, {12, 0.0, false}}
		}
	};
	Diagnostic diagnostic;
	REQUIRE(ProjectSourceTimelineWindow(timeline, diagnostic) == Status::Ok);
	CHECK(timeline.First == 0);
	CHECK(timeline.Last == 1);
	CHECK(SourceTimelineFirstFrame(timeline) == -0.5);
	CHECK(SourceTimelineLastFrame(timeline) == 11.0);

	timeline.SourceBounds->Start = {SourceFrameBoundPresence::Explicit, {4, 0.0, false}};
	timeline.SourceBounds->End = {SourceFrameBoundPresence::Explicit, {4, 0.0, false}};
	CHECK(NormalizeSourceTimelineBounds(timeline, diagnostic) == Status::Ok);
	CHECK(timeline.SourceBounds->Start.Presence == SourceFrameBoundPresence::Null);
	CHECK(timeline.SourceBounds->End.Presence == SourceFrameBoundPresence::Null);

	timeline.SourceBounds->Start = {SourceFrameBoundPresence::Explicit, {0, 0.0, false}};
	timeline.SourceBounds->End = {SourceFrameBoundPresence::Explicit, {0, 0.0, false}};
	CHECK(NormalizeSourceTimelineBounds(timeline, diagnostic) == Status::Ok);
	CHECK(timeline.SourceBounds->Start.Presence == SourceFrameBoundPresence::Explicit);
	CHECK(timeline.SourceBounds->End.Presence == SourceFrameBoundPresence::Explicit);
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

TEST_CASE(
	"every source catalogue node is searchable and placed with source defaults", "[studio][imagegraph]"
) {
	Document document;
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	size_t registered = 0;
	for (const engine::imagegraph::CatalogueEntry &entry : engine::imagegraph::Catalogue()) {
		const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(std::string(entry.Type));
		if (!type) continue;
		const size_t inputs = static_cast<size_t>(
			std::count_if(entry.Schema.Ports.begin(), entry.Schema.Ports.end(), [](const auto &port) {
				return port.Direction == engine::imagegraph::PortDirection::Input;
			})
		);
		const std::string_view expectedTitle =
			entry.Type == "pc.graph_preview" ? "Image Preview" : entry.Title;
		if (type->Title == expectedTitle && type->Category == "Pixel Composer/" + std::string(entry.Family) &&
			type->Inputs.size() == inputs && type->Outputs.size() == entry.Schema.Ports.size() - inputs)
			registered++;
	}
	CHECK(registered == engine::imagegraph::Catalogue().size());
	for (const auto &[id, category] :
		 {std::pair<std::string_view, std::string_view>{"image.solid", "Generate"},
		  {"image.flip", "Filter"},
		  {"image.audio_recording", "Audio"},
		  {"value.array", "Values"}}) {
		const nodegraph::NodeType *type = nodegraph::NodeTypes::Find(std::string(id));
		REQUIRE(type != nullptr);
		CHECK(type->Category == category);
	}
	const nodegraph::NodeType *preview = nodegraph::NodeTypes::Find("pc.graph_preview");
	REQUIRE(preview != nullptr);
	CHECK(preview->Title == "Image Preview");
	const engine::imagegraph::CatalogueEntry *previewEntry =
		engine::imagegraph::FindCatalogueEntry("pc.graph_preview");
	REQUIRE(previewEntry != nullptr);
	CHECK(preview->Category == "Pixel Composer/" + std::string(previewEntry->Family));

	REQUIRE(canvas.Add("pc.bw", 20.0f, 30.0f) != nodegraph::NO_NODE);
	Document added;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, added, error));
	REQUIRE(added.Nodes.size() == 1);
	const auto value = [&](std::string_view property) -> const engine::imagegraph::Value * {
		for (const auto &entry : added.Nodes[0].Values)
			if (entry.Port == property) return &entry.Data;
		return nullptr;
	};
	REQUIRE(value("contrast"));
	CHECK(*value("contrast") == engine::imagegraph::Value{1.0});
	REQUIRE(value("channel"));
	CHECK(*value("channel") == engine::imagegraph::Value{int64_t{15}});
	REQUIRE(value("contrast_map_range"));
	CHECK(*value("contrast_map_range") == engine::imagegraph::Value{engine::imagegraph::Vector2{0.0, 1.0}});
	Document parsed;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(added), parsed, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(parsed == added);
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
	// Migration retains imported project settings in the current native grammar.
	CHECK(imported.Graph.FormatVersion == 9);

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

TEST_CASE(
	"project settings edits are atomic and preserve format and history", "[studio][imagegraph][project]"
) {
	using engine::imagegraph::Diagnostic;
	using engine::imagegraph::Limits;
	using engine::imagegraph::ProjectSettings;
	using engine::imagegraph::Status;
	Document document;
	const Document original = document;
	Diagnostic diagnostic;
	ProjectSettings settings{7, 9, 6, 12, {{1, 2, 3, 4}, {5, 6, 7, 8}}};
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	CHECK(document.FormatVersion == 7);
	CHECK(document.Project == settings);
	Document restored;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(document), restored, diagnostic) == Status::Ok
	);
	CHECK(restored == document);
	studio::ImageGraphHistory history;
	history.Record(original, document);
	const Document authored = document;
	for (int invalidField = 0; invalidField < 9; invalidField++) {
		ProjectSettings invalid = settings;
		switch (invalidField) {
		case 0:
			invalid.SurfaceWidth = 0;
			break;
		case 1:
			invalid.SurfaceHeight = 0;
			break;
		case 2:
			invalid.SurfaceWidth = std::numeric_limits<uint32_t>::max();
			break;
		case 3:
			invalid.SurfaceHeight = Limits::MaximumDimension + 1;
			break;
		case 4:
			invalid.Interpolation = -1;
			break;
		case 5:
			invalid.Interpolation = std::numeric_limits<int64_t>::max();
			break;
		case 6:
			invalid.Oversample = -1;
			break;
		case 7:
			invalid.Oversample = 13;
			break;
		case 8:
			invalid.Palette.resize(Limits::MaximumProjectPaletteEntries + 1);
			break;
		}
		CHECK_FALSE(studio::SetImageGraphProjectSettings(document, invalid, diagnostic));
		CHECK(diagnostic.Code != Status::Ok);
		CHECK(document == authored);
	}
	REQUIRE(history.Undo(document));
	CHECK(document == original);
	REQUIRE(history.Redo(document));
	CHECK(document == authored);
	studio::RemoveImageGraphProjectSettings(document);
	CHECK_FALSE(document.Project.has_value());
	CHECK(document.FormatVersion == 7);
	history.Record(authored, document);
	const Document removed = document;
	REQUIRE(history.Undo(document));
	CHECK(document == authored);
	REQUIRE(history.Redo(document));
	CHECK(document == removed);
	settings = {1, Limits::MaximumDimension, 0, 0, {}};
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	CHECK(diagnostic.Code == Status::Ok);
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(document), restored, diagnostic) == Status::Ok
	);
	CHECK(restored == document);
	settings.Palette.resize(Limits::MaximumProjectPaletteEntries);
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	CHECK(document.Project->Palette.size() == Limits::MaximumProjectPaletteEntries);
}

TEST_CASE(
	"project edits change real catalogue inherited dimensions and restore defaults",
	"[studio][imagegraph][project]"
) {
	using engine::imagegraph::Diagnostic;
	using engine::imagegraph::Status;
	Document document;
	document.FormatVersion = 7;
	document.Nodes.push_back(
		{"fill",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{0.5, 1}},
		  {"dimension_unit", engine::imagegraph::EnumValue{1}},
		  {"color", Colour{23, 45, 67, 89}}}}
	);
	document.Outputs.push_back({"image", "fill", "surface_out"});
	Diagnostic diagnostic;
	const auto image = [&] {
		engine::imagegraph::Plan plan;
		REQUIRE(engine::imagegraph::Compile(document, plan, diagnostic) == Status::Ok);
		engine::imagegraph::Image rendered;
		REQUIRE(engine::imagegraph::Evaluate(document, plan, "image", rendered, diagnostic) == Status::Ok);
		return rendered;
	};
	CHECK(image().Width == 16);
	CHECK(image().Height == 32);
	engine::imagegraph::ProjectSettings settings;
	settings.SurfaceWidth = 8;
	settings.SurfaceHeight = 3;
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	const auto rendered = image();
	CHECK(rendered.Width == 4);
	CHECK(rendered.Height == 3);
	REQUIRE(rendered.Pixels.size() == 4 * 3 * 4);
	CHECK(rendered.Pixels[0] == 23);
	CHECK(rendered.Pixels[3] == 89);
	Document reloaded;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(document), reloaded, diagnostic) == Status::Ok
	);
	document = std::move(reloaded);
	const auto reloadedImage = image();
	CHECK(reloadedImage.Width == rendered.Width);
	CHECK(reloadedImage.Height == rendered.Height);
	CHECK(reloadedImage.Pixels == rendered.Pixels);
	CHECK(reloadedImage.Hash == rendered.Hash);
	studio::RemoveImageGraphProjectSettings(document);
	CHECK(image().Width == 16);
	CHECK(image().Height == 32);
}

TEST_CASE("new source palette nodes clone project palette once", "[studio][imagegraph][project]") {
	studio::RegisterImageGraphNodeTypes();
	Document document;
	engine::imagegraph::Diagnostic diagnostic;
	engine::imagegraph::ProjectSettings settings;
	settings.Palette = {{12, 34, 56, 78}, {90, 87, 65, 43}};
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, error));
	const auto nodeId = canvas.Add("pc.gradient_palette", 0, 0);
	REQUIRE(nodeId != nodegraph::NO_NODE);
	Document added;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, added, error));
	REQUIRE(added.Nodes.size() == 1);
	const auto paletteOf = [](const Node &node) -> const engine::imagegraph::ArrayValue & {
		const auto property = std::find_if(node.Values.begin(), node.Values.end(), [](const auto &entry) {
			return entry.Port == "palette";
		});
		REQUIRE(property != node.Values.end());
		return std::get<engine::imagegraph::ArrayValue>(property->Data);
	};
	const engine::imagegraph::ArrayValue cloned{
		engine::imagegraph::ValueType::Colour, {settings.Palette[0], settings.Palette[1]}
	};
	CHECK(paletteOf(added.Nodes[0]) == cloned);
	settings.Palette = {{255, 0, 0, 255}};
	REQUIRE(studio::SetImageGraphProjectSettings(added, settings, diagnostic));
	Document saved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, added, ids, saved, error));
	CHECK(paletteOf(saved.Nodes[0]) == cloned);
	const engine::imagegraph::ArrayValue explicitPalette{
		engine::imagegraph::ValueType::Colour, {Colour{1, 1, 1, 1}}
	};
	REQUIRE(studio::SetImageGraphValue(saved, saved.Nodes[0].Id, "palette", explicitPalette, diagnostic));
	REQUIRE(studio::SaveImageGraphCanvas(canvas, saved, ids, added, error));
	CHECK(paletteOf(added.Nodes[0]) == explicitPalette);
	Document reloaded;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(added), reloaded, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(reloaded == added);
	for (const std::string_view port : {"color_from", "color_to"}) {
		const auto palette = studio::ImageGraphPropertyDefault(saved, "pc.gradient_replace_color", port);
		REQUIRE(palette.has_value());
		CHECK(
			std::get<engine::imagegraph::ArrayValue>(*palette).Elements ==
			std::vector<engine::imagegraph::ElementValue>{settings.Palette[0]}
		);
	}
	saved.Project->Palette.resize(engine::imagegraph::Limits::MaximumProjectPaletteEntries + 1);
	CHECK_FALSE(studio::ImageGraphPropertyDefault(saved, "pc.gradient_palette", "palette").has_value());
}

TEST_CASE(
	"Studio array preview retains captured Audio Window channel shape", "[studio][imagegraph][array_preview]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"audio"}}}, {}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t{4}},
		  {"cursor_location", EnumValue{0}},
		  {"step", int64_t{1}},
		  {"match_timeline", false}},
		 {}}
	};
	document.Links = {{"capture", "audio", "window", "audio_data"}};
	document.Outputs = {{"samples", "capture", "audio"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphOutput(document, "samples", "window", "bit_array", diagnostic));
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	for (const AudioCaptureFrame &frame :
		 {AudioCaptureFrame{"audio", 0, {1, 2, 3, 4, 5, 6}, 10},
		  AudioCaptureFrame{"audio", 0, {}, 10, {{1, 2, 3, 4, 5, 6}, {10, 20, 30, 40, 50, 60}}},
		  AudioCaptureFrame{"audio", 0, {}, 10, {{}, {}}}}) {
		EvaluationRequest request;
		request.AudioFrames = std::span<const AudioCaptureFrame>(&frame, 1);
		studio::ImageGraphPreviewValue preview;
		REQUIRE(
			studio::EvaluateImageGraphPreview(document, plan, "samples", request, preview, diagnostic) ==
			Status::Ok
		);
		const auto *value = std::get_if<EvaluatedValue>(&preview);
		REQUIRE(value != nullptr);
		const auto *array = std::get_if<ArrayValue>(&value->Data);
		REQUIRE(array != nullptr);
		CHECK(array->Elements.empty());
		CHECK(array->Nested.size() == (frame.Channels.empty() ? 1 : frame.Channels.size()));
		for (size_t channel = 0; channel < array->Nested.size(); channel++) {
			const auto &samples = frame.Channels.empty() ? frame.Samples : frame.Channels[channel];
			const size_t expected = std::min(size_t{4}, samples.empty() ? 0 : samples.size() - 1);
			REQUIRE(array->Nested[channel].size() == expected);
			for (size_t index = 0; index < expected; index++)
				CHECK(std::get<double>(array->Nested[channel][index]) == samples[index]);
		}
	}
}

TEST_CASE(
	"numeric array preview validates finite samples and total display budget",
	"[studio][imagegraph][array_preview]"
) {
	using namespace engine::imagegraph;
	Diagnostic diagnostic;
	ArrayValue array{ValueType::Scalar, {1.0, -0.5}};
	CHECK(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array.Elements.resize(Limits::MaximumArrayElements, 0.0);
	CHECK(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array.Elements.push_back(0.0);
	CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	array = {ValueType::Scalar, {}, std::vector<std::vector<ElementValue>>(Limits::MaximumAudioChannels)};
	CHECK(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array.Nested[0].resize(Limits::MaximumArrayElements, 0.0);
	CHECK(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array.Nested[1].push_back(1.0);
	CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	array = {ValueType::Scalar, {}, std::vector<std::vector<ElementValue>>(Limits::MaximumAudioChannels + 1)};
	CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
	for (double invalid :
		 {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		array = {ValueType::Scalar, {}, {{invalid}}};
		CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
		CHECK(diagnostic.Code == Status::InvalidValue);
	}
	array = {ValueType::Scalar, {1.0}, {{2.0}}};
	CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array = {ValueType::Scalar, {int64_t{1}}};
	CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array = {ValueType::Integer, {int64_t{1}, int64_t{-2}}};
	CHECK(studio::CheckImageGraphArrayPreview(array, diagnostic));
	array = {ValueType::Colour, {Colour{}}};
	CHECK_FALSE(studio::CheckImageGraphArrayPreview(array, diagnostic));
	CHECK(diagnostic.Code == Status::UnsupportedExecution);
}

TEST_CASE("Studio numeric array preview preserves flat FFT order", "[studio][imagegraph][array_preview]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"fft", "pc.fft", "", {}, {{"data", ArrayValue{ValueType::Scalar, {1.0, 1.0, 1.0, 1.0}}}}, {}}
	};
	document.Outputs = {{"spectrum", "fft", "array"}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	studio::ImageGraphPreviewValue preview;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "spectrum", {}, preview, diagnostic) == Status::Ok
	);
	const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(preview).Data);
	CHECK(array.Nested.empty());
	CHECK(array.Elements == std::vector<ElementValue>{0.0, 0.0, 4.0});
}

TEST_CASE(
	"Studio preview limits resolved native surfaces before replacing pixels",
	"[studio][imagegraph][preview_budget]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"fill", "pc.solid", "", {}, {{"dimension", Vector2{1, 1}}, {"dimension_unit", EnumValue{1}}}, {}}
	};
	document.Outputs = {{"image", "fill", "surface_out"}};
	Diagnostic diagnostic;
	const auto evaluate = [&](const EvaluationRequest &request, studio::ImageGraphPreviewValue &preview) {
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		return studio::EvaluateImageGraphPreview(document, plan, "image", request, preview, diagnostic);
	};
	ProjectSettings settings;
	settings.SurfaceWidth = 128;
	settings.SurfaceHeight = 1;
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	studio::ImageGraphPreviewValue preview;
	REQUIRE(evaluate({}, preview) == Status::Ok);
	const Image accepted = std::get<Image>(preview);
	CHECK(accepted.Width == 128);
	settings.SurfaceWidth = 129;
	REQUIRE(studio::SetImageGraphProjectSettings(document, settings, diagnostic));
	CHECK(evaluate({}, preview) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "fill");
	CHECK(std::get<Image>(preview).Pixels == accepted.Pixels);
	REQUIRE(studio::SetImageGraphValue(document, "fill", "dimension_unit", EnumValue{0}, diagnostic));
	REQUIRE(studio::SetImageGraphValue(document, "fill", "dimension", Vector2{129, 1}, diagnostic));
	CHECK(evaluate({}, preview) == Status::LimitExceeded);
	CHECK(std::get<Image>(preview).Width == accepted.Width);
	REQUIRE(studio::SetImageGraphValue(document, "fill", "dimension", Vector2{128, 1}, diagnostic));
	EvaluationRequest request;
	request.MaximumImageDimension = 127;
	CHECK(evaluate(request, preview) == Status::LimitExceeded);
	request.MaximumImageDimension = 128;
	CHECK(evaluate(request, preview) == Status::Ok);
	for (uint32_t invalid : {uint32_t{0}, Limits::MaximumDimension + 1}) {
		request.MaximumImageDimension = invalid;
		CHECK(evaluate(request, preview) == Status::InvalidValue);
	}
	// A linked dimension bypasses authored Project units, but must retain the host allocation cap.
	document.Nodes.push_back(
		{"size",
		 "pc.vector_polar_to_cart",
		 "",
		 {},
		 {{"polar_coord", Vector2{129, 0}}, {"cartesian_origin", Vector2{0, -1}}},
		 {}}
	);
	document.Links.push_back({"size", "cartesian_coord", "fill", "dimension"});
	CHECK(evaluate({}, preview) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "fill");
	document.Links.clear();
	document.Nodes.back() = {
		"mask", "pc.solid", "", {}, {{"dimension", Vector2{129, 1}}, {"dimension_unit", EnumValue{0}}}, {}
	};
	document.Links.push_back({"mask", "surface_out", "fill", "mask"});
	CHECK(evaluate({}, preview) == Status::LimitExceeded);
	CHECK(diagnostic.NodeId == "mask");
	CHECK(std::get<Image>(preview).Width == accepted.Width);
}

TEST_CASE("numeric preview caps upstream native images", "[studio][imagegraph][preview_budget]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"fill", "pc.solid", "", {}, {{"dimension", Vector2{128, 1}}, {"dimension_unit", EnumValue{0}}}, {}},
		{"data", "pc.surface_data", "", {}, {}, {}},
		{"number", "pc.to_number", "", {}, {}, {}}
	};
	document.Links = {
		{"fill", "surface_out", "data", "surface"}, {"data", "format_string", "number", "text"}
	};
	document.Outputs = {{"numeric", "number", "number"}};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	studio::ImageGraphPreviewValue preview;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "numeric", {}, preview, diagnostic) == Status::Ok
	);
	const auto accepted = std::get<EvaluatedValue>(preview);
	CHECK(std::get<double>(accepted.Data) == 8.0);
	REQUIRE(studio::SetImageGraphValue(document, "fill", "dimension", Vector2{129, 1}, diagnostic));
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(
		studio::EvaluateImageGraphPreview(document, plan, "numeric", {}, preview, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(diagnostic.NodeId == "fill");
	CHECK(std::get<EvaluatedValue>(preview).Data == accepted.Data);
}

namespace {
	TemporaryDirectory AudioTemporaryDirectory() {
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto path =
			std::filesystem::temp_directory_path() / ("atomic-studio-audio-" + std::to_string(nonce));
		std::filesystem::create_directories(path);
		return TemporaryDirectory{path};
	}

	void WriteAudioTestFile(const std::filesystem::path &path, std::string_view bytes) {
		std::ofstream file(path, std::ios::binary);
		file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		REQUIRE(file.good());
	}

	std::string PreviewWave(uint16_t channels, const std::vector<uint8_t> &samples) {
		std::string bytes;
		const auto word = [&](uint32_t value, size_t count) {
			for (size_t index = 0; index < count; index++)
				bytes.push_back(char((value >> (index * 8)) & 255));
		};
		bytes += "RIFF";
		word(36 + samples.size() + samples.size() % 2, 4);
		bytes += "WAVEfmt ";
		word(16, 4);
		word(1, 2);
		word(channels, 2);
		word(8, 4);
		word(8 * channels, 4);
		word(channels, 2);
		word(8, 2);
		bytes += "data";
		word(samples.size(), 4);
		for (uint8_t sample : samples)
			bytes.push_back(char(sample));
		if (samples.size() % 2) bytes.push_back(0);
		return bytes;
	}
}

TEST_CASE("capture host operations invalidate a cached real image", "[studio][imagegraph][audio_sources]") {
	using namespace engine::imagegraph;
	const auto temporary = AudioTemporaryDirectory();
	const auto capturePath = temporary.Path / "frames.capture";
	Document document;
	document.FormatVersion = 8;
	document.Nodes = {
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"audio"}}}, {}},
		{"volume", "image.audio_volume", "", {}, {}, {}},
		{"gradient",
		 "image.gradient",
		 "",
		 {},
		 {{"width", int64_t{4}},
		  {"height", int64_t{4}},
		  {"gradient", Gradient{0, {{0, Colour{0, 0, 0, 255}}, {1, Colour{255, 255, 255, 255}}}}}},
		 {}}
	};
	document.Links = {
		{"capture", "samples", "volume", "samples"}, {"volume", "loudness", "gradient", "angle_value"}
	};
	document.Outputs = {{"image", "gradient", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	const Status compileStatus = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compileStatus == Status::Ok);
	studio::ImageGraphPreviewCache cache;
	std::vector<AudioCaptureFrame> loaded;
	std::vector<AudioCaptureFrame> frames{{"audio", 0, {1.0}}};
	std::string encoded;
	REQUIRE(WriteAudioCapture(frames, encoded, diagnostic) == Status::Ok);
	WriteAudioTestFile(capturePath, encoded);
	REQUIRE(studio::LoadImageGraphAudioCapture(loaded, cache, capturePath, diagnostic));
	EvaluationRequest request;
	request.AudioFrames = loaded;
	studio::ImageGraphPreviewValue preview;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "image", request, preview, diagnostic) == Status::Ok
	);
	const Image first = std::get<Image>(preview);
	REQUIRE(cache.Store(1, 0, 0, first));
	CHECK_FALSE(studio::LoadImageGraphAudioCapture(loaded, cache, temporary.Path / "missing", diagnostic));
	REQUIRE(cache.Find(1, 0, 0) != nullptr);
	CHECK(cache.Find(1, 0, 0)->Pixels == first.Pixels);
	CHECK(loaded == frames);
	frames[0].Samples = {0.000001};
	REQUIRE(WriteAudioCapture(frames, encoded, diagnostic) == Status::Ok);
	WriteAudioTestFile(capturePath, encoded);
	REQUIRE(studio::LoadImageGraphAudioCapture(loaded, cache, capturePath, diagnostic));
	CHECK(cache.Find(1, 0, 0) == nullptr);
	request.AudioFrames = loaded;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "image", request, preview, diagnostic) == Status::Ok
	);
	const Image second = std::get<Image>(preview);
	REQUIRE(first.Pixels != second.Pixels);
	REQUIRE(cache.Store(1, 0, 0, second));
	studio::ClearImageGraphAudioCapture(loaded, cache);
	CHECK(cache.Find(1, 0, 0) == nullptr);
	CHECK(loaded.empty());
	request.AudioFrames = loaded;
	CHECK(
		studio::EvaluateImageGraphPreview(document, plan, "image", request, preview, diagnostic) ==
		Status::InvalidValue
	);
}

TEST_CASE("named WAV host sources retain exact planar preview data", "[studio][imagegraph][audio_sources]") {
	using namespace engine::imagegraph;
	const auto temporary = AudioTemporaryDirectory();
	const auto wavePath = temporary.Path / "physical.wav";
	WriteAudioTestFile(wavePath, PreviewWave(2, {0, 128, 64, 192, 128, 255}));
	std::vector<AudioClipSource> sources;
	studio::ImageGraphPreviewCache cache;
	Diagnostic diagnostic;
	std::string sourceId = "../Tone.WAV";
	REQUIRE(studio::LoadImageGraphWavSource(sources, cache, sourceId, wavePath, diagnostic));
	sourceId = "temporary name changed";
	REQUIRE(sources.size() == 1);
	CHECK(sources[0].SourceId == "../Tone.WAV");
	CHECK((sources[0].Data.Channels == std::vector<std::vector<double>>{{0, 0.5, 1}, {1, 1.5, 255.0 / 128}}));
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"file", "pc.wav_file_read", "", {}, {{"path", std::string{"../Tone.WAV"}}}, {}},
		{"window",
		 "pc.audio_window",
		 "",
		 {},
		 {{"width", int64_t{2}},
		  {"step", int64_t{1}},
		  {"cursor_location", EnumValue{0}},
		  {"match_timeline", false}},
		 {}}
	};
	document.Links = {{"file", "data", "window", "audio_data"}};
	document.Outputs = {{"samples", "window", "bit_array"}};
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	for (const uint64_t tick : {uint64_t{0}, uint64_t{20}}) {
		EvaluationRequest request;
		request.Tick = tick;
		request.AudioClips = sources;
		studio::ImageGraphPreviewValue preview;
		REQUIRE(
			studio::EvaluateImageGraphPreview(document, plan, "samples", request, preview, diagnostic) ==
			Status::Ok
		);
		const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(preview).Data);
		CHECK((array.Nested == std::vector<std::vector<ElementValue>>{{0.0, 0.5}, {1.0, 1.5}}));
	}
	document.Nodes[0].Values.push_back({"mono", true});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.AudioClips = sources;
	studio::ImageGraphPreviewValue preview;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "samples", request, preview, diagnostic) ==
		Status::Ok
	);
	CHECK(
		(std::get<ArrayValue>(std::get<EvaluatedValue>(preview).Data).Nested ==
		 std::vector<std::vector<ElementValue>>{{0.5, 1.0}})
	);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK(std::get<std::string>(restored.Nodes[0].Values[0].Data) == "../Tone.WAV");

	document.Nodes[0].Values[0].Data = std::string{"../tone.wav"};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(
		studio::EvaluateImageGraphPreview(document, plan, "samples", request, preview, diagnostic) ==
		Status::InvalidValue
	);
	REQUIRE(cache.Store(1, 0, 0, Image{1, 1, {5, 6, 7, 255}}));
	WriteAudioTestFile(wavePath, PreviewWave(1, {128, 64, 0}));
	REQUIRE(studio::LoadImageGraphWavSource(sources, cache, "../Tone.WAV", wavePath, diagnostic));
	REQUIRE(sources.size() == 1);
	CHECK((sources[0].Data.Channels == std::vector<std::vector<double>>{{1, 0.5, 0}}));
	CHECK(cache.Find(1, 0, 0) == nullptr);
	CHECK_FALSE(studio::RemoveImageGraphWavSource(sources, cache, "../tone.wav"));
	REQUIRE(cache.Store(1, 0, 0, Image{1, 1, {5, 6, 7, 255}}));
	CHECK(studio::RemoveImageGraphWavSource(sources, cache, "../Tone.WAV"));
	CHECK(sources.empty());
	CHECK(cache.Find(1, 0, 0) == nullptr);
}

TEST_CASE("WAV host preflights replacement peak and malformed files", "[studio][imagegraph][audio_sources]") {
	using namespace engine::imagegraph;
	const auto temporary = AudioTemporaryDirectory();
	const auto path = temporary.Path / "tone.wav";
	const std::string wave = PreviewWave(1, {0, 128, 255});
	WriteAudioTestFile(path, wave);
	std::vector<AudioClipSource> sources;
	studio::ImageGraphPreviewCache cache;
	Diagnostic diagnostic;
	REQUIRE(studio::LoadImageGraphWavSource(sources, cache, "tone", path, diagnostic));
	const AudioBit original = sources[0].Data;
	const uint64_t retained = sizeof(AudioClipSource) + 4 + sizeof(std::vector<double>) + 3 * sizeof(double);
	const uint64_t conversion =
		sizeof(AudioBit) + Limits::MaximumAudioChannels * sizeof(std::vector<double>) + 6 * sizeof(double);
	const uint64_t peak = retained + sizeof(AudioClipSource) + 4 + wave.size() + conversion;
	REQUIRE(cache.Store(1, 0, 0, Image{1, 1, {5, 6, 7, 255}}));
	CHECK_FALSE(studio::LoadImageGraphWavSource(sources, cache, "next", path, diagnostic, peak - 1));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(sources.size() == 1);
	CHECK(sources[0].Data == original);
	CHECK(cache.Find(1, 0, 0) != nullptr);
	REQUIRE(studio::LoadImageGraphWavSource(sources, cache, "next", path, diagnostic, peak));
	CHECK(sources.size() == 2);
	REQUIRE(cache.Store(1, 0, 0, Image{1, 1, {5, 6, 7, 255}}));
	for (const std::string &malformed :
		 {std::string{}, std::string{"RIFFbroken"}, wave.substr(0, wave.size() - 2)}) {
		WriteAudioTestFile(path, malformed);
		CHECK_FALSE(studio::LoadImageGraphWavSource(sources, cache, "tone", path, diagnostic));
		CHECK(sources[0].Data == original);
		CHECK(cache.Find(1, 0, 0) != nullptr);
	}
	std::filesystem::resize_file(path, 16 * 1024 * 1024 + 1);
	CHECK_FALSE(studio::LoadImageGraphWavSource(sources, cache, "tone", path, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	WriteAudioTestFile(path, PreviewWave(1, std::vector<uint8_t>(Limits::MaximumAudioClipSamples + 1)));
	CHECK_FALSE(studio::LoadImageGraphWavSource(sources, cache, "tone", path, diagnostic));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(sources[0].Data == original);
	CHECK(cache.Find(1, 0, 0) != nullptr);
	CHECK_FALSE(studio::LoadImageGraphWavSource(sources, cache, "", path, diagnostic));
	CHECK_FALSE(
		studio::LoadImageGraphWavSource(
			sources, cache, "tone", path, diagnostic, Limits::MaximumEvaluationBytes + 1
		)
	);
}

TEST_CASE(
	"Studio source driver controls persist, undo and replay seeks", "[studio][imagegraph][source_drivers]"
) {
	using namespace engine::imagegraph;
	const std::array<KeyframeSourceDriver, 6> drivers{
		KeyframeLinearDriver{2},
		KeyframeSnapDriver{3},
		KeyframeBounceDriver{},
		KeyframeElasticDriver{},
		KeyframeCurveDriver{},
		KeyframeSineDriver{1, 2, 0, 0}
	};
	const std::array<double, 6> expected{22, 6, 6, 14, 6, 6};
	for (size_t kind = 0; kind < drivers.size(); kind++) {
		Document document;
		document.FormatVersion = 5;
		document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}, {}}};
		document.Outputs = {{"out", "number", "number"}};
		document.Keyframes = {{"number", "value", 2, 0.0, "linear"}, {"number", "value", 12, 10.0, "linear"}};
		document.Timeline = TimelineSettings{16, 0, 15, "loop", 30};
		const Document before = document;
		Diagnostic diagnostic;
		REQUIRE(studio::SetImageGraphKeyframeSourceDriver(document, 0, drivers[kind], diagnostic));
		CHECK(document.FormatVersion == 8);
		CHECK(document.Keyframes[1].Interpolation == "source");
		CHECK(document.Tracks.size() == 1);
		Document parsed;
		REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == document);
		studio::ImageGraphHistory history;
		history.Record(before, document);
		REQUIRE(history.Undo(document));
		CHECK(document == before);
		REQUIRE(history.Redo(document));
		CHECK(document == parsed);
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		for (const uint64_t tick : {uint64_t{8}, uint64_t{3}, uint64_t{8}}) {
			EvaluationRequest request;
			request.Tick = tick;
			studio::ImageGraphPreviewValue preview;
			REQUIRE(
				studio::EvaluateImageGraphPreview(document, plan, "out", request, preview, diagnostic) ==
				Status::Ok
			);
			if (tick == 8)
				CHECK(
					std::get<double>(std::get<EvaluatedValue>(preview).Data) == Catch::Approx(expected[kind])
				);
		}
		const Document authored = document;
		CHECK_FALSE(studio::SetImageGraphKeyframeInterpolation(document, 0, "linear", diagnostic));
		CHECK(document == authored);
		CHECK_FALSE(studio::SetImageGraphKeyframeSineDriver(document, 0, KeyframeSineDriver{}, diagnostic));
		CHECK(document == authored);
		REQUIRE(studio::SetImageGraphKeyframeSourceDriver(document, 0, std::nullopt, diagnostic));
		CHECK_FALSE(document.Keyframes[0].SourceDriver.has_value());
		CHECK(document.FormatVersion == 8);
	}
}

TEST_CASE(
	"Studio source driver validation preserves invalid edit targets", "[studio][imagegraph][source_drivers]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}, {}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {{"number", "value", 0, 0.0, "linear"}, {"number", "value", 10, 10.0, "linear"}};
	const Document original = document;
	Diagnostic diagnostic;
	KeyframeCurveDriver oversized;
	oversized.Data.Anchors.resize(Limits::MaximumCurveAnchors + 1);
	for (const KeyframeSourceDriver &driver : std::array<KeyframeSourceDriver, 4>{
			 KeyframeLinearDriver{std::numeric_limits<double>::infinity()},
			 KeyframeBounceDriver{1025, .5, 2},
			 KeyframeSineDriver{1, 1, std::numeric_limits<double>::quiet_NaN(), 0},
			 oversized
		 }) {
		CHECK_FALSE(studio::SetImageGraphKeyframeSourceDriver(document, 0, driver, diagnostic));
		CHECK(document == original);
	}
	CHECK_FALSE(studio::SetImageGraphKeyframeSourceDriver(document, 2, KeyframeSnapDriver{}, diagnostic));
	CHECK(document == original);
	REQUIRE(studio::SetImageGraphKeyframeSineDriver(document, 0, KeyframeSineDriver{}, diagnostic));
	const Document legacy = document;
	CHECK_FALSE(studio::SetImageGraphKeyframeSourceDriver(document, 0, KeyframeLinearDriver{}, diagnostic));
	CHECK(document == legacy);
	REQUIRE(studio::SetImageGraphKeyframeSineDriver(document, 0, std::nullopt, diagnostic));
	REQUIRE(
		studio::SetImageGraphKeyframeSourceDriver(document, 0, KeyframeSineDriver{1, 1, 0, 2}, diagnostic)
	);
	CHECK(document.Keyframes[0].SourceDriver.has_value());
	CHECK_FALSE(document.Keyframes[0].SineDriver.has_value());
	const Document source = document;
	CHECK_FALSE(studio::SetImageGraphTrackQuaternionMode(document, "number", "value", 1, diagnostic));
	CHECK(document == source);
}

TEST_CASE(
	"Studio quaternion mode controls survive history and actual Euler seeks",
	"[studio][imagegraph][source_drivers]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {{"angles", "pc.quarternion_to_euler", "", {}, {{"rotation", Quaternion{}}}, {}}};
	document.Outputs = {{"angles", "angles", "euler_angles"}};
	document.Keyframes = {
		{"angles", "rotation", 0, Quaternion{0, 0, 0, 0}, "linear"},
		{"angles", "rotation", 4, Quaternion{90, 0, 0, 0}, "linear"}
	};
	const Document before = document;
	Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphTrackQuaternionMode(document, "angles", "rotation", 1, diagnostic));
	CHECK(document.FormatVersion == 8);
	CHECK(document.Tracks[0].QuaternionMode == 1);
	Document parsed;
	REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	studio::ImageGraphHistory history;
	history.Record(before, document);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.Redo(document));
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	for (const uint64_t tick : {uint64_t{2}, uint64_t{4}, uint64_t{0}, uint64_t{2}}) {
		EvaluatedValue value;
		EvaluationRequest request;
		request.Tick = tick;
		REQUIRE(EvaluateValue(document, plan, "angles", request, value, diagnostic) == Status::Ok);
		CHECK((std::get<Vector3>(value.Data) == Vector3{-double(tick) * 22.5, 0, 0}));
	}
	const Document authored = document;
	CHECK_FALSE(studio::SetImageGraphTrackQuaternionMode(document, "angles", "rotation", 2, diagnostic));
	CHECK(document == authored);
	CHECK_FALSE(studio::SetImageGraphKeyframeInterpolation(document, 0, "linear", diagnostic));
	CHECK(document == authored);
	REQUIRE(studio::SetImageGraphTrackQuaternionMode(document, "angles", "rotation", 0, diagnostic));
	CHECK(document.Tracks[0].QuaternionMode == 0);
	REQUIRE(
		studio::SetImageGraphTrackQuaternionMode(document, "angles", "rotation", std::nullopt, diagnostic)
	);
	CHECK_FALSE(document.Tracks[0].QuaternionMode.has_value());
	CHECK(document.FormatVersion == 8);
}

TEST_CASE("signed author clocks seek keys and cache distinct preview frames", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	studio::ImageGraphPlayback playback;
	REQUIRE(studio::SeekImageGraphAuthorFrame(playback, -2.5, true, true));
	CHECK(studio::GetImageGraphFrame(playback) == FrameTime{2, .5, true});
	CHECK_FALSE(studio::AdvanceImageGraphPlayback(playback, 1));
	CHECK(studio::GetImageGraphFrame(playback) == FrameTime{2, .5, true});
	REQUIRE(studio::SeekImageGraphAuthorFrame(playback, -2.5, true, false));
	CHECK(studio::GetImageGraphFrame(playback) == FrameTime{2, 0, true});
	REQUIRE(studio::SeekImageGraphAuthorFrame(playback, -2.5, false, true));
	CHECK(studio::GetImageGraphFrame(playback) == FrameTime{});
	REQUIRE(studio::SeekImageGraphAuthorFrame(playback, 2.5, true, true));
	const auto saved = studio::GetImageGraphFrame(playback);
	CHECK_FALSE(
		studio::SeekImageGraphAuthorFrame(playback, std::numeric_limits<double>::infinity(), true, true)
	);
	CHECK(studio::GetImageGraphFrame(playback) == saved);
	Document document;
	document.Nodes = {{"value", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 0.0}}}};
	Diagnostic error;
	REQUIRE(studio::SetImageGraphKeyframe(document, "value", "x", 2, "linear", error, .5, true));
	REQUIRE(studio::SetImageGraphKeyframe(document, "value", "x", 2, "linear", error, .5));
	REQUIRE(studio::SetImageGraphKeyframe(document, "value", "x", 2, "linear", error));
	CHECK(document.Keyframes.size() == 3);
	CHECK(studio::PreviousImageGraphKey(document, FrameTime{2, .5, false}) == FrameTime{2, 0, false});
	CHECK(studio::NextImageGraphKey(document, FrameTime{2, .5, true}) == FrameTime{2, 0, false});
	REQUIRE(studio::RemoveImageGraphKeyframe(document, "value", "x", 2, error, .5, true));
	CHECK(document.Keyframes.size() == 2);
	CHECK_FALSE(studio::RemoveImageGraphKeyframe(document, "value", "x", 2, error, .5, true));
	studio::ImageGraphPreviewCache cache;
	Image positive{1, 1, {1, 2, 3, 255}}, negative{1, 1, {4, 5, 6, 255}};
	REQUIRE(cache.Store(1, 0, 2, positive, .5));
	REQUIRE(cache.Store(1, 0, 2, negative, .5, true));
	REQUIRE(cache.Find(1, 0, 2, .5));
	CHECK(cache.Find(1, 0, 2, .5)->Pixels == positive.Pixels);
	REQUIRE(cache.Find(1, 0, 2, .5, true));
	CHECK(cache.Find(1, 0, 2, .5, true)->Pixels == negative.Pixels);
}

TEST_CASE(
	"native key-kind staging cancels commits undoes and survives exact-time edits", "[studio][imagegraph]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 8;
	document.Nodes = {{"point", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 0.0}}}};
	document.Outputs = {{"number", "point", "x"}};
	Diagnostic error;
	REQUIRE(studio::SetImageGraphKeyframe(document, "point", "x", 2, "linear", error, .5, true));
	REQUIRE(studio::SetImageGraphKeyframe(document, "point", "x", 2, "linear", error, .5));
	const auto before = document;
	studio::KeyframeKindEditor editor;
	studio::ImageGraphHistory history;
	REQUIRE(editor.Begin(document, 0));
	REQUIRE(editor.Select(KeyframeKind::Adder));
	CHECK(document == before);
	editor.Cancel();
	CHECK_FALSE(editor.Commit(document, error));
	CHECK(document == before);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(editor.Begin(document, 0));
	REQUIRE(editor.Select(KeyframeKind::Adder));
	REQUIRE(editor.Commit(document, error));
	CHECK(document.Keyframes[0].Kind == KeyframeKind::Adder);
	CHECK(document.Keyframes[1].Kind == KeyframeKind::Normal);
	const auto after = document;
	history.Record(before, after);
	REQUIRE(history.Undo(document));
	CHECK(document == before);
	REQUIRE(history.Redo(document));
	CHECK(document == after);
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {2, .5, true}));
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "number", request, value, error) == Status::Ok);
	CHECK(std::get<double>(value.Data) == 1);
	REQUIRE(studio::SetImageGraphValue(document, "point", "x", 4.0, error));
	REQUIRE(studio::SetImageGraphKeyframe(document, "point", "x", 2, "linear", error, .5, true));
	CHECK(document.Keyframes.size() == 2);
	CHECK(document.Keyframes[0].Kind == KeyframeKind::Adder);
	CHECK(std::get<double>(document.Keyframes[0].Data) == 4);
	REQUIRE(studio::SetImageGraphKeyframe(document, "point", "x", 3, "linear", error));
	CHECK(document.Keyframes.back().Kind == KeyframeKind::Normal);
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string canvasError;
	REQUIRE(studio::LoadImageGraphCanvas(document, canvas, ids, canvasError));
	canvas.Find(ids.ToCanvas.at("point"))->X += 20;
	Document copied;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, document, ids, copied, canvasError));
	CHECK(copied.Keyframes == document.Keyframes);
	Document loaded;
	REQUIRE(Read(Write(copied), loaded, error) == Status::Ok);
	CHECK(loaded == copied);
	const auto preserved = loaded;
	CHECK_FALSE(
		studio::SetImageGraphKeyframeKind(loaded, loaded.Keyframes.size(), KeyframeKind::Normal, error)
	);
	CHECK_FALSE(studio::SetImageGraphKeyframeKind(loaded, 0, static_cast<KeyframeKind>(99), error));
	CHECK(loaded == preserved);
	Document legacy;
	legacy.FormatVersion = 8;
	legacy.Keyframes = {{"point", "x", 0, 1.0, "linear"}};
	REQUIRE(studio::SetImageGraphKeyframeKind(legacy, 0, KeyframeKind::Adder, error));
	CHECK(legacy.FormatVersion == 9);
}

TEST_CASE("key-kind drafts retain signed identity and reject stale metadata", "[studio][imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Keyframes = {{"point", "x", 2, 1.0, "linear"}, {"point", "x", 2, 2.0, "linear"}};
	REQUIRE(SetFrameTime(document.Keyframes[0], {2, .5, true}));
	REQUIRE(SetFrameTime(document.Keyframes[1], {2, .5, false}));
	studio::KeyframeKindEditor editor;
	Diagnostic error;
	REQUIRE(editor.Begin(document, 0));
	REQUIRE(editor.Select(KeyframeKind::Adder));
	std::swap(document.Keyframes[0], document.Keyframes[1]);
	REQUIRE(editor.Commit(document, error));
	CHECK(document.Keyframes[0].Kind == KeyframeKind::Normal);
	CHECK(document.Keyframes[1].Kind == KeyframeKind::Adder);
	REQUIRE(editor.Begin(document, 1));
	REQUIRE(editor.Select(KeyframeKind::Normal));
	document.Keyframes.erase(document.Keyframes.begin() + 1);
	const auto erased = document;
	CHECK_FALSE(editor.Commit(document, error));
	CHECK(document == erased);
	editor.Cancel();
	REQUIRE(editor.Begin(document, 0));
	REQUIRE(editor.Select(KeyframeKind::Normal));
	REQUIRE(studio::SetImageGraphKeyframeKind(document, 0, KeyframeKind::Adder, error));
	const auto replaced = document;
	CHECK_FALSE(editor.Commit(document, error));
	CHECK(document == replaced);
	CHECK_FALSE(editor.Select(static_cast<KeyframeKind>(99)));
	CHECK(editor.Draft == KeyframeKind::Normal);
}

TEST_CASE(
	"key transfer keeps move drivers and pastes driverless metadata clones",
	"[studio][imagegraph][timeline_keys]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {{"number", "value", 2, 2.0, "linear"}, {"number", "value", 8, 8.0, "linear"}};
	Diagnostic error;
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(document, 0, KeyframeLinearDriver{2}, error));
	REQUIRE(studio::SetImageGraphKeyframeKind(document, 0, KeyframeKind::Adder, error));
	REQUIRE(SetFrameTime(document.Keyframes[0], {2, .125, true}));
	const auto original = document.Keyframes[0];
	std::vector<Keyframe> captured;
	const std::array selection{studio::ImageGraphKeyframeIdentity{"number", "value", GetFrameTime(original)}};
	REQUIRE(studio::CaptureImageGraphKeyframes(document, selection, captured, error));
	REQUIRE(
		studio::TransferImageGraphKeyframes(
			document, captured, GetFrameTime(original), {4, .25, false}, false, error
		)
	);
	const auto moved =
		*std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
			return key.Kind == KeyframeKind::Adder;
		});
	CHECK(GetFrameTime(moved) == FrameTime{4, .25, false});
	CHECK(moved.SourceDriver == original.SourceDriver);
	CHECK(moved.Ease == original.Ease);
	CHECK(original == captured[0]);
	REQUIRE(
		studio::TransferImageGraphKeyframes(
			document, captured, GetFrameTime(original), {6, .75, false}, true, error
		)
	);
	const auto clone =
		std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
			return GetFrameTime(key) == FrameTime{6, .75, false};
		});
	REQUIRE(clone != document.Keyframes.end());
	CHECK(clone->Kind == original.Kind);
	CHECK(clone->Ease == original.Ease);
	CHECK(clone->Data == original.Data);
	CHECK_FALSE(clone->SourceDriver);
	CHECK_FALSE(clone->SineDriver);
	Document parsed;
	REQUIRE(Read(Write(document), parsed, error) == Status::Ok);
	CHECK(parsed == document);
	const auto before = document;
	CHECK_FALSE(
		studio::TransferImageGraphKeyframes(
			document, captured, GetFrameTime(original), {1, 0, false}, false, error
		)
	);
	CHECK(document == before);
	Keyframe legacy = original;
	legacy.SourceDriver.reset();
	legacy.SineDriver = KeyframeSineDriver{};
	REQUIRE(
		studio::TransferImageGraphKeyframes(
			document, std::span(&legacy, 1), GetFrameTime(legacy), {10, 0, false}, true, error
		)
	);
	CHECK_FALSE(document.Keyframes.back().SineDriver);
}

TEST_CASE(
	"key transfer preserves tiny fractions clamps negative targets and refuses overflow atomically",
	"[studio][imagegraph][timeline_keys]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {{"number", "value", 1000000, 1.0, "linear"}};
	REQUIRE(SetFrameTime(document.Keyframes[0], {1000000, 1e-20, false}));
	const auto originals = document.Keyframes;
	Diagnostic error;
	REQUIRE(
		studio::TransferImageGraphKeyframes(
			document, originals, {1000000, 0, false}, {1, 0, false}, false, error
		)
	);
	CHECK(GetFrameTime(document.Keyframes[0]) == FrameTime{1, 1e-20, false});
	const auto before = document;
	CHECK_FALSE(
		studio::TransferImageGraphKeyframes(
			document, originals, {0, 0, false}, {Limits::MaximumTick, 0, false}, true, error
		)
	);
	CHECK(document == before);
	REQUIRE(
		studio::TransferImageGraphKeyframes(
			document, originals, GetFrameTime(originals[0]), {1, .5, true}, true, error
		)
	);
	CHECK(GetFrameTime(document.Keyframes[0]) == FrameTime{});
	CHECK(GetFrameTime(document.Keyframes[1]) == FrameTime{1, 1e-20, false});
	const auto retained = document;
	std::vector<Keyframe> result = originals;
	const std::array missing{studio::ImageGraphKeyframeIdentity{"number", "value", {2, 0, false}}};
	CHECK_FALSE(studio::CaptureImageGraphKeyframes(document, missing, result, error));
	CHECK(result == originals);
	Keyframe oversized = originals[0];
	oversized.Data = std::string(Limits::MaximumTextBytes + 1, 'x');
	CHECK_FALSE(
		studio::TransferImageGraphKeyframes(
			document, std::span(&oversized, 1), GetFrameTime(oversized), {3, 0, false}, true, error
		)
	);
	CHECK(document == retained);
}

TEST_CASE(
	"clamped move collisions retain first selected key while paste replaces with last clone",
	"[studio][imagegraph][timeline_keys]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {{"number", "value", 1, 1.0, "linear"}, {"number", "value", 2, 2.0, "linear"}};
	const auto originals = document.Keyframes;
	Diagnostic error;
	REQUIRE(
		studio::TransferImageGraphKeyframes(document, originals, {1, 0, false}, {4, 0, true}, false, error)
	);
	REQUIRE(document.Keyframes.size() == 1);
	CHECK(GetFrameTime(document.Keyframes[0]) == FrameTime{});
	CHECK(std::get<double>(document.Keyframes[0].Data) == 1);
	REQUIRE(
		studio::TransferImageGraphKeyframes(document, originals, {1, 0, false}, {4, 0, true}, true, error)
	);
	REQUIRE(document.Keyframes.size() == 1);
	CHECK(std::get<double>(document.Keyframes[0].Data) == 2);
}

TEST_CASE(
	"key snapshot and transfer reject exhausted aggregate payload budgets before replacement",
	"[studio][imagegraph][timeline_keys]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"number", "value.number", "", {}, {{"value", 0.0}}}};
	document.Outputs = {{"out", "number", "number"}};
	document.Keyframes = {{"number", "value", 1, 1.0, "linear"}};
	const auto before = document;
	std::vector<Keyframe> captured = before.Keyframes;
	const std::array selected{studio::ImageGraphKeyframeIdentity{"number", "value", {1, 0, false}}};
	Diagnostic error;
	CHECK_FALSE(studio::CaptureImageGraphKeyframes(document, selected, captured, error, 1));
	CHECK(captured == before.Keyframes);
	CHECK_FALSE(
		studio::TransferImageGraphKeyframes(document, captured, {1, 0, false}, {2, 0, false}, false, error, 1)
	);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(document == before);
}

TEST_CASE(
	"targeted paste maps source display names skips missing inputs and retains copied metadata",
	"[studio][imagegraph][target_paste]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.vector3", "", {}, {{"x", 1.0}, {"y", 2.0}, {"z", 3.0}}},
		{"target", "pc.vector2", "", {}, {{"x", 0.0}, {"y", 0.0}}}
	};
	document.Outputs = {{"out", "target", "x"}};
	document.Keyframes = {
		{"source", "x", 1, 10.0, "linear"},
		{"source", "y", 3, 20.0, "linear"},
		{"source", "z", 4, 30.0, "linear"}
	};
	Diagnostic error;
	REQUIRE(studio::SetImageGraphKeyframeKind(document, 0, KeyframeKind::Adder, error));
	REQUIRE(studio::SetImageGraphKeyframeSourceDriver(document, 0, KeyframeLinearDriver{2}, error));
	const auto clipboard = document.Keyframes;
	const bool pasted = studio::PasteImageGraphKeyframesToProperty(
		document, clipboard, {5, .25, false}, "target", "y", error
	);
	INFO(error.Message);
	CAPTURE(error.Code, error.NodeId, error.Port);
	REQUIRE(pasted);
	CHECK(error.Code == Status::UnknownPort);
	REQUIRE(document.Keyframes.size() == 5);
	const auto x = std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "target" && key.Port == "x";
	});
	const auto y = std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "target" && key.Port == "y";
	});
	REQUIRE(x != document.Keyframes.end());
	REQUIRE(y != document.Keyframes.end());
	CHECK(GetFrameTime(*x) == FrameTime{5, .25, false});
	CHECK(GetFrameTime(*y) == FrameTime{7, .25, false});
	CHECK(x->Kind == KeyframeKind::Adder);
	CHECK(x->Ease == clipboard[0].Ease);
	CHECK_FALSE(x->SourceDriver);
	const auto track = std::find_if(document.Tracks.begin(), document.Tracks.end(), [](const auto &entry) {
		return entry.NodeId == "target" && entry.Port == "x";
	});
	REQUIRE(track != document.Tracks.end());
	CHECK(track->End == "hold");
	CHECK(document.Keyframes[0] == clipboard[0]);
	Document parsed;
	REQUIRE(Read(Write(document), parsed, error) == Status::Ok);
	CHECK(parsed == document);
}

TEST_CASE(
	"targeted paste refuses unrepresented numeric crossings and preserves the authored document",
	"[studio][imagegraph][target_paste]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.vector2", "", {}, {{"x", 1.0}, {"y", 2.0}}},
		{"target", "pc.vector2", "", {}, {{"x", 0.0}, {"y", 0.0}}}
	};
	document.Outputs = {{"out", "target", "x"}};
	document.Keyframes = {{"source", "x", 1, 10.0, "linear"}};
	const auto clipboard = document.Keyframes;
	Diagnostic error;
	const auto before = document;
	CHECK_FALSE(
		studio::PasteImageGraphKeyframesToProperty(
			document, clipboard, {5, 0, false}, "target", "integer", error
		)
	);
	CHECK(error.Code == Status::UnsupportedExecution);
	CHECK(document == before);
	CHECK_FALSE(
		studio::PasteImageGraphKeyframesToProperty(
			document, clipboard, {5, 0, false}, "target", "removed", error
		)
	);
	CHECK(error.Code == Status::UnknownPort);
	CHECK(document == before);
	CHECK_FALSE(
		studio::PasteImageGraphKeyframesToProperty(
			document, clipboard, {5, 0, false}, "target", "x", error, 1
		)
	);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(document == before);
}

TEST_CASE(
	"target paste reaches typed dynamic inputs and renders the authored fractional key",
	"[studio][imagegraph][target_paste]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source",
		 "pc.string_merge",
		 "",
		 {},
		 {},
		 {{"text_0", ValueType::Text, Value{std::string{"initial"}}}}},
		{"target",
		 "pc.string_merge",
		 "",
		 {},
		 {},
		 {{"text_0", ValueType::Text, Value{std::string{"initial"}}}}}
	};
	document.Keyframes = {
		{"source", "text_0", 1, std::string{"first"}, "step"},
		{"source", "text_0", 3, std::string{"last"}, "step"}
	};
	document.Outputs = {{"out", "target", "text"}};
	const auto clipboard = document.Keyframes;
	Diagnostic error;
	REQUIRE(
		studio::PasteImageGraphKeyframesToProperty(
			document, clipboard, {5, .25, false}, "target", "text_0", error
		)
	);
	Plan plan;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	REQUIRE(SetFrameTime(request, {5, .25, false}));
	EvaluatedValue result;
	REQUIRE(EvaluateValue(document, plan, "out", request, result, error) == Status::Ok);
	CHECK(std::get<std::string>(result.Data) == "first");
	REQUIRE(SetFrameTime(request, {7, .25, false}));
	REQUIRE(EvaluateValue(document, plan, "out", request, result, error) == Status::Ok);
	CHECK(std::get<std::string>(result.Data) == "last");
}

TEST_CASE(
	"typed Studio image previews convert every surface format atomically", "[studio][imagegraph][preview]"
) {
	using engine::imagegraph::Image;
	using engine::imagegraph::SurfaceFormat;
	using engine::imagegraph::SurfacePixel;
	Image rgba8Exact{1, 1, {12, 34, 56, 78}};
	std::vector<std::byte> exactDisplay;
	REQUIRE(studio::detail::PrepareImageGraphPreviewRgba8(rgba8Exact, exactDisplay));
	CHECK(
		(exactDisplay == std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{78}})
	);
	const std::array<SurfaceFormat, 7> formats{
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (const SurfaceFormat format : formats) {
		Image image;
		image.Width = 1;
		image.Height = 1;
		image.Format = format;
		const auto layout = engine::imagegraph::CheckedSurfaceLayout(1, 1, format, 16);
		REQUIRE(layout);
		image.Pixels.resize(static_cast<size_t>(layout->Bytes));
		const SurfacePixel pixel = format == SurfaceFormat::R8Unorm || format == SurfaceFormat::R16Float ||
										   format == SurfaceFormat::R32Float
									   ? SurfacePixel{.25, 0, 0, 1}
								   : format == SurfaceFormat::RGBA4Unorm ? SurfacePixel{0, .5, 1, 1}
																		 : SurfacePixel{-1, .5, 2, 1};
		REQUIRE(engine::imagegraph::StoreSurfacePixel(image, 0, 0, pixel));
		std::vector<std::byte> display{std::byte{0x12}};
		REQUIRE(studio::detail::PrepareImageGraphPreviewRgba8(image, display));
		REQUIRE(display.size() == 4);
		if (format == SurfaceFormat::RGBA8Unorm) {
			CHECK(
				(display ==
				 std::vector<std::byte>{std::byte{0}, std::byte{128}, std::byte{255}, std::byte{255}})
			);
		} else if (format == SurfaceFormat::RGBA4Unorm) {
			CHECK(
				(display ==
				 std::vector<std::byte>{std::byte{0}, std::byte{136}, std::byte{255}, std::byte{255}})
			);
		} else if (format == SurfaceFormat::R8Unorm || format == SurfaceFormat::R16Float ||
				   format == SurfaceFormat::R32Float) {
			CHECK((
				display == std::vector<std::byte>{std::byte{64}, std::byte{64}, std::byte{64}, std::byte{255}}
			));
		} else {
			CHECK(
				(display ==
				 std::vector<std::byte>{std::byte{0}, std::byte{128}, std::byte{255}, std::byte{255}})
			);
		}
	}

	Image malformed{1, 1, {1}};
	malformed.Format = SurfaceFormat::R32Float;
	std::vector<std::byte> prior{std::byte{9}, std::byte{8}};
	CHECK_FALSE(studio::detail::PrepareImageGraphPreviewRgba8(malformed, prior));
	CHECK((prior == std::vector<std::byte>{std::byte{9}, std::byte{8}}));
	Image nonfinite;
	nonfinite.Width = 1;
	nonfinite.Height = 1;
	nonfinite.Format = SurfaceFormat::RGBA32Float;
	nonfinite.Pixels.resize(16);
	nonfinite.Pixels[0] = 0x00;
	nonfinite.Pixels[1] = 0x00;
	nonfinite.Pixels[2] = 0xc0;
	nonfinite.Pixels[3] = 0x7f;
	CHECK_FALSE(studio::detail::PrepareImageGraphPreviewRgba8(nonfinite, prior));
	CHECK((prior == std::vector<std::byte>{std::byte{9}, std::byte{8}}));
	Image unknown = malformed;
	unknown.Format = static_cast<SurfaceFormat>(255);
	CHECK_FALSE(studio::detail::PrepareImageGraphPreviewRgba8(unknown, prior));
	CHECK((prior == std::vector<std::byte>{std::byte{9}, std::byte{8}}));

	Image maximum;
	maximum.Width = studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
	maximum.Height = studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
	maximum.Format = SurfaceFormat::RGBA32Float;
	maximum.Pixels.resize(studio::IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES);
	for (uint32_t y = 0; y < maximum.Height; y++)
		for (uint32_t x = 0; x < maximum.Width; x++)
			REQUIRE(engine::imagegraph::StoreSurfacePixel(maximum, x, y, {2, -1, .5, 1}));
	REQUIRE(studio::detail::PrepareImageGraphPreviewRgba8(maximum, prior));
	CHECK(prior.size() == studio::IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES);
	CHECK(prior.front() == std::byte{255});
	CHECK(prior[1] == std::byte{0});
}

TEST_CASE(
	"typed preview cache enforces image and retained caps with LRU replacement",
	"[studio][imagegraph][preview]"
) {
	using engine::imagegraph::Image;
	using engine::imagegraph::SurfaceFormat;
	studio::ImageGraphPreviewCache cache;
	Image rgba8;
	rgba8.Width = studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
	rgba8.Height = studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
	rgba8.Format = SurfaceFormat::RGBA8Unorm;
	rgba8.Pixels.resize(studio::IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES);
	Image rgba32;
	rgba32.Width = studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
	rgba32.Height = studio::IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION;
	rgba32.Format = SurfaceFormat::RGBA32Float;
	rgba32.Pixels.resize(studio::IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES);
	REQUIRE(cache.Store(1, 0, 0, rgba8));
	CHECK(cache.HeldBytes() == studio::IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES);
	cache.Clear();
	for (size_t index = 0; index < studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES; index++)
		REQUIRE(cache.Store(1, index, 0, rgba32));
	CHECK(cache.HeldBytes() == studio::IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES);
	Image tiny{1, 1, {1, 2, 3, 4}};
	REQUIRE(cache.Store(1, studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES, 0, tiny));
	CHECK(cache.Find(1, 0, 0) == nullptr);
	CHECK(cache.Find(1, 1, 0) != nullptr);
	CHECK(cache.HeldBytes() <= studio::IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES);

	cache.Clear();
	for (size_t index = 0; index < studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES; index++)
		REQUIRE(cache.Store(2, index, 0, tiny));
	CHECK(cache.HeldBytes() == studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES * tiny.Pixels.size());
	CHECK(cache.Find(2, 0, 0) != nullptr);
	REQUIRE(cache.Store(2, studio::IMAGE_COMPOSER_PREVIEW_CACHE_ENTRIES, 0, tiny));
	CHECK(cache.Find(2, 0, 0) != nullptr);
	CHECK(cache.Find(2, 1, 0) == nullptr);
	CHECK(cache.HeldBytes() <= studio::IMAGE_COMPOSER_PREVIEW_CACHE_MAXIMUM_BYTES);
	Image replacement{1, 1, {9, 8, 7, 6}};
	REQUIRE(cache.Store(2, 0, 0, replacement));
	CHECK(cache.Find(2, 0, 0)->Pixels == replacement.Pixels);
	const size_t before = cache.HeldBytes();
	Image malformed{1, 1, {1}};
	malformed.Format = SurfaceFormat::RGBA32Float;
	CHECK_FALSE(cache.Store(2, 0, 0, malformed));
	CHECK(cache.HeldBytes() == before);
	CHECK(cache.Find(2, 0, 0)->Pixels == replacement.Pixels);
}

TEST_CASE(
	"Image graph history admission runs before publishing either direction", "[studio][imagegraph][history]"
) {
	for (const bool redo : {false, true}) {
		const auto before = Fixture();
		auto after = before;
		after.Nodes[0].Values[0].Data = int64_t{17};
		studio::ImageGraphHistory history;
		REQUIRE(history.TryRecord(before, after));
		auto current = after;
		if (redo) REQUIRE(history.Undo(current));
		const auto original = current;
		const bool canUndo = history.CanUndo(), canRedo = history.CanRedo();
		int called = 0;
		const auto refuse = [&](const Document &authored, const Document &restored) {
			++called;
			CHECK(authored == original);
			CHECK(restored == (redo ? after : before));
			CHECK(current == original);
			CHECK(history.CanUndo() == canUndo);
			CHECK(history.CanRedo() == canRedo);
			return false;
		};
		CHECK_FALSE((redo ? history.Redo(current, refuse) : history.Undo(current, refuse)));
		CHECK(called == 1);
		CHECK(current == original);
		CHECK(history.CanUndo() == canUndo);
		CHECK(history.CanRedo() == canRedo);
		const auto allocationFailure = [](const Document &, const Document &) -> bool {
			throw std::bad_alloc{};
		};
		CHECK_FALSE(
			(redo ? history.Redo(current, allocationFailure) : history.Undo(current, allocationFailure))
		);
		CHECK(current == original);
		CHECK(history.CanUndo() == canUndo);
		CHECK(history.CanRedo() == canRedo);
		REQUIRE((redo ? history.Redo(current) : history.Undo(current)));
		CHECK(current == (redo ? after : before));
	}
}
TEST_CASE(
	"Image graph history budget refusal never calls runtime admission", "[studio][imagegraph][history]"
) {
	Document before, after;
	after.Nodes.push_back({"large", "unknown.large", "", {}, {{"payload", std::string(512, 'x')}}, {}});
	studio::ImageGraphHistory history(4, engine::imagegraph::Write(before).size() + 32);
	history.Record(before, after);
	bool called = false;
	CHECK_FALSE(history.Undo(after, [&](const Document &, const Document &) {
		called = true;
		return true;
	}));
	CHECK_FALSE(called);
	CHECK(history.CanUndo());
	CHECK_FALSE(history.CanRedo());
}
