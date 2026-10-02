#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/WavPreview.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.evaluation_snapshot")
TEST_DEPENDS("engine.imagegraph.document")

using namespace engine::imagegraph;

namespace {
	Node Solid(std::string id) {
		return {
			std::move(id),
			"image.solid",
			"",
			{},
			{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 255}}}
		};
	}
	Node Transform() {
		return {
			"transform",
			"image.transform_3d",
			"",
			{},
			{{"position", Vector3{}},
			 {"anchor", Vector3{}},
			 {"rotation", Quaternion{}},
			 {"scale", Vector3{1, 1, 1}},
			 {"texture_tiling", Vector2{1, 1}},
			 {"projection", EnumValue{1}},
			 {"fov", 45.0},
			 {"view_range", Vector2{.001, 10}},
			 {"depth_range", Vector2{0, 1}}}
		};
	}
} // namespace

TEST_CASE(
	"Node input snapshots resolve timeline properties and linked images "
	"without target execution",
	"[imagegraph]"
) {
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {Solid("surface"), Transform()};
	document.Links = {{"surface", "image", "transform", "surface"}};
	document.Outputs = {{"out", "transform", "rendered"}};
	document.Keyframes = {{"transform", "fov", 0, 45.0, "linear"}, {"transform", "fov", 10, 90.0, "linear"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "transform", {.Tick = 5}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	CHECK(snapshot.Images()[0].Port == "surface");
	CHECK(snapshot.Images()[0].Data.Width == 1);
	CHECK(snapshot.Images()[0].Data.Pixels == std::vector<uint8_t>{12, 34, 56, 255});
	const auto fov = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
		return value.Port == "fov";
	});
	REQUIRE(fov != snapshot.Values().end());
	CHECK(std::get<double>(fov->Data) == 67.5);
	CHECK_FALSE(fov->Linked);
	CHECK_FALSE(fov->Domain);
	const uint64_t retained = snapshot.RetainedBytes();
	const double previousFov = std::get<double>(fov->Data);
	const auto previousPixels = snapshot.Images()[0].Data.Pixels;
	CHECK(
		EvaluateNodeInputs(document, plan, "transform", {.Tick = 5}, snapshot, diagnostic, retained) ==
		Status::LimitExceeded
	);
	CHECK(snapshot.RetainedBytes() == retained);
	const auto preservedFov =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "fov";
		});
	REQUIRE(preservedFov != snapshot.Values().end());
	CHECK(std::get<double>(preservedFov->Data) == previousFov);
	CHECK(snapshot.Images()[0].Data.Pixels == previousPixels);
}

TEST_CASE(
	"Node input snapshots resolve declared catalogue inputs without a "
	"native executor",
	"[imagegraph]"
) {
	const CatalogueEntry *pending = nullptr;
	for (const CatalogueEntry &entry : Catalogue()) {
		const CatalogueInput *surface = FindCatalogueInputIndex(entry, 0);
		if (!HasNativeExecutor(entry.Type) && surface && surface->Id == "surface_in" &&
			!entry.Outputs.empty() && entry.Outputs.front().Id == "surface_out") {
			pending = &entry;
			break;
		}
	}
	REQUIRE(pending);
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {Solid("surface"), {"pending", std::string(pending->Type), "", {}, {}}};
	document.Links = {{"surface", "image", "pending", "surface_in"}};
	document.Outputs = {{"out", "pending", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "pending", {}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 1);
	CHECK(snapshot.Images()[0].Port == "surface_in");
	CHECK(snapshot.Images()[0].Data.Pixels == std::vector<uint8_t>{12, 34, 56, 255});
	Image output;
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "pending");
	CHECK(diagnostic.Message == "node has no native executor");
}

TEST_CASE(
	"WAV preview captures linked animated controls on the shared "
	"evaluation ledger",
	"[imagegraph]"
) {
	Document document;
	document.FormatVersion = 8;
	document.Nodes = {
		{"wav", "pc.wav_file_read", "", {}, {{"path", std::string("exact.wav")}}},
		{"gain", "pc.number", "", {}, {{"value", .25}}}
	};
	document.Links = {{"gain", "number", "wav", "attribute_preview_gain"}};
	document.Outputs = {{"data", "wav", "data"}};
	document.Keyframes = {{"gain", "value", 0, .25, "linear"}, {"gain", "value", 10, .75, "step"}};
	EvaluationRequest request;
	const std::array<AudioClipSource, 1> clips{{{"exact.wav", {std::vector<double>{.25}, 48000, {}}}}};
	request.AudioClips = clips;
	request.Tick = 5;
	Diagnostic diagnostic;
	WavPreviewControls controls;
	REQUIRE(ResolveWavPreviewControls(document, "wav", request, controls, diagnostic) == Status::Ok);
	CHECK(controls.SourceId == "exact.wav");
	CHECK(controls.Gain == Catch::Approx(.5));
	CHECK(controls.SampleRate == 48000);
	CHECK(controls.Frames == 1);
}

TEST_CASE(
	"Catalogue input snapshots preserve images through static Any and "
	"dynamic Image slots",
	"[imagegraph][evaluation_snapshot]"
) {
	Document document;
	document.FormatVersion = 9;
	Node collector{"collector", "pc.array_add", "", {}, {}, {{"value_0", ValueType::Image, std::nullopt}}};
	Node first = Solid("first"), second = Solid("second");
	second.Values[2].Data = Colour{201, 102, 3, 255};
	document.Nodes = {first, second, collector};
	document.Links = {{"first", "image", "collector", "array"}, {"second", "image", "collector", "value_0"}};
	document.Outputs = {{"out", "collector", "output"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "collector", {}, snapshot, diagnostic) == Status::Ok);
	REQUIRE(snapshot.Images().size() == 2);
	CHECK(snapshot.Images()[0].Port == "array");
	CHECK(snapshot.Images()[1].Port == "value_0");
	for (const auto &image : snapshot.Images()) {
		CHECK(image.Data.Width == 1);
		CHECK(image.Data.Height == 1);
		CHECK(image.Data.Format == SurfaceFormat::RGBA8Unorm);
	}
	CHECK(snapshot.Images()[0].Data.Pixels == std::vector<uint8_t>{12, 34, 56, 255});
	CHECK(snapshot.Images()[1].Data.Pixels == std::vector<uint8_t>{201, 102, 3, 255});
	const auto *firstPixels = snapshot.Images()[0].Data.Pixels.data();
	const auto *secondPixels = snapshot.Images()[1].Data.Pixels.data();
	const uint64_t retained = snapshot.RetainedBytes();
	CHECK(
		EvaluateNodeInputs(document, plan, "collector", {}, snapshot, diagnostic, retained) ==
		Status::LimitExceeded
	);
	CHECK(snapshot.Images()[0].Data.Pixels.data() == firstPixels);
	CHECK(snapshot.Images()[1].Data.Pixels.data() == secondPixels);
	CHECK(snapshot.Images()[0].Data.Pixels == std::vector<uint8_t>{12, 34, 56, 255});
	CHECK(snapshot.Images()[1].Data.Pixels == std::vector<uint8_t>{201, 102, 3, 255});
}

namespace {
	const EvaluationInputValue &SnapshotInput(const EvaluationSnapshot &snapshot, std::string_view port) {
		const auto found =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &v) {
				return v.Port == port;
			});
		REQUIRE(found != snapshot.Values().end());
		return *found;
	}
	Document AudioSnapshotDocument(bool mono = false) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Timeline = TimelineSettings{8, 0, 7, "loop", 4};
		doc.Nodes = {
			{"path",
			 "pc.string_merge",
			 "",
			 {},
			 {},
			 {{"text_0", ValueType::Text, Value{std::string("clip")}}}},
			{"file", "pc.wav_file_read", "", {}, {{"mono", mono}}},
			{"window",
			 "pc.audio_window",
			 "",
			 {},
			 {{"width", 3.5},
			  {"step", int64_t{1}},
			  {"cursor_location", EnumValue{0}},
			  {"match_timeline", false}}}
		};
		doc.Links = {{"path", "text", "file", "path"}, {"file", "data", "window", "audio_data"}};
		doc.Outputs = {{"samples", "window", "bit_array"}, {"audio", "file", "data"}};
		return doc;
	}
} // namespace
TEST_CASE(
	"Terminal snapshots own WAV planes after caller data and graph are "
	"destroyed",
	"[imagegraph][evaluation_snapshot]"
) {
	EvaluationSnapshot snapshot;
	Diagnostic error;
	{
		Document doc = AudioSnapshotDocument();
		Document parsed;
		REQUIRE(Read(Write(doc), parsed, error) == Status::Ok);
		Plan plan;
		const auto compiled = Compile(parsed, plan, error);
		INFO(error.Message);
		REQUIRE(compiled == Status::Ok);
		std::array<AudioClipSource, 1> clips{{{"clip", {{}, 8, {{1, 2, 3, 4}, {3, 4, 5, 6}}}}}};
		EvaluationRequest request;
		request.AudioClips = clips;
		REQUIRE(SetFrameTime(request, {2, .5, true}));
		REQUIRE(EvaluateNodeInputs(parsed, plan, "window", request, snapshot, error) == Status::Ok);
		const auto &entry = SnapshotInput(snapshot, "audio_data");
		REQUIRE(entry.Linked);
		const auto &audio = std::get<AudioBit>(entry.Data);
		REQUIRE(audio.Channels.size() == 2);
		CHECK(audio.Channels[0] == std::vector<double>{1, 2, 3, 4});
		CHECK(audio.Channels[1] == std::vector<double>{3, 4, 5, 6});
		CHECK(audio.Channels[0].data() != clips[0].Data.Channels[0].data());
		CHECK(std::get<int64_t>(SnapshotInput(snapshot, "width").Data) == 4);
		clips[0].Data.Channels[0][0] = 99;
		CHECK(audio.Channels[0][0] == 1);
	}
	const auto &owned = std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data);
	CHECK(owned.SampleRate == 8);
	CHECK(owned.Channels[0] == std::vector<double>{1, 2, 3, 4});
	EvaluationSnapshot moved = std::move(snapshot);
	CHECK(
		std::get<AudioBit>(SnapshotInput(moved, "audio_data").Data).Channels[1] ==
		std::vector<double>{3, 4, 5, 6}
	);
}
TEST_CASE(
	"Terminal snapshots retain source Mono conversion and exact capture "
	"clock semantics",
	"[imagegraph][evaluation_snapshot]"
) {
	Document doc = AudioSnapshotDocument(true);
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	const std::array<AudioClipSource, 1> clips{{{"clip", {{}, 8, {{1, 2, 3, 4}, {3, 4, 5, 6}}}}}};
	EvaluationRequest request;
	request.AudioClips = clips;
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(doc, plan, "window", request, snapshot, error) == Status::Ok);
	const auto &mono = std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data);
	REQUIRE(mono.Channels.size() == 1);
	CHECK(mono.Channels[0] == std::vector<double>{2, 3, 4, 5});
	EvaluatedValue data;
	REQUIRE(EvaluateValue(doc, plan, "audio", request, data, error) == Status::Ok);
	CHECK(std::get<AudioBit>(data.Data) == mono);
	doc.Nodes[1] = {"file", "image.audio_recording", "", {}, {{"source_id", std::string("capture")}}};
	doc.Nodes[2].Values.back().Data = .75;
	doc.Links = {{"file", "audio", "window", "audio_data"}};
	doc.Outputs = {{"samples", "window", "bit_array"}};
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	const std::array<AudioCaptureFrame, 1> frames{{{"capture", 2, {}, 8, {{1, 2, 3, 4}, {3, 4, 5, 6}}}}};
	request.AudioFrames = frames;
	request.AudioClips = {};
	REQUIRE(SetFrameTime(request, {2, .5, true}));
	REQUIRE(EvaluateNodeInputs(doc, plan, "window", request, snapshot, error) == Status::Ok);
	CHECK(std::get<bool>(SnapshotInput(snapshot, "match_timeline").Data));
	CHECK(
		std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data).Channels[0] ==
		std::vector<double>{1, 2, 3, 4}
	);
	const auto old = snapshot.RetainedBytes();
	const auto *pointer = std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data).Channels[0].data();
	REQUIRE(SetFrameTime(request, {3, .5, true}));
	CHECK(EvaluateNodeInputs(doc, plan, "window", request, snapshot, error) == Status::InvalidValue);
	CHECK(snapshot.RetainedBytes() == old);
	CHECK(std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data).Channels[0].data() == pointer);
}
TEST_CASE(
	"Real static and dynamic Any input aliases retain independent "
	"AudioBit snapshots",
	"[imagegraph][evaluation_snapshot]"
) {
	Document doc = AudioSnapshotDocument();
	doc.Nodes.back() = {"window", "pc.array_add", "", {}, {}, {{"value_0", ValueType::Any, std::nullopt}}};
	doc.Links = {
		{"path", "text", "file", "path"},
		{"file", "data", "window", "array"},
		{"file", "data", "window", "value_0"}
	};
	doc.Outputs = {{"data", "window", "output"}};
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	const std::array<AudioClipSource, 1> clips{{{"clip", {{}, 8, {{1, 2, 3, 4}, {3, 4, 5, 6}}}}}};
	EvaluationRequest request;
	request.AudioClips = clips;
	EvaluationSnapshot snapshot;
	// Only input capture runs; no audio computation is attributed to Array Add.
	REQUIRE(EvaluateNodeInputs(doc, plan, "window", request, snapshot, error) == Status::Ok);
	const auto &a = std::get<AudioBit>(SnapshotInput(snapshot, "array").Data);
	const auto &b = std::get<AudioBit>(SnapshotInput(snapshot, "value_0").Data);
	CHECK(a == clips[0].Data);
	CHECK(b == clips[0].Data);
	CHECK(a.Channels[0].data() != b.Channels[0].data());
	CHECK(a.Channels[0].data() != clips[0].Data.Channels[0].data());
	CHECK(SnapshotInput(snapshot, "array").Linked);
	CHECK(SnapshotInput(snapshot, "value_0").Linked);
}
TEST_CASE(
	"Replacement AudioBit snapshot cap has an exact atomic boundary with "
	"an old owner",
	"[imagegraph][evaluation_snapshot]"
) {
	Document doc = AudioSnapshotDocument();
	Plan plan;
	Diagnostic error;
	const auto compiled = Compile(doc, plan, error);
	INFO(error.Message);
	REQUIRE(compiled == Status::Ok);
	const std::array<AudioClipSource, 1> clips{
		{{"clip", {{}, 8, {std::vector<double>(4096, .25), std::vector<double>(4096, .75)}}}}
	};
	EvaluationRequest request;
	request.AudioClips = clips;
	const auto tryCap = [&](uint64_t cap) {
		EvaluationSnapshot snapshot;
		REQUIRE(EvaluateNodeInputs(doc, plan, "window", request, snapshot, error) == Status::Ok);
		const auto *oldData =
			std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data).Channels[0].data();
		const auto oldBytes = snapshot.RetainedBytes();
		const auto result = EvaluateNodeInputs(doc, plan, "window", request, snapshot, error, cap);
		if (result != Status::Ok) {
			CHECK(result == Status::LimitExceeded);
			CHECK(snapshot.RetainedBytes() == oldBytes);
			CHECK(
				std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data).Channels[0].data() == oldData
			);
		} else {
			CHECK(
				std::get<AudioBit>(SnapshotInput(snapshot, "audio_data").Data).Channels[1] ==
				clips[0].Data.Channels[1]
			);
		}
		return result;
	};
	uint64_t low = 1, high = 1000000;
	REQUIRE(tryCap(high) == Status::Ok);
	while (low < high) {
		const auto mid = low + (high - low) / 2;
		if (tryCap(mid) == Status::Ok)
			high = mid;
		else
			low = mid + 1;
	}
	REQUIRE(low > 1);
	CHECK(tryCap(low) == Status::Ok);
	CHECK(tryCap(low - 1) == Status::LimitExceeded);
}

TEST_CASE(
	"node snapshots clone nested ordered image arrays and retain replacement on byte failure",
	"[imagegraph][snapshot_arrays]"
) {
	Document document;
	document.FormatVersion = 9;
	Node inner{"inner", "value.array", "", {}, {}};
	inner.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"repeated", ValueType::Image, std::nullopt}
	};
	Node outer{"outer", "value.array", "", {}, {}};
	outer.DynamicInputs = {
		{"nested", ValueType::Array, std::nullopt}, {"tail", ValueType::Image, std::nullopt}
	};
	document.Nodes = {Solid("surface"), inner, outer, {"capture", "pc.array_copy", "", {}, {}}};
	document.Links = {
		{"surface", "image", "inner", "first"},
		{"surface", "image", "inner", "repeated"},
		{"inner", "array", "outer", "nested"},
		{"surface", "image", "outer", "tail"},
		{"outer", "array", "capture", "array"}
	};
	document.Outputs = {{"source", "outer", "array"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	ImageArray expected;
	REQUIRE(EvaluateArray(document, plan, "source", {}, expected, error) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "capture", {}, snapshot, error) == Status::Ok);
	REQUIRE(snapshot.ImageArrays().size() == 1);
	CHECK(snapshot.ImageArrays()[0].Port == "array");
	const auto &actual = snapshot.ImageArrays()[0].Data;
	CHECK(actual.Items == expected.Items);
	REQUIRE(actual.Images.size() == expected.Images.size());
	for (size_t i = 0; i < actual.Images.size(); i++)
		CHECK(actual.Images[i].Pixels == expected.Images[i].Pixels);
	REQUIRE(actual.Items.size() == 2);
	REQUIRE(std::holds_alternative<std::vector<ImageArrayItem>>(actual.Items[0].Data));
	CHECK(std::get<std::vector<ImageArrayItem>>(actual.Items[0].Data).size() == 2);
	CHECK(snapshot.Images().empty());
	const auto retained = snapshot.RetainedBytes();
	CHECK(retained >= actual.Images.capacity() * sizeof(Image));
	uint64_t low = 1, high = Limits::MaximumEvaluationBytes;
	while (low < high) {
		const auto cap = low + (high - low) / 2;
		EvaluationSnapshot probe;
		const auto status = EvaluateNodeInputs(document, plan, "capture", {}, probe, error, cap);
		if (status == Status::Ok)
			high = cap;
		else {
			REQUIRE(status == Status::LimitExceeded);
			low = cap + 1;
		}
	}
	REQUIRE(low + retained <= Limits::MaximumEvaluationBytes);
	CHECK(
		EvaluateNodeInputs(document, plan, "capture", {}, snapshot, error, low + retained - 1) ==
		Status::LimitExceeded
	);
	CHECK(snapshot.RetainedBytes() == retained);
	REQUIRE(snapshot.ImageArrays().size() == 1);
	CHECK(snapshot.ImageArrays()[0].Data.Items == expected.Items);
	CHECK(snapshot.ImageArrays()[0].Data.Images[0].Pixels == expected.Images[0].Pixels);
	REQUIRE(EvaluateNodeInputs(document, plan, "capture", {}, snapshot, error, low + retained) == Status::Ok);
	CHECK(snapshot.ImageArrays()[0].Data.Items == expected.Items);
	auto *colour = std::get_if<Colour>(&document.Nodes[0].Values.back().Data);
	REQUIRE(colour);
	colour->Red = 250;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationSnapshot replacement;
	REQUIRE(EvaluateNodeInputs(document, plan, "capture", {}, replacement, error) == Status::Ok);
	CHECK(replacement.ImageArrays()[0].Data.Images[0].Pixels[0] == 250);
	CHECK(snapshot.ImageArrays()[0].Data.Images[0].Pixels[0] == 12);
}

TEST_CASE(
	"Native snapshots resolve linked typed controls and preserve prior captures on refusal",
	"[imagegraph][evaluation_snapshot]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.number", "", {}, {{"value", 72.0}}},
		{"target", "value.math", "", {}, {{"b", 3.0}, {"mode", int64_t{0}}}}
	};
	document.Links = {{"source", "number", "target", "a"}};
	document.Outputs = {{"out", "target", "result"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "target", {}, snapshot, diagnostic) == Status::Ok);
	const auto find = [&](std::string_view port) {
		return std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &value) {
			return value.Port == port;
		});
	};
	const auto linked = find("a");
	REQUIRE(linked != snapshot.Values().end());
	CHECK(std::get<double>(linked->Data) == 72);
	CHECK(linked->Linked);
	REQUIRE(linked->Domain);
	CHECK(linked->Domain->Type == ValueType::Scalar);
	REQUIRE(find("b") != snapshot.Values().end());
	CHECK_FALSE(find("b")->Linked);
	CHECK(std::get<double>(find("b")->Data) == 3);
	const auto bytes = snapshot.RetainedBytes();
	CHECK(EvaluateNodeInputs(document, plan, "target", {}, snapshot, diagnostic, 1) == Status::LimitExceeded);
	CHECK(snapshot.RetainedBytes() == bytes);
	CHECK(std::get<double>(find("a")->Data) == 72);
}

TEST_CASE(
	"Source Transform snapshot captures linked FOV without executing the renderer",
	"[imagegraph][evaluation_snapshot]"
) {
	Document document;
	document.FormatVersion = 9;
	Node transform = Transform();
	transform.Type = "pc.3_d_transform_image";
	document.Nodes = {Solid("surface"), {"driver", "value.number", "", {}, {{"value", 60.0}}}, transform};
	document.Links = {{"surface", "image", "transform", "surface"}, {"driver", "number", "transform", "fov"}};
	document.Outputs = {{"out", "transform", "rendered"}};
	document.Keyframes = {{"driver", "value", 0, 60.0, "linear"}, {"driver", "value", 10, 80.0, "linear"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "transform", {.Tick = 5}, snapshot, diagnostic) == Status::Ok);
	const auto find = [&] {
		return std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &value) {
			return value.Port == "fov";
		});
	};
	REQUIRE(find() != snapshot.Values().end());
	CHECK(find()->Linked);
	CHECK(std::get<double>(find()->Data) == 70.0);
	REQUIRE(snapshot.Images().size() == 1);
	const auto pixels = snapshot.Images()[0].Data.Pixels;
	const auto bytes = snapshot.RetainedBytes();
	CHECK(
		EvaluateNodeInputs(document, plan, "transform", {.Tick = 10}, snapshot, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(snapshot.RetainedBytes() == bytes);
	CHECK(std::get<double>(find()->Data) == 70.0);
	CHECK(snapshot.Images()[0].Data.Pixels == pixels);
}
