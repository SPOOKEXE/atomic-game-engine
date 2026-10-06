#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceKeyframeTransition.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_separated_vec2")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::json;
	PxcxImport Checked(const Json &root) {
		engine::bake::PxcxArchive source;
		source.MetadataNumber = 121092;
		source.MetadataText = "1.22.10.201";
		source.GraphJson = root.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(source, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
}
TEST_CASE("PXC separated Vec2 IVec2 and Range edits preserve inactive tracks", "[pxcx_vec2_axes]") {
	for (const auto &[type, port] : std::array<std::pair<const char *, const char *>, 4>{
			 {{"pc.solid", "dimension"},
			  {"pc.box_pattern", "position"},
			  {"pc.matrix", "size"},
			  {"pc.colorize", "color_range"}}
		 }) {
		INFO(type << ':' << port);
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		const auto *input = FindCatalogueInput(*entry, port);
		REQUIRE(input);
		REQUIRE(input->SourceIndex >= 0);
		Json inputs = Json::array();
		for (int32_t i = 0; i <= input->SourceIndex; ++i)
			inputs.push_back(Json::object());
		inputs[input->SourceIndex] = {
			{"sep_axis", true},
			{"anim", false},
			{"r", {{"d", Json::array({2, 3})}}},
			{"animators", Json::array({Json{{"d", 2.25}}, Json{{"d", 3.5}}, Json{{"opaque_axis", "keep"}}})},
			{"opaque_input", "keep"}
		};
		Json root = {
			{"nodes",
			 Json::array(
				 {Json{{"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			 )}
		};
		auto imported = Checked(root);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		INFO((imported.Diagnostics.empty() ? "" : imported.Diagnostics.front().Message));
		REQUIRE(imported.Graph.Nodes[0].Type == type);
		REQUIRE(imported.Graph.Nodes[0].SourceSeparatedVec2Animators);
		const auto &before = imported.Graph.Nodes[0].SourceSeparatedVec2Animators->Inputs[0];
		CHECK(before.Port == port);
		CHECK(before.Separated);
		CHECK(before.Axes[0].Keys[0].Data == Value{2.25});
		CHECK(before.Axes[1].Keys[0].Data == Value{3.5});
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		auto desired = imported.Graph;
		auto &axes = desired.Nodes[0].SourceSeparatedVec2Animators->Inputs[0];
		axes.Separated = false;
		axes.Axes[0].Keys[0].Data = .875;
		std::vector<std::byte> bytes;
		const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport reopened;
		REQUIRE(ImportPxcxImageGraph(checked, reopened, failure));
		CHECK(reopened.Graph == desired);
		const auto saved =
			Json::parse(std::string_view(checked.GraphJson.data(), checked.GraphJson.size() - 1));
		const auto &record = saved["nodes"][0]["inputs"][input->SourceIndex];
		CHECK(record["sep_axis"] == false);
		CHECK(record["animators"][2]["opaque_axis"] == "keep");
		CHECK(record["opaque_input"] == "keep");
		CHECK(record["r"] == inputs[input->SourceIndex]["r"]);
	}
}

TEST_CASE("PXC IVec2 preserves only represented constant unit modes", "[pxcx_vec2_axes]") {
	const auto *entry = FindCatalogueEntry("pc.matrix");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "size");
	REQUIRE(input);
	Json record = {{"r", {{"d", Json::array({2, 3})}}}, {"anim", false}};
	Json inputs = Json::array();
	for (int32_t i = 0; i <= input->SourceIndex; ++i)
		inputs.push_back(Json::object());
	inputs[input->SourceIndex] = record;
	Json root = {
		{"nodes",
		 Json::array(
			 {Json{{"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
		 )}
	};
	CHECK(Checked(root).Graph.Nodes.front().Type == "pc.matrix");
	root["nodes"][0]["inputs"][input->SourceIndex]["unit"] = 0;
	CHECK(Checked(root).Graph.Nodes.front().Type == "pc.matrix");
	for (const Json &unit : std::array<Json, 3>{Json(1), Json("reference"), Json(true)}) {
		root["nodes"][0]["inputs"][input->SourceIndex]["unit"] = unit;
		const auto imported = Checked(root);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes.front().Type != "pc.matrix");
		REQUIRE_FALSE(imported.Diagnostics.empty());
		CHECK(imported.Diagnostics.front().Message.find("unit") != std::string::npos);
		CHECK(imported.Source.GraphJson == root.dump() + '\0');
	}
}

TEST_CASE("PXC Vec2 constructor defaults stay separate from the current animator value", "[pxcx_vec2_axes]") {
	const auto *entry = FindCatalogueEntry("pc.solid");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "dimension");
	REQUIRE(input);
	const auto importRecord = [&](Json defaultValue, bool separated = false) {
		Json inputs = Json::array();
		for (int32_t i = 0; i <= input->SourceIndex; ++i)
			inputs.push_back(Json::object());
		Json record = {
			{"anim", false},
			{"sep_axis", separated},
			{"def_val", std::move(defaultValue)},
			{"r", {{"d", Json::array({99, 88})}}}
		};
		if (separated) record["animators"] = Json::array({Json::array(), Json::array()});
		inputs[input->SourceIndex] = std::move(record);
		return Checked(
			{{"nodes",
			  Json::array(
				  {Json{{"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}}}
			  )}}
		);
	};
	const auto currentDimension = [](Document &document) -> AuthoredValue * {
		auto &values = document.Nodes.front().Values;
		const auto found = std::find_if(values.begin(), values.end(), [](const auto &value) {
			return value.Port == "dimension";
		});
		return found == values.end() ? nullptr : &*found;
	};

	auto imported = importRecord(Json::array({7, 8}));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	const auto &node = imported.Graph.Nodes.front();
	REQUIRE(node.Type == "pc.solid");
	REQUIRE(node.SourceVec2Defaults);
	REQUIRE(node.SourceVec2Defaults->Inputs.size() == 1);
	CHECK(node.SourceVec2Defaults->Inputs.front() == SourceVec2Default{"dimension", Vector2{7, 8}});
	CHECK_FALSE(node.SourceSeparatedVec2Animators);
	REQUIRE(currentDimension(imported.Graph));
	CHECK(currentDimension(imported.Graph)->Data == Value{Vector2{99, 88}});
	auto separated = importRecord(Json::array({7, 8}), true);
	REQUIRE(separated.Graph.Nodes.size() == 1);
	const auto &separatedNode = separated.Graph.Nodes.front();
	REQUIRE(separatedNode.Type == "pc.solid");
	REQUIRE(separatedNode.SourceVec2Defaults);
	CHECK(separatedNode.SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});
	REQUIRE(separatedNode.SourceSeparatedVec2Animators);
	REQUIRE(separatedNode.SourceSeparatedVec2Animators->Inputs.size() == 1);
	const auto &separatedInput = separatedNode.SourceSeparatedVec2Animators->Inputs.front();
	CHECK(separatedInput.Initialized);
	const auto &axes = separatedInput.Axes;
	REQUIRE(axes[0].Keys.size() == 1);
	REQUIRE(axes[1].Keys.size() == 1);
	CHECK(axes[0].Keys.front().Data == Value{7.0});
	CHECK(axes[1].Keys.front().Data == Value{8.0});
	REQUIRE(currentDimension(separated.Graph));
	CHECK(currentDimension(separated.Graph)->Data == Value{Vector2{99, 88}});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored == imported.Graph);

	Document edited = imported.Graph;
	auto *dimension = currentDimension(edited);
	REQUIRE(dimension);
	dimension->Data = Vector2{101, 102};
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, edited, {}, bytes, diagnostic));
	engine::bake::PxcxArchive checked;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
	PxcxImport reopened;
	REQUIRE(ImportPxcxImageGraph(checked, reopened, failure));
	REQUIRE(reopened.Graph.Nodes.front().SourceVec2Defaults);
	CHECK(reopened.Graph.Nodes.front().SourceVec2Defaults->Inputs.front().Data == Vector2{7, 8});
	REQUIRE(currentDimension(reopened.Graph));
	CHECK(currentDimension(reopened.Graph)->Data == Value{Vector2{101, 102}});
	const auto saved = Json::parse(std::string_view(checked.GraphJson.data(), checked.GraphJson.size() - 1));
	const auto &savedDimension = saved["nodes"][0]["inputs"][input->SourceIndex];
	CHECK(savedDimension["def_val"] == Json::array({7, 8}));
	CHECK(savedDimension["r"]["d"] == Json::array({101, 102}));

	for (const auto &mismatched : std::array<Json, 3>{Json::array({7}), Json::array({7, 8, 9}), Json(7)}) {
		auto ignored = importRecord(mismatched);
		REQUIRE(ignored.Graph.Nodes.size() == 1);
		CHECK(ignored.Graph.Nodes.front().Type == "pc.solid");
		CHECK_FALSE(ignored.Graph.Nodes.front().SourceVec2Defaults);
		REQUIRE(currentDimension(ignored.Graph));
		CHECK(currentDimension(ignored.Graph)->Data == Value{Vector2{99, 88}});
	}
	const auto unsupported = importRecord(Json::array({7, "unknown"}));
	REQUIRE(unsupported.Graph.Nodes.size() == 1);
	CHECK(unsupported.Graph.Nodes.front().Type != "pc.solid");
	CHECK_FALSE(unsupported.Diagnostics.empty());
	CHECK(unsupported.Diagnostics.front().Message.find("constructor default pair") != std::string::npos);

	const auto priorGraph = separated.Graph;
	const auto priorSource = separated.Source.OriginalBytes;
	Document coldDesired = separated.Graph;
	auto &coldAxes = coldDesired.Nodes.front().SourceSeparatedVec2Animators->Inputs.front();
	coldAxes.Initialized = false;
	coldAxes.Axes[0].Keys.clear();
	coldAxes.Axes[1].Keys.clear();
	currentDimension(coldDesired)->Data = Vector2{101, 102};
	std::vector<std::byte> result{std::byte{0x77}};
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
	CHECK(diagnostic.Message.find("cold scalar storage") != std::string::npos);
	currentDimension(coldDesired)->Data = Vector2{99, 88};
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
	CHECK(separated.Source.OriginalBytes == priorSource);
	CHECK(separated.Graph == priorGraph);
	coldDesired.Nodes.front().Id.assign(Limits::MaximumTextBytes + 1, 'x');
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(diagnostic.NodeId.empty());
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
	coldDesired.Nodes.front().Id = "node";
	coldDesired.Nodes.front().SourceSeparatedVec2Animators->Inputs.front().Port.assign(
		Limits::MaximumTextBytes + 1, 'x'
	);
	CHECK_FALSE(WritePxcxProjection(separated, coldDesired, {}, result, diagnostic));
	CHECK(diagnostic.Port.empty());
	CHECK(result == std::vector<std::byte>{std::byte{0x77}});
}

namespace {
	Json ScalarRow(double frame, double value, const char *tail) {
		return Json::array(
			{Json::array({0, frame, "marker"}),
			 value,
			 Json::array({.25, -.5}),
			 Json::array({.5, 1.25}),
			 1,
			 2,
			 true,
			 0,
			 16777215,
			 Json{{"future", tail}}}
		);
	}
	PxcxImport ScalarProject(bool dynamic) {
		const auto *entry = FindCatalogueEntry(dynamic ? "pc.gradient_points_n" : "pc.solid");
		REQUIRE(entry);
		Json inputs = Json::array();
		const auto record = [](double offset) {
			return Json{
				{"anim", true},
				{"sep_axis", true},
				{"r", {{"d", Json::array({8, 8})}}},
				{"animators",
				 Json::array(
					 {Json::array({ScalarRow(-1.5, 8 + offset, "x0"), ScalarRow(4.25, 10 + offset, "x1")}),
					  Json::array({ScalarRow(-1.5, 12 + offset, "y0"), ScalarRow(4.25, 14 + offset, "y1")}),
					  Json{{"future_axis", "keep"}}}
				 )},
				{"future_input", "keep"}
			};
		};
		if (dynamic) {
			REQUIRE(entry->DynamicGroupLength == 3);
			for (int32_t index = 0; index < entry->DynamicFixedLength; ++index)
				inputs.push_back(Json::object());
			for (size_t index = 0; index < 6; ++index) {
				inputs.push_back(record(double(index)));
				inputs.push_back(Json{{"r", {{"d", 16777215}}}});
				inputs.push_back(Json{{"r", {{"d", 6}}}});
			}
		} else {
			const auto *input = FindCatalogueInput(*entry, "dimension");
			REQUIRE(input);
			for (int32_t index = 0; index <= input->SourceIndex; ++index)
				inputs.push_back(Json::object());
			inputs[input->SourceIndex] = record(0);
		}
		return Checked(
			Json{
				{"animator", {{"frames_total", 16}, {"playback", 0}, {"framerate", 30}}},
				{"nodes",
				 Json::array(
					 {Json{
						  {"id", "node"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}
					  },
					  Json{
						  {"id", "sink"},
						  {"type", "Node_Project_Output"},
						  {"x", 100},
						  {"y", 0},
						  {"inputs", Json::array({Json{{"from_node", "node"}, {"from_index", 0}}})}
					  }}
				 )},
				{"future_project", "keep"}
			}
		);
	}
	Document ScalarEdit(const Document &original, std::string_view port, const KeyframeSourceDriver &driver) {
		Diagnostic error;
		FrameTime time;
		REQUIRE(SplitFrameTime(-1.5, time));
		const SourceKeyframeIdentity identity{"node", port, time, 1};
		std::vector<Keyframe> pins;
		const auto captured = CaptureSourceKeyframes(original, {&identity, 1}, pins, error);
		INFO(error.Message);
		REQUIRE(captured == Status::Ok);
		REQUIRE(pins.size() == 1);
		auto replacement = pins.front();
		replacement.Ease = KeyframeEase{"cut", "bezier", {.125, -.75}, {.625, 1.5}};
		replacement.Kind = KeyframeKind::Adder;
		replacement.SourceDriver = driver;
		const SourceKeyframeEdit edit{&pins.front(), &replacement, false, 1};
		Document changed;
		const auto status = ApplySourceKeyframeEdits(original, {&edit, 1}, changed, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return changed;
	}
	PxcxImport ReopenScalar(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport reopened;
		REQUIRE(ImportPxcxImageGraph(archive, reopened, failure));
		return reopened;
	}
	void CheckScalarRoundtrip(const PxcxImport &imported, const Document &desired, size_t sourceIndex) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool written = WritePxcxProjection(imported, desired, {}, bytes, error);
		INFO(error.Message);
		REQUIRE(written);
		const auto reopened = ReopenScalar(bytes);
		const auto &expectedInputs = desired.Nodes.front().SourceSeparatedVec2Animators->Inputs;
		const auto &actualInputs = reopened.Graph.Nodes.front().SourceSeparatedVec2Animators->Inputs;
		REQUIRE(actualInputs.size() == expectedInputs.size());
		for (size_t input = 0; input < actualInputs.size(); ++input) {
			CHECK(actualInputs[input].Port == expectedInputs[input].Port);
			for (size_t axis = 0; axis < 2; ++axis) {
				const auto &actual = actualInputs[input].Axes[axis].Keys;
				const auto &expected = expectedInputs[input].Axes[axis].Keys;
				REQUIRE(actual.size() == expected.size());
				for (size_t index = 0; index < actual.size(); ++index) {
					auto key = actual[index];
					CHECK_FALSE(key.SourceKeyId.empty());
					if (key.Kind == KeyframeKind::Adder)
						CHECK(key.SourceKeyId != expected[index].SourceKeyId);
					key.SourceKeyId = expected[index].SourceKeyId;
					CHECK(key == expected[index]);
				}
			}
		}
		CHECK(reopened.Graph.Keyframes == imported.Graph.Keyframes);
		const auto before = Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
		const auto after = Json::parse(
			std::string_view(reopened.Source.GraphJson.data(), reopened.Source.GraphJson.size() - 1)
		);
		CHECK(after["future_project"] == before["future_project"]);
		const auto &oldInput = before["nodes"][0]["inputs"][sourceIndex];
		const auto &newInput = after["nodes"][0]["inputs"][sourceIndex];
		CHECK(newInput["future_input"] == oldInput["future_input"]);
		CHECK(newInput["r"] == oldInput["r"]);
		CHECK(newInput["animators"][0] == oldInput["animators"][0]);
		CHECK(newInput["animators"][2] == oldInput["animators"][2]);
		CHECK(newInput["animators"][1][0][9] == oldInput["animators"][1][0][9]);
		CHECK(newInput["animators"][1][0][0][2] == oldInput["animators"][1][0][0][2]);
		CHECK(newInput["animators"][1][1] == oldInput["animators"][1][1]);
		std::vector<std::byte> repeated;
		REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, repeated, error));
		CHECK(repeated == bytes);
		Document native;
		REQUIRE(Read(Write(reopened.Graph), native, error) == Status::Ok);
		CHECK(native == reopened.Graph);
	}
}

TEST_CASE("PXC scalar metadata writes preserve signed clocks and opaque records", "[pxcx_vec2_axes]") {
	const auto imported = ScalarProject(false);
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.solid");
	const auto *input = FindCatalogueInput(*FindCatalogueEntry("pc.solid"), "dimension");
	REQUIRE(input);
	const std::array<KeyframeSourceDriver, 6> drivers{
		KeyframeLinearDriver{.375},
		KeyframeSnapDriver{.75},
		KeyframeBounceDriver{4, .625, 3},
		KeyframeElasticDriver{5, .75, 4},
		KeyframeCurveDriver{},
		KeyframeSineDriver{.125, 2, .25, .5}
	};
	for (const auto &driver : drivers) {
		CAPTURE(driver.index());
		const auto desired = ScalarEdit(imported.Graph, "dimension", driver);
		CheckScalarRoundtrip(imported, desired, size_t(input->SourceIndex));
	}
}

TEST_CASE("PXC native-only scalar audio refuses without replacing source bytes", "[pxcx_vec2_axes]") {
	const auto imported = ScalarProject(false);
	const auto desired =
		ScalarEdit(imported.Graph, "dimension", KeyframeAudioDriver{"clip", "peak", 1, 2, .25});
	const auto before = imported.Source.OriginalBytes;
	auto bytes = before;
	Diagnostic error;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
	CHECK(error.Code == Status::UnsupportedExecution);
	CHECK(bytes == before);
	CHECK(imported.Source.OriginalBytes == before);
}

TEST_CASE("PXC more than five separated inputs retain every declared component", "[pxcx_vec2_axes]") {
	const auto imported = ScalarProject(true);
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.gradient_points_n");
	REQUIRE(imported.Graph.Nodes.front().SourceSeparatedVec2Animators);
	REQUIRE(imported.Graph.Nodes.front().SourceSeparatedVec2Animators->Inputs.size() == 6);
	const auto desired = ScalarEdit(imported.Graph, "point_i_5", KeyframeLinearDriver{.25});
	const auto *entry = FindCatalogueEntry("pc.gradient_points_n");
	REQUIRE(entry->DynamicGroupLength == 3);
	const size_t index = entry->DynamicFixedLength + 5 * entry->DynamicGroupLength;
	CheckScalarRoundtrip(imported, desired, index);
}
