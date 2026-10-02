#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/WavClip.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcximport")
TEST_DEPENDS("engine.imagegraph.wav_clip")

// Hand mappings decline the node, so the source catalogue keeps it as a typed node with every input.
static std::string Catalogued(std::string_view foreign) {
	const auto *entry = engine::imagegraph::FindCatalogueSource(foreign);
	return entry ? std::string(entry->Type) : "missing catalogue entry";
}

using engine::bake::PxcxArchive;
using engine::bake::PxcxLimits;
using engine::bake::ReadPxcx;
using engine::bake::WritePxcx;
using engine::imagegraphio::ImportPxcxImageGraph;
using engine::imagegraphio::PxcxImport;

namespace {
	std::vector<const engine::imagegraph::Keyframe *> KeyframesFor(
		const engine::imagegraph::Document &document, std::string_view nodeId, std::string_view port
	) {
		std::vector<const engine::imagegraph::Keyframe *> result;
		for (const auto &key : document.Keyframes)
			if (key.NodeId == nodeId && key.Port == port) result.push_back(&key);
		return result;
	}
	bool HasCanonicalStaticInput(
		const engine::imagegraph::Document &document, std::string_view nodeId, std::string_view port
	) {
		using namespace engine::imagegraph;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end() ||
			std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), port) ==
				node->SourceStaticInputs.end())
			return false;
		const auto keys = KeyframesFor(document, nodeId, port);
		const auto tracks =
			std::count_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
				return track.NodeId == nodeId && track.Port == port;
			});
		return keys.size() == 1 && GetFrameTime(*keys.front()) == FrameTime{} &&
			   keys.front()->Kind == KeyframeKind::Normal && tracks == 1;
	}
	std::string Value(std::string_view json) {
		return "{\"r\":{\"d\":" + std::string(json) + "}}";
	}
	std::string Mask() {
		return R"JSON({"r":{"d":-4},"attri":{"mask_alpha_only":false}})JSON";
	}
	std::string Wire(std::string_view source) {
		return "{\"from_node\":\"" + std::string(source) + "\",\"from_index\":0}";
	}
	std::string Node(
		std::string_view id,
		std::string_view type,
		const std::vector<std::string> &inputs,
		std::string_view attributes = {}
	) {
		std::string text =
			"{\"id\":\"" + std::string(id) + "\",\"type\":\"" + std::string(type) + "\",\"x\":1,\"y\":2,";
		if (!attributes.empty()) text += "\"attri\":" + std::string(attributes) + ",";
		text += "\"inputs\":[";
		for (size_t index = 0; index < inputs.size(); index++) {
			if (index) text += ',';
			text += inputs[index];
		}
		return text + "]}";
	}
	std::vector<std::string> SolidInputs() {
		std::vector<std::string> inputs(6, Value("-4"));
		inputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
		inputs[1] = Value("4278850590");
		inputs[2] = Value("false");
		inputs[3] = Mask();
		inputs[4] = Value("true");
		return inputs;
	}
	std::vector<std::string> InvertInputs() {
		std::vector<std::string> inputs(8, Value("-4"));
		inputs[0] = Wire("solid");
		inputs[2] = Value("1");
		inputs[3] = Value("true");
		inputs[4] = Value("15");
		inputs[5] = Value("false");
		inputs[6] = Value("0");
		inputs[7] = Value("false");
		return inputs;
	}
	std::vector<std::string> BlendInputs(bool animated) {
		std::vector<std::string> inputs(16, Value("0"));
		inputs[0] = Wire("invert");
		inputs[1] = Wire("foreign");
		inputs[2] = Value("3");
		inputs[3] = animated ? R"JSON({"r":[[0,1],[10,0.5]],"anim":true})JSON" : Value("1");
		inputs[4] = Mask();
		inputs[5] = Value("0");
		inputs[6] = Value("0");
		inputs[7] = Value("[8,8]");
		inputs[8] = Value("true");
		inputs[9] = Value("false");
		inputs[10] = Value("0");
		inputs[11] = Value("0");
		inputs[12] = Value("false");
		inputs[13] = Value("1");
		inputs[14] = Value("[0.5,0.5]");
		inputs[15] = Value("false");
		return inputs;
	}
	std::vector<std::string> GradientInputs(
		bool customCurve = false,
		bool uvMap = false,
		bool mappedRadius = false,
		bool linkedAngle = false,
		int keyMode = 0
	) {
		std::vector<std::string> inputs(26, Value("-4"));
		inputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
		inputs[1] =
			R"JSON({"r":{"d":"{\"keys\":[{\"time\":0.0,\"value\":4278190080.0},{\"time\":1.0,\"value\":4294967295.0}],\"type\":0.0}"}})JSON";
		inputs[1].replace(inputs[1].rfind("0.0"), 3, std::to_string(keyMode) + ".0");
		inputs[2] = Value("0");
		inputs[3] = Value("90");
		inputs[4] = Value("0.5");
		if (mappedRadius) {
			inputs[4] = R"JSON({"r":{"d":[0.5,1.0]},"attri":{"mapped":true}})JSON";
			inputs[11] = Wire("map");
		}
		if (linkedAngle) inputs[3] = Wire("math");
		inputs[5] = Value("0");
		inputs[6] = Value("[0.5,0.5]");
		inputs[7] = Value("0");
		inputs[9] = Value("1");
		inputs[14] = Value("true");
		inputs[16] = Value("[0,0,1,0]");
		inputs[17] = Value("[1,1]");
		if (uvMap) inputs[18] = Wire("uv");
		inputs[19] = Value("1");
		const std::string identity = "[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0.3333333333333333,"
									 "-0.3333333333333333,-0.3333333333333333,1,1,0,0]";
		inputs[20] = Value(identity);
		inputs[21] = Value("0");
		inputs[22] = Value("[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0,-0.3333333333333333,0,1,0,0,0]");
		inputs[23] = Value("[0,1]");
		inputs[24] = Value(customCurve ? "[0,1,0,0,1,0,0,0,0,0,0.4,0.3,-0.3,-0.3,1,1,0,0]" : identity);
		inputs[25] = Value("[0,1]");
		return inputs;
	}
	std::vector<std::string> NumberInputs(bool integer = true) {
		std::vector<std::string> inputs(20, Value("0"));
		inputs[0] = Value("30.28");
		inputs[1] = Value(integer ? "true" : "false");
		return inputs;
	}
	std::vector<std::string> MathScalarInputs(
		int mode = 0,
		std::string_view b = "90",
		std::string_view amount = "0",
		std::string_view from = "[0,1]",
		std::string_view to = "[0,1]"
	) {
		std::vector<std::string> inputs(9, Value("0"));
		inputs[0] = Value(std::to_string(mode));
		inputs[1] = Wire("number");
		inputs[2] = Value(b);
		inputs[3] = Value("1");
		inputs[4] = Value("false");
		inputs[5] = Value(amount);
		inputs[6] = Value(from);
		inputs[7] = Value(to);
		inputs[8] = Value("false");
		return inputs;
	}
	PxcxArchive GradientFixture(
		bool customCurve = false,
		bool uvMap = false,
		bool mappedRadius = false,
		bool linkedAngle = false,
		int keyMode = 0
	) {
		const std::string graph = "{\"attributes\":{\"surface_dimension\":[8,8]},\"nodes\":[" +
								  (uvMap ? Node("uv", "Node_Solid", SolidInputs()) + "," : "") +
								  (mappedRadius ? Node("map", "Node_Solid", SolidInputs()) + "," : "") +
								  (linkedAngle ? Node("math", "Node_Math", {Value("180")}) + "," : "") +
								  Node(
									  "gradient",
									  "Node_Gradient",
									  GradientInputs(customCurve, uvMap, mappedRadius, linkedAngle, keyMode)
								  ) +
								  "," + Node("sink", "Node_Project_Output", {Wire("gradient")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	PxcxArchive ScalarAngleFixture(
		int mode = 0,
		bool animatedNumber = false,
		std::string_view b = "90",
		std::string_view amount = "0",
		std::string_view from = "[0,1]",
		std::string_view to = "[0,1]",
		bool animatedAmount = false
	) {
		std::vector<std::string> number = NumberInputs();
		if (animatedNumber) number[0] = R"JSON({"r":[[0,30],[10,40]],"anim":true})JSON";
		std::vector<std::string> math = MathScalarInputs(mode, b, amount, from, to);
		if (animatedAmount) math[5] = R"JSON({"r":[[0,0],[10,1]],"anim":true})JSON";
		const std::string graph =
			"{\"attributes\":{\"surface_dimension\":[8,8]},\"nodes\":[" +
			Node("number", "Node_Number", number) + "," + Node("math", "Node_Math", math) + "," +
			Node("gradient", "Node_Gradient", GradientInputs(false, false, false, true)) + "," +
			Node("sink", "Node_Project_Output", {Wire("gradient")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	PxcxArchive FilterFixture(bool threshold, bool animated = false) {
		std::vector<std::string> inputs(threshold ? 23 : 9, Value("0"));
		inputs[0] = Wire("solid");
		if (threshold) {
			inputs[1] = Value("true");
			inputs[2] = Value("0.25");
			inputs[3] = Value("0.125");
			inputs[4] = Value("-4");
			inputs[5] = Value("1");
			inputs[6] = Value("true");
			inputs[7] = Value("false");
			inputs[8] = Value("0.5");
			inputs[10] = Value("15");
			inputs[11] = Value("false");
			inputs[13] = Value("-4");
			inputs[14] = Value("-4");
			inputs[16] = Value("4");
			inputs[17] = Value("false");
			inputs[18] = Value("false");
			inputs[19] = Value("0");
			inputs[20] = Value("false");
			const std::string curve =
				"[0,1,0,0,1,0,0,0,0,1,0.3333333333333333,0,-0.3333333333333333,0,1,1,0,0]";
			inputs[21] = Value(curve);
			inputs[22] = Value(curve);
		} else {
			inputs[1] = animated ? R"JSON({"r":[[0,0],[10,0.5]],"anim":true})JSON" : Value("0.5");
			inputs[2] = Value("0");
			inputs[3] = Value("true");
			inputs[4] = Value("-4");
			inputs[5] = Value("1");
			inputs[6] = Value("false");
		}
		const std::string graph = "{\"attributes\":{\"surface_dimension\":[8,8]},\"nodes\":[" +
								  Node("solid", "Node_Solid", SolidInputs()) + "," +
								  Node("filter", threshold ? "Node_Threshold" : "Node_Offset", inputs) + "," +
								  Node("sink", "Node_Project_Output", {Wire("filter")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	PxcxArchive SimplexFixture(bool colored = false) {
		std::vector<std::string> inputs(20, Value("-4"));
		inputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
		inputs[1] = Value("[0,0]");
		inputs[2] = Value("[0.2,0.2]");
		inputs[3] = Value("2");
		inputs[4] = Value(colored ? "1" : "0");
		for (size_t index : {5, 6, 7})
			inputs[index] = Value("[0,1]");
		inputs[10] = Value("0");
		inputs[11] = Value("2");
		inputs[12] = Value("0.5");
		inputs[13] = Mask();
		inputs[14] = Value("397418");
		inputs[16] = Value("1");
		inputs[17] = Value("true");
		inputs[18] = Value("[0,1]");
		inputs[19] = Value("[0,1]");
		const std::string graph = "{\"attributes\":{\"surface_dimension\":[64,64]},\"nodes\":[" +
								  Node("noise", "Node_Noise_Simplex", inputs) + "," +
								  Node("sink", "Node_Project_Output", {Wire("noise")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	PxcxArchive PolarFixture(bool animated = false, int radiusMode = 0) {
		std::vector<std::string> inputs(19, Value("0"));
		inputs[0] = Wire("solid");
		inputs[1] = Value("-4");
		inputs[2] = Value("1");
		inputs[3] = Value("true");
		inputs[4] = Value("15");
		inputs[6] = Value("1");
		inputs[7] = Value("false");
		inputs[9] = Value(std::to_string(radiusMode));
		inputs[11] = Value("-4");
		inputs[12] = Value("[1,1]");
		inputs[13] = Value("[0,360]");
		inputs[15] = Value("-4");
		inputs[16] = animated ? R"JSON({"r":[[0,0],[10,360]],"anim":true})JSON" : Value("0");
		inputs[17] = Value("-4");
		inputs[18] = R"JSON({"r":{"d":[0.5,0.5]},"unit":1})JSON";
		const std::string graph =
			"{\"attributes\":{\"surface_dimension\":[8,8]},\"nodes\":[" +
			Node("solid", "Node_Solid", SolidInputs()) + "," +
			Node(
				"polar",
				"Node_Polar",
				inputs,
				R"JSON({"interpolate":2,"process":true,"oversample":0,"color_depth":0})JSON"
			) +
			"," + Node("sink", "Node_Project_Output", {Wire("polar")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	PxcxArchive Fixture(bool animated = false, uint32_t version = 121092) {
		const std::string graph = "{\"attributes\":{\"surface_dimension\":[8,8]},\"nodes\":[" +
								  Node("solid", "Node_Solid", SolidInputs()) + "," +
								  Node("invert", "Node_Invert", InvertInputs()) + "," +
								  Node("foreign", "Vendor_Future", {Value("0")}) + "," +
								  Node("blend", "Node_Blend", BlendInputs(animated)) + "," +
								  Node("sink", "Node_Project_Output", {Wire("blend"), Value("false")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = version;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	PxcxArchive CorpusControlFixture(
		std::string_view type, const std::vector<std::string> &inputs, std::string_view attributes = {}
	) {
		const std::string graph = "{\"attributes\":{\"surface_dimension\":[64,64]},\"nodes\":[" +
								  Node("solid", "Node_Solid", SolidInputs()) + "," +
								  Node("filter", type, inputs, attributes) + "," +
								  Node("sink", "Node_Project_Output", {Wire("filter")}) + "]}";
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = graph + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
	std::vector<std::string> TileCorpusInputs() {
		std::vector<std::string> inputs(13, Value("0"));
		inputs[0] = Wire("solid");
		inputs[2] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
		inputs[3] = Value("[2,2]");
		inputs[4] = Value("[0,0]");
		inputs[5] = Value("[0,0]");
		inputs[9] = Value("-4");
		inputs[10] = Value("1");
		inputs[12] = Value("[0.5,0.5]");
		return inputs;
	}
	std::vector<std::string> BlurCorpusInputs() {
		std::vector<std::string> inputs(18, Value("0"));
		inputs[0] = Wire("solid");
		inputs[1] = Value("7");
		inputs[3] = Value("false");
		inputs[4] = Value("4278190080");
		inputs[5] = Mask();
		inputs[6] = Value("1");
		inputs[7] = Value("true");
		inputs[8] = Value("7");
		inputs[9] = Value("false");
		inputs[11] = Value("false");
		inputs[12] = Value("1");
		inputs[14] = Value("-4");
		inputs[15] = Value("1");
		inputs[16] = Value("-4");
		inputs[17] = Value("[0,1,0,0,1,0,0,0,0,1,0.3333333333333333,0,-0.3333333333333333,0,1,1,0,0]");
		return inputs;
	}
	std::vector<std::string> ColorAdjustCorpusInputs() {
		std::vector<std::string> inputs(26, Value("0"));
		inputs[0] = Wire("solid");
		inputs[2] = Value("0.5");
		inputs[6] = Value("4294967295");
		inputs[8] = Mask();
		inputs[9] = Value("1");
		inputs[10] = Value("1.55");
		inputs[11] = Value("true");
		inputs[13] = Value("[]");
		inputs[15] = Value("15");
		inputs[16] = Value("false");
		inputs[17] = Value("1");
		for (size_t index = 18; index <= 25; ++index)
			inputs[index] = Value("-4");
		return inputs;
	}
	std::vector<std::string> PosterizeCorpusInputs(bool alpha = true) {
		std::vector<std::string> inputs(16, Value("-4"));
		inputs[0] = Wire("solid");
		inputs[1] = R"JSON({"r":{"d":[4278387201]},"def_val":[4278650889],"attri":{"mapped":false}})JSON";
		inputs[2] = Value("true");
		inputs[3] = Value("4");
		inputs[4] = Value("1");
		inputs[5] = Value("true");
		inputs[6] = Value(alpha ? "true" : "false");
		inputs[7] = Value("-4");
		inputs[8] = Value("0");
		inputs[9] = Value("true");
		inputs[10] = Value("0");
		inputs[11] = Value("-4");
		inputs[12] = Mask();
		inputs[13] = Value("1");
		inputs[14] = Value("false");
		inputs[15] = Value("0");
		return inputs;
	}
	std::string ReferenceValue(std::string_view json) {
		return "{\"r\":{\"d\":" + std::string(json) + "},\"unit\":1}";
	}
	std::vector<std::string> ShapeCorpusInputs(std::string_view kind = "Rectangle") {
		std::vector<std::string> inputs(53, Value("0"));
		inputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
		inputs[1] = Value("2");
		inputs[2] = Value("\"" + std::string(kind) + "\"");
		inputs[6] = Value("false");
		inputs[10] = Value("4294967295");
		inputs[11] = Value("4278190080");
		inputs[12] = Value("false");
		inputs[15] = Value("1");
		inputs[16] = ReferenceValue("[0.5,0.5]");
		inputs[17] = ReferenceValue("[0.5,0.5]");
		inputs[18] = Value("false");
		inputs[19] = Value("0");
		inputs[20] = Value("[0,1]");
		inputs[28] = Value("1");
		inputs[29] = Value(
			"[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0.3333333333333333,-0.3333333333333333,-0."
			"3333333333333333,1,1,0,0]"
		);
		inputs[32] = ReferenceValue("[0,0.40625]");
		inputs[33] = ReferenceValue("[0.5,0.609375]");
		inputs[35] = ReferenceValue("[1,0.40625]");
		inputs[37] = Value("false");
		inputs[40] = ReferenceValue("[0.5251572327044026,0.9386792452830189]");
		inputs[42] = Value("[0,0]");
		inputs[44] = Value("-4");
		inputs[45] = Value("1");
		inputs[46] = Value("-4");
		inputs[48] = Value("[0,0,0,0]");
		inputs[49] = Value("true");
		inputs[50] = Mask();
		inputs[51] = Value("false");
		inputs[52] = ReferenceValue("[0.6,0.2]");
		return inputs;
	}
	std::vector<std::string> VignetteCorpusInputs() {
		std::vector<std::string> inputs(17, Value("0"));
		inputs[0] = Wire("solid");
		inputs[1] = Value("true");
		inputs[2] = Value("60");
		inputs[3] = Value("1");
		inputs[4] = Value("0.25");
		inputs[7] = Value("-4");
		inputs[8] = Value("-4");
		inputs[9] = Value("-4");
		inputs[10] = Value(
			"[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0.3333333333333333,-0.3333333333333333,-0."
			"3333333333333333,1,1,0,0]"
		);
		inputs[11] = Value("-4");
		inputs[12] = Value("1");
		inputs[13] = Value("false");
		inputs[15] = ReferenceValue("[0.5,0.5]");
		inputs[16] = Value("4294967295");
		return inputs;
	}
	const engine::imagegraph::AuthoredValue *
	FindValue(const engine::imagegraph::Node &node, std::string_view port) {
		const auto found = std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
			return value.Port == port;
		});
		return found == node.Values.end() ? nullptr : &*found;
	}
	uint64_t PaletteHash(const engine::imagegraph::ArrayValue &palette) {
		uint64_t hash = 14695981039346656037ull;
		for (const engine::imagegraph::ElementValue &element : palette.Elements) {
			const auto &color = std::get<engine::imagegraph::Colour>(element);
			for (const uint8_t byte : {color.Red, color.Green, color.Blue, color.Alpha}) {
				hash ^= byte;
				hash *= 1099511628211ull;
			}
		}
		return hash;
	}
	void HashDouble(uint64_t &hash, double value) {
		const uint64_t bits = std::bit_cast<uint64_t>(value);
		for (unsigned shift = 0; shift < 64; shift += 8) {
			hash ^= static_cast<uint8_t>(bits >> shift);
			hash *= 1099511628211ull;
		}
	}
	uint64_t CurveControlsHash(const engine::imagegraph::Node &node) {
		uint64_t hash = 14695981039346656037ull;
		for (std::string_view port : {"brightness", "red", "green", "blue", "alpha"}) {
			const auto *value = FindValue(node, port);
			if (!value || !std::holds_alternative<engine::imagegraph::Curve>(value->Data)) return 0;
			const auto &curve = std::get<engine::imagegraph::Curve>(value->Data);
			for (double number : curve.Header)
				HashDouble(hash, number);
			for (const auto &anchor : curve.Anchors)
				for (double number : anchor)
					HashDouble(hash, number);
		}
		return hash;
	}
	uint64_t ColorizeGradientHash(const engine::imagegraph::Node &node) {
		const auto *value = FindValue(node, "gradient");
		if (!value || !std::holds_alternative<engine::imagegraph::Gradient>(value->Data)) return 0;
		uint64_t hash = 14695981039346656037ull;
		for (const auto &key : std::get<engine::imagegraph::Gradient>(value->Data).Keys) {
			HashDouble(hash, key.Time);
			for (uint8_t byte : {key.Color.Red, key.Color.Green, key.Color.Blue, key.Color.Alpha}) {
				hash ^= byte;
				hash *= 1099511628211ull;
			}
		}
		return hash;
	}
}

TEST_CASE("PXCX known controls map while unknown source remains lossless", "[imagegraphio]") {
	const PxcxArchive source = Fixture();
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(failure.empty());
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	CHECK(imported.Source.GraphJson == source.GraphJson);
	CHECK(imported.Graph.FormatVersion == 9);
	CHECK_FALSE(imported.ReferencePreview().has_value());
	CHECK(imported.NativeNodes == 3);
	REQUIRE(imported.Graph.Nodes.size() == 5);
	CHECK(imported.Graph.Nodes[0].Type == "image.solid");
	CHECK(imported.Graph.Nodes[1].Type == "image.invert");
	CHECK(imported.Graph.Nodes[2].Type == "pxcx.opaque/Vendor_Future");
	CHECK(imported.Graph.Nodes[3].Type == "image.blend");
	CHECK(imported.Graph.Nodes[4].Type == Catalogued("Node_Project_Output"));
	CHECK(imported.Graph.Nodes[0].Values[0].Data == engine::imagegraph::Value{int64_t{8}});
	CHECK(imported.Graph.Nodes[0].Values[1].Data == engine::imagegraph::Value{int64_t{8}});
	CHECK(
		imported.Graph.Nodes[0].Values[2].Data ==
		engine::imagegraph::Value{engine::imagegraph::Colour{30, 20, 10, 255}}
	);
	CHECK(imported.Graph.Nodes[3].Values[5].Data == engine::imagegraph::Value{int64_t{3}});
	CHECK(imported.Graph.Links[0].FromPort == "image");
	CHECK(imported.Graph.Links[0].ToPort == "image");
	CHECK(imported.Graph.Links[2].FromPort == "output-0");
	CHECK(imported.Graph.Links[2].ToPort == "foreground");
	REQUIRE(imported.Graph.Outputs.size() == 1);
	CHECK(imported.Graph.Outputs[0].NodeId == "blend");
	CHECK(imported.Graph.Outputs[0].Port == "image");
	CHECK_FALSE(imported.Diagnostics.empty());
	engine::imagegraph::Document nativeRoundTrip;
	engine::imagegraph::Diagnostic diagnostic;
	REQUIRE(
		engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), nativeRoundTrip, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(nativeRoundTrip == imported.Graph);
	std::vector<std::byte> written;
	REQUIRE(WritePxcx(imported.Source, written, failure));
	CHECK(written == source.OriginalBytes);
}

TEST_CASE("PXCX animated control and unknown save version remain opaque", "[imagegraphio]") {
	for (const auto &[animated, version, nativeCount] :
		 {std::tuple{true, uint32_t{121092}, size_t{2}}, std::tuple{false, uint32_t{121093}, size_t{0}}}) {
		const PxcxArchive source = Fixture(animated, version);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.NativeNodes == nativeCount);
		CHECK(imported.Graph.Nodes[3].Type == "pxcx.opaque/Node_Blend");
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
}

TEST_CASE(
	"PXCX gradient maps source curves and geometry without dropping authored values", "[imagegraphio]"
) {
	for (bool customCurve : {false, true}) {
		const PxcxArchive source = GradientFixture(customCurve);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 2);
		CHECK(imported.Graph.Nodes[0].Type == "image.gradient");
		CHECK(imported.NativeNodes == 1);
		{
			REQUIRE(imported.Graph.Nodes[0].Values.size() == 19);
			const auto *gradient =
				std::get_if<engine::imagegraph::Gradient>(&imported.Graph.Nodes[0].Values[2].Data);
			REQUIRE(gradient != nullptr);
			CHECK(gradient->Mode == 0);
			REQUIRE(gradient->Keys.size() == 2);
			CHECK(gradient->Keys[0].Color == engine::imagegraph::Colour{0, 0, 0, 255});
			CHECK(gradient->Keys[1].Color == engine::imagegraph::Colour{255, 255, 255, 255});
			const auto *curve = FindValue(imported.Graph.Nodes[0], "curve");
			REQUIRE(curve != nullptr);
			CHECK(
				std::get<engine::imagegraph::Curve>(curve->Data).Anchors[0][4] ==
				(customCurve ? 0.4 : 1.0 / 3.0)
			);
			CHECK(imported.Graph.Outputs[0].Port == "image");
		}
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
}

TEST_CASE("PXCX gradient UV link maps only its source backed image port", "[imagegraphio]") {
	const PxcxArchive source = GradientFixture(false, true);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 3);
	CHECK(imported.Graph.Nodes[1].Type == "image.gradient");
	CHECK(imported.NativeNodes == 2);
	REQUIRE(imported.Graph.Links.size() >= 1);
	CHECK(imported.Graph.Links[0].ToPort == "uv_map");
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
}

TEST_CASE(
	"PXCX Gradient maps a declared radius texture range and keeps scalar Angle opaque", "[imagegraphio]"
) {
	const PxcxArchive mapped = GradientFixture(false, false, true);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(mapped, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 3);
	CHECK(imported.Graph.Nodes[1].Type == "image.gradient");
	CHECK(imported.NativeNodes == 2);
	const auto *radius = FindValue(imported.Graph.Nodes[1], "radius");
	const auto *radiusMaximum = FindValue(imported.Graph.Nodes[1], "radius_max");
	REQUIRE(radius != nullptr);
	REQUIRE(radiusMaximum != nullptr);
	CHECK(std::get<double>(radius->Data) == 0.5);
	CHECK(std::get<double>(radiusMaximum->Data) == 1.0);
	REQUIRE(imported.Graph.Links.size() >= 1);
	CHECK(imported.Graph.Links[0].ToPort == "radius_map");
	const PxcxArchive linked = GradientFixture(false, false, false, true);
	REQUIRE(ImportPxcxImageGraph(linked, imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == Catalogued("Node_Gradient"));
	CHECK(imported.Source.OriginalBytes == linked.OriginalBytes);
}

TEST_CASE("PXCX static Number Add chain routes a typed Gradient Angle", "[imagegraphio]") {
	PxcxImport imported;
	std::string failure;
	const PxcxArchive supported = ScalarAngleFixture();
	REQUIRE(ImportPxcxImageGraph(supported, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 4);
	CHECK(imported.Graph.Nodes[0].Type == "value.number");
	CHECK(imported.Graph.Nodes[1].Type == "value.math");
	CHECK(imported.Graph.Nodes[2].Type == "image.gradient");
	CHECK(imported.NativeNodes == 3);
	const auto *number = FindValue(imported.Graph.Nodes[0], "value");
	REQUIRE(number != nullptr);
	CHECK(std::get<double>(number->Data) == 30.0);
	REQUIRE(imported.Graph.Links.size() == 3);
	CHECK(imported.Graph.Links[0].FromPort == "number");
	CHECK(imported.Graph.Links[0].ToPort == "a");
	CHECK(imported.Graph.Links[1].FromPort == "result");
	CHECK(imported.Graph.Links[1].ToPort == "angle_value");
	CHECK(imported.Source.OriginalBytes == supported.OriginalBytes);
	const auto selected = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "gradient");
	REQUIRE(selected.Compilation == engine::imagegraph::Status::Ok);
	CHECK(selected.Cuts.empty());
	engine::imagegraph::Image image;
	engine::imagegraph::Diagnostic diagnostic;
	engine::imagegraph::Plan plan;
	REQUIRE(engine::imagegraph::Compile(selected.Graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
	REQUIRE(
		engine::imagegraph::Evaluate(selected.Graph, plan, "gradient", image, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(image.Pixels[7 * 4] == 16);

	const PxcxArchive otherMode = ScalarAngleFixture(4);
	REQUIRE(ImportPxcxImageGraph(otherMode, imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == Catalogued("Node_Math"));
	CHECK(imported.Graph.Nodes[2].Type == Catalogued("Node_Gradient"));
	const PxcxArchive animated = ScalarAngleFixture(0, true);
	REQUIRE(ImportPxcxImageGraph(animated, imported, failure));
	CHECK(imported.Graph.Nodes[0].Type == "pxcx.opaque/Node_Number");
	CHECK(imported.Graph.Nodes[1].Type == Catalogued("Node_Math"));
	CHECK(imported.Graph.Nodes[2].Type == Catalogued("Node_Gradient"));
}

TEST_CASE("PXCX guarded scalar Math modes preserve exact source operands", "[imagegraphio]") {
	struct Case {
		int Mode;
		std::string_view B;
		std::string_view Amount;
		std::string_view From;
		std::string_view To;
		double Expected;
	};
	const std::array cases = {
		Case{1, "90", "0", "[0,1]", "[0,1]", -60.0},
		Case{2, "90", "0", "[0,1]", "[0,1]", 2700.0},
		Case{3, "90", "0", "[0,1]", "[0,1]", 1.0 / 3.0},
		Case{3, "0", "0", "[0,1]", "[0,1]", 0.0},
		Case{13, "90", "0.25", "[0,1]", "[0,1]", 45.0},
		Case{18, "90", "0", "[10,50]", "[-2,2]", 0.0},
		Case{18, "90", "0", "[10,20]", "[0,1]", 2.0},
	};
	for (const Case &test : cases) {
		INFO(test.Mode);
		const PxcxArchive source =
			ScalarAngleFixture(test.Mode, false, test.B, test.Amount, test.From, test.To);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes[0].Type == "value.number");
		REQUIRE(imported.Graph.Nodes[1].Type == "value.math");
		CHECK(imported.Graph.Nodes[2].Type == Catalogued("Node_Gradient"));
		engine::imagegraph::Document graph;
		graph.FormatVersion = 3;
		graph.Nodes = {imported.Graph.Nodes[0], imported.Graph.Nodes[1]};
		graph.Links = {imported.Graph.Links[0]};
		graph.Outputs = {{"value", "math", "result"}};
		engine::imagegraph::Plan plan;
		engine::imagegraph::Diagnostic diagnostic;
		REQUIRE(engine::imagegraph::Compile(graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
		engine::imagegraph::EvaluatedValue value;
		REQUIRE(
			engine::imagegraph::EvaluateValue(graph, plan, "value", {0, 0}, value, diagnostic) ==
			engine::imagegraph::Status::Ok
		);
		CHECK(std::get<double>(value.Data) == Catch::Approx(test.Expected));
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(ScalarAngleFixture(18, false, "90", "0", "[1,1]"), imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == Catalogued("Node_Math"));
	REQUIRE(ImportPxcxImageGraph(
		ScalarAngleFixture(13, false, "90", "0", "[0,1]", "[0,1]", true), imported, failure
	));
	CHECK(imported.Graph.Nodes[1].Type == "pxcx.opaque/Node_Math");
}

TEST_CASE("PXCX Gradient key interpolation imports only pinned shader modes", "[imagegraphio]") {
	PxcxImport imported;
	std::string failure;
	const PxcxArchive supported = GradientFixture(false, false, false, false, 3);
	REQUIRE(ImportPxcxImageGraph(supported, imported, failure));
	CHECK(imported.Graph.Nodes[0].Type == "image.gradient");
	const auto *value = FindValue(imported.Graph.Nodes[0], "gradient");
	REQUIRE(value != nullptr);
	CHECK(std::get<engine::imagegraph::Gradient>(value->Data).Mode == 3);
	const PxcxArchive unknown = GradientFixture(false, false, false, false, 7);
	REQUIRE(ImportPxcxImageGraph(unknown, imported, failure));
	CHECK(imported.Graph.Nodes[0].Type == "pxcx.opaque/Node_Gradient");
}

TEST_CASE("PXCX simple filters map static controls and keep animation opaque", "[imagegraphio]") {
	for (const auto &[threshold, animated, expectedType] :
		 {std::tuple{true, false, "image.threshold"},
		  std::tuple{false, false, "image.offset"},
		  std::tuple{false, true, "pxcx.opaque/Node_Offset"}}) {
		const PxcxArchive source = FilterFixture(threshold, animated);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 3);
		CHECK(imported.Graph.Nodes[1].Type == expectedType);
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		CHECK(imported.Graph.Links[0].ToPort == (animated ? "input-0" : "image"));
		CHECK(imported.Graph.Outputs[0].Port == (animated ? "output-0" : "image"));
	}
}

TEST_CASE("PXCX scalar Simplex maps while color mode remains opaque", "[imagegraphio]") {
	for (bool colored : {false, true}) {
		const PxcxArchive source = SimplexFixture(colored);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 2);
		CHECK(
			imported.Graph.Nodes[0].Type ==
			(colored ? Catalogued("Node_Noise_Simplex") : "image.noise_simplex")
		);
		if (!colored) {
			CHECK(imported.Graph.Nodes[0].Values[0].Data == engine::imagegraph::Value{int64_t{64}});
			CHECK(imported.Graph.Nodes[0].Values[1].Data == engine::imagegraph::Value{int64_t{64}});
			CHECK(imported.Graph.Nodes[0].Values[2].Data == engine::imagegraph::Value{397418.0});
			CHECK(imported.Graph.Outputs[0].Port == "image");
		}
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
}

TEST_CASE(
	"PXCX static Checker solid branch maps exact color pixels and guards render mode", "[imagegraphio]"
) {
	std::vector<std::string> inputs(14, Value("-4"));
	inputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
	inputs[1] = R"JSON({"r":{"d":0.25},"unit":1})JSON";
	inputs[2] = Value("0");
	inputs[3] = R"JSON({"r":{"d":[0.5,0.5]},"unit":1})JSON";
	inputs[4] = Value("4278190090");
	inputs[5] = Value("4278190100");
	inputs[8] = Value("0");
	inputs[9] = Value("true");
	inputs[12] = Value("1");
	inputs[13] = Value("1");
	const auto fixture = [&](const std::vector<std::string> &sourceInputs) {
		return CorpusControlFixture(
			"Node_Checker", sourceInputs, R"JSON({"process":true,"color_depth":1})JSON"
		);
	};
	PxcxImport imported;
	std::string failure;
	const PxcxArchive supported = fixture(inputs);
	REQUIRE(ImportPxcxImageGraph(supported, imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == "image.checker");
	const auto selected = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "filter");
	REQUIRE(selected.Compilation == engine::imagegraph::Status::Ok);
	CHECK(selected.Cuts.empty());
	engine::imagegraph::Plan plan;
	engine::imagegraph::Diagnostic diagnostic;
	engine::imagegraph::Image image;
	REQUIRE(engine::imagegraph::Compile(selected.Graph, plan, diagnostic) == engine::imagegraph::Status::Ok);
	REQUIRE(
		engine::imagegraph::Evaluate(selected.Graph, plan, "filter", image, diagnostic) ==
		engine::imagegraph::Status::Ok
	);
	CHECK(image.Width == 64);
	CHECK(image.Pixels[0] == 10);
	CHECK(image.Pixels[4] == 20);
	inputs[8] = Value("1");
	REQUIRE(ImportPxcxImageGraph(fixture(inputs), imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == Catalogued("Node_Checker"));
	inputs[8] = Value("0");
	inputs[1] = R"JSON({"r":[[[0,0],0.25],[[0,5],0.5]],"anim":true,"unit":1})JSON";
	REQUIRE(ImportPxcxImageGraph(fixture(inputs), imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == "pxcx.opaque/Node_Checker");
}

TEST_CASE(
	"PXCX Polar maps static source controls and preserves animated or unknown variants", "[imagegraphio]"
) {
	for (int radiusMode : {0, 1, 2, 3}) {
		const PxcxArchive source = PolarFixture(false, radiusMode);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 3);
		CHECK(imported.Graph.Nodes[1].Type == (radiusMode == 3 ? "pc.polar" : "image.polar"));
		if (radiusMode == 3) {
			const auto *raw = FindValue(imported.Graph.Nodes[1], "radius_mode");
			REQUIRE(raw);
			CHECK(raw->Data == engine::imagegraph::Value{3.0});
		}
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		if (radiusMode != 3) {
			const auto *mode = FindValue(imported.Graph.Nodes[1], "radius_mode");
			REQUIRE(mode != nullptr);
			CHECK(mode->Data == engine::imagegraph::Value{int64_t{radiusMode}});
			CHECK(imported.Graph.Links[0].ToPort == "image");
			const auto selected = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "polar");
			REQUIRE(selected.Compilation == engine::imagegraph::Status::Ok);
			CHECK(selected.Cuts.empty());
		}
	}
	const PxcxArchive animated = PolarFixture(true);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(animated, imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == "pxcx.opaque/Node_Polar");
}

TEST_CASE("PXCX selected subgraph cuts named opaque dependencies", "[imagegraphio]") {
	const PxcxArchive source = Fixture();
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	const std::vector<std::byte> original = imported.Source.OriginalBytes;
	const engine::imagegraph::Document originalGraph = imported.Graph;

	const auto sourceOnly = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "solid");
	CHECK(sourceOnly.Compilation == engine::imagegraph::Status::Ok);
	CHECK(sourceOnly.Cuts.empty());
	REQUIRE(sourceOnly.Graph.Nodes.size() == 1);
	CHECK(sourceOnly.Graph.Nodes[0].Id == "solid");
	REQUIRE(sourceOnly.Graph.Outputs.size() == 1);
	CHECK(sourceOnly.Graph.Outputs[0].Id == "solid");

	const auto dependent = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "blend");
	CHECK(dependent.Compilation == engine::imagegraph::Status::Ok);
	REQUIRE(dependent.Cuts.size() == 1);
	CHECK(dependent.Cuts[0].NodeId == "blend");
	CHECK(dependent.Cuts[0].Port == "foreground");
	CHECK(dependent.Cuts[0].Message.find("foreign.output-0 -> blend.foreground") != std::string::npos);

	const auto opaque = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "foreign");
	CHECK(opaque.Compilation == engine::imagegraph::Status::UnknownNode);
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == original);
}

TEST_CASE(
	"PXCX static corpus controls map only supported Tile Blur and Color Adjust paths", "[imagegraphio]"
) {
	for (const auto &[foreign, native, inputs] : std::array{
			 std::tuple{"Node_Tile", "image.tile", TileCorpusInputs()},
			 std::tuple{"Node_Blur", "image.blur", BlurCorpusInputs()},
			 std::tuple{"Node_Color_adjust", "image.color_adjust", ColorAdjustCorpusInputs()},
		 }) {
		INFO(foreign);
		const PxcxArchive source = CorpusControlFixture(foreign, inputs);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Graph.Nodes[1].Type == native);
		CHECK(imported.Graph.Links[0].ToPort == "image");
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		engine::imagegraph::Document roundTrip;
		engine::imagegraph::Diagnostic diagnostic;
		REQUIRE(
			engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), roundTrip, diagnostic) ==
			engine::imagegraph::Status::Ok
		);
		CHECK(roundTrip == imported.Graph);
	}
	for (const auto &[foreign, inputs] : std::array{
			 std::pair{
				 "Node_Tile",
				 [] {
					 auto v = TileCorpusInputs();
					 v[9] = Wire("solid");
					 return v;
				 }()
			 },
			 std::pair{
				 "Node_Blur",
				 [] {
					 auto v = BlurCorpusInputs();
					 v[1] = Value("10.34");
					 return v;
				 }()
			 },
			 std::pair{"Node_Color_adjust", [] {
						   auto v = ColorAdjustCorpusInputs();
						   v[1] = R"JSON({"r":[[0,0],[10,0.5]],"anim":true})JSON";
						   return v;
					   }()},
		 }) {
		INFO(foreign);
		const PxcxArchive source = CorpusControlFixture(foreign, inputs);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		// The Color Adjust case's keyframe records lack the source ease fields, so it stays opaque.
		const bool malformedAnimation = std::string_view(foreign) == "Node_Color_adjust";
		CHECK(
			imported.Graph.Nodes[1].Type ==
			(malformedAnimation ? "pxcx.opaque/" + std::string(foreign) : Catalogued(foreign))
		);
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
}

TEST_CASE("PXCX Posterize maps current typed palette and supported alpha controls", "[imagegraphio]") {
	for (const bool posterizeAlpha : {false, true}) {
		const PxcxArchive source =
			CorpusControlFixture("Node_Posterize", PosterizeCorpusInputs(posterizeAlpha));
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 3);
		const auto &posterize = imported.Graph.Nodes[1];
		CHECK(posterize.Type == "image.posterize");
		CHECK(imported.NativeNodes == 2);
		const auto *paletteValue = FindValue(posterize, "palette");
		REQUIRE(paletteValue != nullptr);
		const auto *palette = std::get_if<engine::imagegraph::ArrayValue>(&paletteValue->Data);
		REQUIRE(palette != nullptr);
		CHECK(palette->ElementType == engine::imagegraph::ValueType::Colour);
		REQUIRE(palette->Elements.size() == 1);
		CHECK(
			std::get<engine::imagegraph::Colour>(palette->Elements[0]) ==
			engine::imagegraph::Colour{1, 2, 3, 255}
		);
		CHECK(FindValue(posterize, "posterize_alpha")->Data == engine::imagegraph::Value{posterizeAlpha});
		CHECK(imported.Graph.Links[0].ToPort == "image");
		engine::imagegraph::Document roundTrip;
		engine::imagegraph::Diagnostic diagnostic;
		REQUIRE(
			engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), roundTrip, diagnostic) ==
			engine::imagegraph::Status::Ok
		);
		CHECK(roundTrip == imported.Graph);
	}

	for (const auto &[index, value] : std::array{
			 std::pair{size_t{8}, Value("1")},
			 std::pair{size_t{10}, Value("0.25")},
			 std::pair{size_t{11}, Wire("solid")},
		 }) {
		auto inputs = PosterizeCorpusInputs();
		inputs[index] = value;
		const PxcxArchive source = CorpusControlFixture("Node_Posterize", inputs);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Graph.Nodes[1].Type == Catalogued("Node_Posterize"));
	}
	auto animated = PosterizeCorpusInputs();
	animated[1] = R"JSON({"r":[[0,[4278387201]],[10,[4278650889]]],"anim":true})JSON";
	const PxcxArchive source = CorpusControlFixture("Node_Posterize", animated);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(imported.Graph.Nodes[1].Type == "pxcx.opaque/Node_Posterize");
}

TEST_CASE("PXCX source Shape controls map four static geometry kinds", "[imagegraphio]") {
	for (std::string_view kind : {"Rectangle", "Ellipse", "Half", "Triangle"}) {
		INFO(kind);
		const PxcxArchive source = CorpusControlFixture("Node_Shape", ShapeCorpusInputs(kind));
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes[1].Type == "image.shape");
		REQUIRE(imported.Graph.Links.size() == 1);
		CHECK(imported.Graph.Links[0].FromPort == "colored");
		CHECK(imported.Graph.Outputs[0].Port == "colored");
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		const auto selected = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, "filter");
		CHECK(selected.Compilation == engine::imagegraph::Status::Ok);
		REQUIRE(selected.Graph.Outputs.size() == 1);
		CHECK(selected.Graph.Outputs[0].Port == "colored");
		if (kind == "Half") {
			const auto *point = FindValue(imported.Graph.Nodes[1], "point1");
			REQUIRE(point != nullptr);
			CHECK(
				point->Data ==
				engine::imagegraph::Value{engine::imagegraph::Vector2{0.5251572327044026, 0.9386792452830189}}
			);
		}
	}
	for (size_t guard = 0; guard < 4; ++guard) {
		auto inputs = ShapeCorpusInputs();
		if (guard == 0)
			inputs[44] = Wire("solid");
		else if (guard == 1)
			inputs[29] = Value("[0,1,0,0,1,0,0,0,0,0,0.2,0.2,-0.2,-0.2,1,1,0,0]");
		else if (guard == 2)
			inputs[19] = Wire("solid");
		else
			inputs[16] = Value("[0.5,0.5]");
		const PxcxArchive source = CorpusControlFixture("Node_Shape", inputs);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Graph.Nodes[1].Type == "pxcx.opaque/Node_Shape");
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
}

TEST_CASE("PXCX scalar Vignette maps current controls and rejects sampling variants", "[imagegraphio]") {
	const PxcxArchive source = CorpusControlFixture("Node_Vignette", VignetteCorpusInputs());
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Nodes[1].Type == "image.vignette");
	CHECK(imported.Graph.Links[0].ToPort == "image");
	CHECK(imported.Graph.Nodes[1].Values[0].Port == "color");
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	for (size_t guard = 0; guard < 4; ++guard) {
		auto inputs = VignetteCorpusInputs();
		if (guard == 0)
			inputs[8] = Wire("solid");
		else if (guard == 1)
			inputs[10] = Value("[0,1,0,0,1,0,0,0,0,0,0.2,0.2,-0.2,-0.2,1,1,0,0]");
		else if (guard == 2)
			inputs[11] = Wire("solid");
		else
			inputs[15] = Value("[0.5,0.5]");
		const PxcxArchive guarded = CorpusControlFixture("Node_Vignette", inputs);
		PxcxImport rejected;
		REQUIRE(ImportPxcxImageGraph(guarded, rejected, failure));
		CHECK(rejected.Graph.Nodes[1].Type == Catalogued("Node_Vignette"));
		CHECK(rejected.Source.OriginalBytes == guarded.OriginalBytes);
	}
}

TEST_CASE("PXCX external projects import through the native adapter", "[imagegraphio]") {
	const char *directory = std::getenv("PXCX_EXTERNAL_FIXTURE_DIR");
	if (!directory || *directory == '\0') {
		SUCCEED("set PXCX_EXTERNAL_FIXTURE_DIR for external project acceptance");
		return;
	}
	constexpr std::array<std::string_view, 5> names = {
		"Black-Hole_121092.pxc",
		"Fire-Tornado_121092.pxc",
		"Glass-Block-Refraction_121092.pxc",
		"Ornate-Trim_121092.pxc",
		"Spark-Bolt_121092.pxc"
	};
	constexpr std::array<uint64_t, 5> referenceHashes = {
		0x30ea6ec3bf0179a5ull,
		0xefefd3d674d50cc5ull,
		0x779230334d52dc05ull,
		0x231063ff80bc8c45ull,
		0x16cd849cebd68e25ull
	};
	constexpr std::array<std::array<size_t, 6>, 5> mappedCorpusControls = {
		{{0, 2, 0, 1, 2, 0}, {0, 0, 1, 2, 0, 1}, {1, 1, 4, 1, 2, 1}, {2, 0, 0, 1, 4, 0}, {0, 0, 0, 4, 0, 2}}
	};
	constexpr std::array<size_t, 5> sourceShapeCounts = {6, 0, 2, 4, 0};
	constexpr std::array<size_t, 5> sourceVignetteCounts = {0, 1, 1, 0, 2};
	constexpr std::array<size_t, 5> curveCounts = {3, 1, 1, 0, 0};
	constexpr std::array<size_t, 5> colorizeCounts = {1, 1, 0, 0, 0};
	constexpr std::array<size_t, 5> polarCounts = {1, 0, 0, 0, 1};
	constexpr std::array<size_t, 5> checkerCounts = {0, 0, 0, 0, 1};
	struct ExpectedMappedControl {
		std::string_view Id;
		uint64_t Hash;
	};
	constexpr std::array<ExpectedMappedControl, 5> expectedCurves = {
		{{"gSJ9DY1357376sG5hKyFm8Pl4MnHgFvQ", 0x8dc9e1eb917c5c27ull},
		 {"gSJ9p73609810pGTsEjhA7j1aLqlnSJj", 0xe9ddc742c223e475ull},
		 {"gSJ9vg4005454VfMJJLrQeazTv3tLbZE", 0x07abb28f9a471e56ull},
		 {"gSGCK0576017lkqUgQXN68hZn6P7feEc", 0x637bc9a2d0e26b93ull},
		 {"gSDFJV1296798u16SUCVA9wHTsEloRuy", 0x6ffef13427e256e1ull}}
	};
	constexpr std::array<ExpectedMappedControl, 2> expectedColorizes = {
		{{"gSJ9DA1333258rnJX1rGmdSC1fBM2JsX", 0xdbfdf40d7ab9f4b4ull},
		 {"gSGCaS064518hvBP7U7y1hPAUXGBArhJ", 0x31672581f0f0c41full}}
	};
	for (size_t index = 0; index < names.size(); index++) {
		const std::string_view name = names[index];
		INFO(name);
		std::ifstream input(std::filesystem::path(directory) / name, std::ios::binary | std::ios::ate);
		REQUIRE(input.is_open());
		const std::streamsize count = input.tellg();
		REQUIRE(count > 0);
		REQUIRE(static_cast<uint64_t>(count) <= PxcxLimits::MaximumArchiveBytes);
		std::vector<std::byte> bytes(static_cast<size_t>(count));
		input.seekg(0);
		input.read(reinterpret_cast<char *>(bytes.data()), count);
		REQUIRE(input.gcount() == count);
		PxcxArchive source;
		PxcxImport imported;
		std::string failure;
		REQUIRE(ReadPxcx(bytes, source, failure));
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		const std::array<size_t, 6> mapped = {
			static_cast<size_t>(std::count_if(
				imported.Graph.Nodes.begin(),
				imported.Graph.Nodes.end(),
				[](const auto &node) { return node.Type == "image.tile"; }
			)),
			static_cast<size_t>(std::count_if(
				imported.Graph.Nodes.begin(),
				imported.Graph.Nodes.end(),
				[](const auto &node) { return node.Type == "image.blur"; }
			)),
			static_cast<size_t>(std::count_if(
				imported.Graph.Nodes.begin(),
				imported.Graph.Nodes.end(),
				[](const auto &node) { return node.Type == "image.color_adjust"; }
			)),
			static_cast<size_t>(std::count_if(
				imported.Graph.Nodes.begin(),
				imported.Graph.Nodes.end(),
				[](const auto &node) { return node.Type == "image.posterize"; }
			)),
			static_cast<size_t>(std::count_if(
				imported.Graph.Nodes.begin(),
				imported.Graph.Nodes.end(),
				[](const auto &node) { return node.Type == "image.shape"; }
			)),
			static_cast<size_t>(
				std::count_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
					return node.Type == "image.vignette";
				})
			)
		};
		CHECK(mapped == mappedCorpusControls[index]);
		CHECK(std::count_if(source.Nodes.begin(), source.Nodes.end(), [](const auto &node) {
				  return node.Type == "Node_Shape";
			  }) == static_cast<std::ptrdiff_t>(sourceShapeCounts[index]));
		CHECK(std::count_if(source.Nodes.begin(), source.Nodes.end(), [](const auto &node) {
				  return node.Type == "Node_Vignette";
			  }) == static_cast<std::ptrdiff_t>(sourceVignetteCounts[index]));
		size_t curveCount = 0, colorizeCount = 0;
		for (const auto &node : imported.Graph.Nodes) {
			if (node.Type == "image.curve") {
				++curveCount;
				const auto expected =
					std::find_if(expectedCurves.begin(), expectedCurves.end(), [&](const auto &row) {
						return row.Id == node.Id;
					});
				REQUIRE(expected != expectedCurves.end());
				CHECK(CurveControlsHash(node) == expected->Hash);
			} else if (node.Type == "image.colorize") {
				++colorizeCount;
				const auto expected =
					std::find_if(expectedColorizes.begin(), expectedColorizes.end(), [&](const auto &row) {
						return row.Id == node.Id;
					});
				REQUIRE(expected != expectedColorizes.end());
				CHECK(ColorizeGradientHash(node) == expected->Hash);
			}
		}
		CHECK(curveCount == curveCounts[index]);
		CHECK(colorizeCount == colorizeCounts[index]);
		CHECK(std::count_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
				  return node.Type == "image.checker";
			  }) == static_cast<std::ptrdiff_t>(checkerCounts[index]));
		if (index == 4) {
			const auto checker =
				std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
					return node.Id == "gSI8FK4886408CHLMA4b5jKIu8qXpKma";
				});
			REQUIRE(checker != imported.Graph.Nodes.end());
			CHECK(checker->Type == "image.checker");
			const auto selected = engine::imagegraphio::ExtractPxcxNativeSubgraph(imported, checker->Id);
			REQUIRE(selected.Compilation == engine::imagegraph::Status::Ok);
			CHECK(selected.Cuts.empty());
			engine::imagegraph::Plan checkerPlan;
			engine::imagegraph::Diagnostic checkerDiagnostic;
			engine::imagegraph::Image checkerImage;
			REQUIRE(
				engine::imagegraph::Compile(selected.Graph, checkerPlan, checkerDiagnostic) ==
				engine::imagegraph::Status::Ok
			);
			REQUIRE(
				engine::imagegraph::Evaluate(
					selected.Graph, checkerPlan, checker->Id, checkerImage, checkerDiagnostic
				) == engine::imagegraph::Status::Ok
			);
			CHECK(checkerImage.Width == 64);
			CHECK(checkerImage.Pixels[0] == 0);
			CHECK(checkerImage.Pixels[4] == 38);
			CHECK(checkerImage.Pixels[(size_t(32) * 64 + 32) * 4] == 38);
		}
		CHECK(std::count_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
				  return node.Type == "image.polar";
			  }) == static_cast<std::ptrdiff_t>(polarCounts[index]));
		if (index == 1) {
			// The animated Polar keeps its source keyframes on the catalogue node.
			const auto animatedPolar =
				std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
					return node.Type == "pc.polar";
				});
			REQUIRE(animatedPolar != imported.Graph.Nodes.end());
			CHECK(
				std::any_of(
					imported.Graph.Keyframes.begin(),
					imported.Graph.Keyframes.end(),
					[&](const auto &keyframe) {
						return keyframe.NodeId == animatedPolar->Id && keyframe.Interpolation == "source";
					}
				)
			);
		}
		struct ExpectedPalette {
			std::string_view Id;
			size_t Count;
			uint64_t Hash;
			bool PosterizeAlpha;
		};
		constexpr std::array<ExpectedPalette, 9> expectedPalettes = {{
			{"gSJ9FJ1462470fe1nOcMQw1Z9qDyCJKf", 9, 0x0e134f4afb1923bf, true},
			{"gSGCBu093007h2EO4sGGQShkkTstE0pu", 32, 0x028d11fad26b9152, false},
			{"gSGCdv273281U95ck93aKFVYaOVxXR3b", 32, 0x028d11fad26b9152, true},
			{"gSDFAi769794aqfifGPMnFF0oXfSSMt2", 32, 0x028d11fad26b9152, true},
			{"gSJCdK224532vO6H13ZkCKIUuRop6Zq5", 8, 0xefac4e49b0ba24b2, true},
			{"gSI7P41870945Fsn2KyVsiza6JpYeF4c", 32, 0x028d11fad26b9152, true},
			{"gSI8Ci4730754a4kWyNz7Kq6wj2rB60M", 32, 0x028d11fad26b9152, true},
			{"gSI8Gr4980237wndWmO4ZDvLJD0KQYjP", 7, 0x0d305b2cc3370811, true},
			{"gSI8Kg5208654sdkZlea7b6nCbzqSFtm", 32, 0x028d11fad26b9152, true},
		}};
		size_t posterizeCount = 0;
		for (const auto &nativeNode : imported.Graph.Nodes) {
			if (nativeNode.Type != "image.posterize") continue;
			++posterizeCount;
			INFO(nativeNode.Id);
			const auto expected =
				std::find_if(expectedPalettes.begin(), expectedPalettes.end(), [&](const auto &row) {
					return row.Id == nativeNode.Id;
				});
			REQUIRE(expected != expectedPalettes.end());
			const auto *paletteValue = FindValue(nativeNode, "palette");
			REQUIRE(paletteValue != nullptr);
			const auto *palette = std::get_if<engine::imagegraph::ArrayValue>(&paletteValue->Data);
			REQUIRE(palette != nullptr);
			CHECK(palette->ElementType == engine::imagegraph::ValueType::Colour);
			CHECK(palette->Elements.size() == expected->Count);
			CHECK(PaletteHash(*palette) == expected->Hash);
			const auto *alpha = FindValue(nativeNode, "posterize_alpha");
			REQUIRE(alpha != nullptr);
			CHECK(alpha->Data == engine::imagegraph::Value{expected->PosterizeAlpha});
		}
		CHECK(posterizeCount == mappedCorpusControls[index][3]);
		// Fire-Tornado's Path node keys anchors at fractional frames, which native ticks cannot hold.
		constexpr std::array<size_t, 5> opaqueCounts = {0, 1, 0, 0, 0};
		CHECK(std::count_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
				  return node.Type.starts_with("pxcx.opaque/");
			  }) == static_cast<std::ptrdiff_t>(opaqueCounts[index]));
		CHECK(imported.CatalogueNodes + imported.NativeNodes + opaqueCounts[index] == source.Nodes.size());
		REQUIRE(imported.Graph.Project);
		CHECK(imported.Graph.Project->SurfaceWidth == 64);
		REQUIRE(imported.Graph.Timeline);
		engine::imagegraph::Plan importedPlan;
		engine::imagegraph::Diagnostic importedDiagnostic;
		if (opaqueCounts[index] == 0) {
			INFO(importedDiagnostic.Message);
			CHECK(
				engine::imagegraph::Compile(imported.Graph, importedPlan, importedDiagnostic) ==
				engine::imagegraph::Status::Ok
			);
		}
		CHECK(imported.Source.OriginalBytes == bytes);
		CHECK(imported.Graph.Nodes.size() == source.Nodes.size());
		CHECK(imported.Graph.Links.size() == source.Links.size());
		if (index == 0) {
			const auto findNode = [&](std::string_view id) {
				return std::find_if(
					imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [&](const auto &node) {
						return node.Id == id;
					}
				);
			};
			const auto number = findNode("gSJ9h83131344ufUsbDrtj4IrQ39l8kL");
			const auto math = findNode("gSJ9iV3214367wfVodNULBU5tC393UqH");
			const auto gradient = findNode("gSJ9Q42107231Xx7MdqeVcCtDUPilYEM");
			REQUIRE(number != imported.Graph.Nodes.end());
			REQUIRE(math != imported.Graph.Nodes.end());
			REQUIRE(gradient != imported.Graph.Nodes.end());
			CHECK(number->Type == "value.number");
			CHECK(math->Type == "value.math");
			CHECK(gradient->Type == "image.gradient");
			const auto *value = FindValue(*number, "value");
			REQUIRE(value != nullptr);
			CHECK(std::get<double>(value->Data) == 30.0);
			CHECK(
				std::any_of(imported.Graph.Links.begin(), imported.Graph.Links.end(), [&](const auto &link) {
					return link.FromNode == number->Id && link.FromPort == "number" &&
						   link.ToNode == math->Id && link.ToPort == "a";
				})
			);
			CHECK(
				std::any_of(imported.Graph.Links.begin(), imported.Graph.Links.end(), [&](const auto &link) {
					return link.FromNode == math->Id && link.FromPort == "result" &&
						   link.ToNode == gradient->Id && link.ToPort == "angle_value";
				})
			);
			CHECK(
				std::count_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
					return node.Type == "value.math";
				}) == 2
			);
		} else if (index == 4) {
			const auto number =
				std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
					return node.Id == "gSI7pH3443622TxtSg7lqko1MozxXOt3";
				});
			REQUIRE(number != imported.Graph.Nodes.end());
			CHECK(number->Type == Catalogued("Node_Number"));
		}
		for (size_t linkIndex = 0; linkIndex < source.Links.size(); ++linkIndex) {
			const auto &sourceLink = source.Links[linkIndex];
			const auto shape =
				std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [&](const auto &node) {
					return node.Id == sourceLink.FromNode && node.Type == "image.shape";
				});
			if (shape == imported.Graph.Nodes.end()) continue;
			constexpr std::array<std::string_view, 4> outputs = {"colored", "mask", "height", "uv"};
			REQUIRE(sourceLink.FromIndex < outputs.size());
			CHECK(imported.Graph.Links[linkIndex].FromPort == outputs[sourceLink.FromIndex]);
		}
		engine::imagegraph::Document nativeRoundTrip;
		engine::imagegraph::Diagnostic diagnostic;
		const auto roundTripStatus =
			engine::imagegraph::Read(engine::imagegraph::Write(imported.Graph), nativeRoundTrip, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(roundTripStatus == engine::imagegraph::Status::Ok);
		CHECK(nativeRoundTrip == imported.Graph);
		CHECK(imported.NativeNodes > 0);
		CHECK(imported.NativeNodes < source.Nodes.size());
		CHECK_FALSE(imported.Diagnostics.empty());
		const auto preview = imported.ReferencePreview();
		REQUIRE(preview.has_value());
		CHECK(preview->Width == 256);
		CHECK(preview->Height == 256);
		CHECK(preview->Rgba.size() == PxcxLimits::ThumbnailRgbaBytes);
		CHECK(preview->Hash == referenceHashes[index]);
	}
}

namespace {
	// Source-backed shape: Node_Audio_Window initializes inputs[2].attributes.unit, and node_value's
	// serialize() clones that dictionary to the input record's attri object.
	PxcxArchive AudioWindowUnitFixture(std::string_view locationRecord, bool linkedLocation = false) {
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		std::string linked;
		if (linkedLocation) {
			std::vector<std::string> inputs(20, Value("0"));
			inputs[0] = Value("0.4");
			inputs[1] = Value("false");
			linked = Node("number", "Node_Number", inputs) + ",";
		}
		initial.GraphJson =
			"{\"nodes\":[" + linked +
			Node(
				"window",
				"Node_Audio_Window",
				{Value("-4"), Value("4"), std::string(locationRecord), Value("0"), Value("1"), Value("false")}
			) +
			"]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive parsed;
		REQUIRE(ReadPxcx(bytes, parsed, failure));
		return parsed;
	}
}

TEST_CASE(
	"PXC Audio Window imports source location-unit attributes and executes recorded audio",
	"[imagegraphio][audio_window]"
) {
	using namespace engine::imagegraph;
	for (int64_t unit = 0; unit < 3; unit++) {
		const std::string location = unit == 0 ? "4" : "0.4";
		const auto source = AudioWindowUnitFixture(
			"{\"r\":{\"d\":" + location + "},\"attri\":{\"unit\":" + std::to_string(unit) + "}}"
		);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes[0].Type == "pc.audio_window");
		const auto *value = FindValue(imported.Graph.Nodes[0], "location_unit");
		REQUIRE(value);
		CHECK(value->Data == engine::imagegraph::Value{EnumValue{unit}});
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		const auto stored = Write(imported.Graph);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(stored, restored, diagnostic) == Status::Ok);
		CHECK(restored == imported.Graph);
		// Host-provided recordings bind to the imported audio socket without file or device access.
		restored.Nodes.push_back(
			{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}}
		);
		restored.Links.push_back({"capture", "audio", "window", "audio_data"});
		restored.Outputs = {{"window", "window", "bit_array"}};
		Plan plan;
		REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
		const std::vector<AudioCaptureFrame> frames{{"mono", 0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 10}};
		EvaluationRequest request;
		request.AudioFrames = frames;
		EvaluatedValue output;
		REQUIRE(EvaluateValue(restored, plan, "window", request, output, diagnostic) == Status::Ok);
		const auto &array = std::get<ArrayValue>(output.Data);
		REQUIRE(array.Nested.size() == 1);
		REQUIRE(array.Nested.front().size() == 4);
		CHECK(std::get<double>(array.Nested.front().front()) == 4);
		CHECK(std::get<double>(array.Nested.front().back()) == 7);
	}
	const auto source = AudioWindowUnitFixture(R"JSON({"r":{"d":4}})JSON");
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(imported.Graph.Nodes.front().Type == "pc.audio_window");
	CHECK(FindValue(imported.Graph.Nodes.front(), "location_unit") == nullptr);
	const auto *entry = FindCatalogueEntry("pc.audio_window");
	REQUIRE(entry);
	const auto *input = FindCatalogueInput(*entry, "location_unit");
	REQUIRE(input);
	CHECK(CatalogueDefault(*input) == engine::imagegraph::Value{EnumValue{0}});
}

TEST_CASE(
	"PXC Audio Window keeps malformed location-unit records opaque and source bytes intact",
	"[imagegraphio][audio_window]"
) {
	for (const std::string_view unit :
		 {"-1", "3", "0.25", "true", "\"Second\"", "null", "18446744073709551615"}) {
		const auto source =
			AudioWindowUnitFixture("{\"r\":{\"d\":0},\"attri\":{\"unit\":" + std::string(unit) + "}}");
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Graph.Nodes.front().Type == "pxcx.opaque/Node_Audio_Window");
		REQUIRE(imported.Diagnostics.size() == 1);
		CHECK(imported.Diagnostics.front().NodeId == "window");
		CHECK(
			imported.Diagnostics.front().Message == "PXCX node stays opaque: Audio Window Location unit must "
													"be Bit, Second or Progress; source bytes retained"
		);
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
	const auto malformed = AudioWindowUnitFixture(R"JSON({"r":{"d":0},"attri":[0]})JSON");
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(malformed, imported, failure));
	CHECK(imported.Graph.Nodes.front().Type == "pxcx.opaque/Node_Audio_Window");
	REQUIRE(imported.Diagnostics.size() == 1);
	CHECK(
		imported.Diagnostics.front().Message ==
		"PXCX node stays opaque: Audio Window Location attributes must be an object; source bytes retained"
	);
	CHECK(imported.Source.OriginalBytes == malformed.OriginalBytes);
}

TEST_CASE(
	"PXC linked Audio Window location retains its source unit attribute", "[imagegraphio][audio_window]"
) {
	using namespace engine::imagegraph;
	const auto source =
		AudioWindowUnitFixture(R"JSON({"from_node":"number","from_index":0,"attri":{"unit":1}})JSON", true);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 2);
	const auto &window = imported.Graph.Nodes[1];
	CHECK(window.Type == "pc.audio_window");
	const auto *unit = FindValue(window, "location_unit");
	REQUIRE(unit);
	CHECK(unit->Data == engine::imagegraph::Value{EnumValue{1}});
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	REQUIRE(imported.Graph.Links.size() == 1);
	CHECK(imported.Graph.Links[0] == Link{"number", "number", "window", "location"});
	imported.Graph.Nodes.push_back(
		{"capture", "image.audio_recording", "", {}, {{"source_id", std::string{"mono"}}}, {}}
	);
	imported.Graph.Links.push_back({"capture", "audio", "window", "audio_data"});
	imported.Graph.Outputs = {{"window", "window", "bit_array"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
	const std::vector<AudioCaptureFrame> frames{{"mono", 0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 10}};
	EvaluationRequest request;
	request.AudioFrames = frames;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(imported.Graph, plan, "window", request, value, diagnostic) == Status::Ok);
	const auto &array = std::get<ArrayValue>(value.Data);
	REQUIRE(array.Nested.size() == 1);
	REQUIRE(array.Nested[0].size() == 4);
	CHECK(std::get<double>(array.Nested[0][0]) == 4);
	CHECK(std::get<double>(array.Nested[0][3]) == 7);
}

TEST_CASE("PXC WAV assets preserve source controls and route Window FFT", "[imagegraphio][wav_clip]") {
	using namespace engine::imagegraph;
	PxcxArchive authored;
	authored.MetadataNumber = 121092;
	authored.MetadataText = "1.22.10.201";
	authored.GraphJson =
		"{\"nodes\":[" +
		::Node(
			"file",
			"Node_WAV_File_Read",
			{::Value("\"tone.wav\""), ::Value("false"), ::Value("false")},
			R"JSON({"preview_shift":0,"preview_gain":0.5,"play":true,"file_checker":true})JSON"
		) +
		"," +
		::Node(
			"window",
			"Node_Audio_Window",
			{Wire("file"), ::Value("4"), ::Value("0"), ::Value("0"), ::Value("1"), ::Value("false")}
		) +
		"," + ::Node("fft", "Node_FFT", {Wire("window"), ::Value("0")}) + "]}" + '\0';
	std::vector<std::byte> sourceBytes;
	std::string failure;
	REQUIRE(WritePxcx(authored, sourceBytes, failure));
	PxcxArchive source;
	REQUIRE(ReadPxcx(sourceBytes, source, failure));
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Source.OriginalBytes == sourceBytes);
	REQUIRE(imported.Graph.Nodes.size() == 3);
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.wav_file_read");
	REQUIRE(
		imported.Graph.Links ==
		std::vector<Link>{{"file", "data", "window", "audio_data"}, {"window", "bit_array", "fft", "data"}}
	);
	const auto *path = FindValue(imported.Graph.Nodes.front(), "path");
	REQUIRE(path);
	REQUIRE(std::get<std::string>(path->Data) == "tone.wav");
	const std::array<uint8_t, 50> wave{'R', 'I', 'F', 'F', 42, 0, 0,   0, 'W', 'A', 'V', 'E', 'f',
									   'm', 't', ' ', 16,  0,  0, 0,   1, 0,   1,	0,	 8,	  0,
									   0,	0,	 8,	  0,   0,  0, 1,   0, 8,   0,	'd', 'a', 't',
									   'a', 5,	 0,	  0,   0,  0, 128, 0, 128, 0,	0};
	AudioBit clip;
	Diagnostic diagnostic;
	REQUIRE(
		DecodeWavClip(std::as_bytes(std::span(wave)), WavClipPolicy::PixelComposer, 4096, clip, diagnostic) ==
		Status::Ok
	);
	std::array assets{AudioClipSource{"tone.wav", std::move(clip)}};
	EvaluationRequest request;
	request.AudioClips = assets;
	imported.Graph.Outputs = {{"spectrum", "fft", "array"}};
	Plan plan;
	REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(imported.Graph, plan, "spectrum", request, output, diagnostic) == Status::Ok);
	const auto &array = std::get<ArrayValue>(output.Data);
	REQUIRE(array.Nested.empty());
	REQUIRE(array.Elements.size() == 3);
	CHECK(std::get<double>(array.Elements[0]) == 2);
	CHECK(std::get<double>(array.Elements[1]) == 0);
	CHECK(std::get<double>(array.Elements[2]) == 2);
}

namespace {
	PxcxArchive LiteralArrayFixture(std::string_view type, const std::vector<std::string> &inputs) {
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson = "{\"attributes\":{\"surface_dimension\":[64,64]},\"nodes\":[" +
							Node("filter", type, inputs) + "]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		return source;
	}
}

TEST_CASE(
	"PXCX static literal Ratio arrays preserve source records and replay through native evaluation",
	"[imagegraphio][node_processor]"
) {
	using namespace engine::imagegraph;
	const std::string gradient =
		R"JSON("{\"keys\":[{\"time\":0,\"value\":4278190080},{\"time\":1,\"value\":4294967295}],\"type\":0}")JSON";
	const std::vector<std::string> inputs{
		::Value(gradient), ::Value("3"), ::Value("0"), ::Value("1"), ::Value("[0,0.5,1]")
	};
	const PxcxArchive source = LiteralArrayFixture("Node_Gradient_Sample", inputs);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	CHECK(imported.Source.GraphJson == source.GraphJson);
	const auto node =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "filter";
		});
	REQUIRE(node != imported.Graph.Nodes.end());
	CHECK(node->Type == "pc.gradient_sample");
	const auto *ratio = FindValue(*node, "ratio");
	REQUIRE(ratio);
	const auto &array = std::get<ArrayValue>(ratio->Data);
	CHECK(array.ElementType == ValueType::Scalar);
	CHECK(array.Elements == std::vector<ElementValue>{0.0, .5, 1.0});
	imported.Graph.Outputs = {{"colors", "filter", "colors"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	Plan plan;
	const Status compiled = Compile(restored, plan, diagnostic);
	INFO(diagnostic.Message);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue first, replay;
	EvaluationRequest request;
	REQUIRE(EvaluateValue(restored, plan, "colors", request, first, diagnostic) == Status::Ok);
	request.Tick = 20;
	REQUIRE(EvaluateValue(restored, plan, "colors", request, replay, diagnostic) == Status::Ok);
	CHECK(first == replay);
	CHECK(
		std::get<ArrayValue>(first.Data).Elements ==
		std::vector<ElementValue>{Colour{0, 0, 0, 255}, Colour{128, 128, 128, 255}, Colour{0, 0, 0, 255}}
	);
	const PxcxArchive outer = LiteralArrayFixture("Node_Gradient_Out", {::Value(gradient), ::Value("[0,1]")});
	PxcxImport batched;
	REQUIRE(ImportPxcxImageGraph(outer, batched, failure));
	const auto outNode =
		std::find_if(batched.Graph.Nodes.begin(), batched.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "filter";
		});
	REQUIRE(outNode != batched.Graph.Nodes.end());
	CHECK(outNode->Type == "pc.gradient_out");
	const auto *sample = FindValue(*outNode, "sample");
	REQUIRE(sample);
	CHECK(std::get<ArrayValue>(sample->Data).Elements == std::vector<ElementValue>{0.0, 1.0});
	batched.Graph.Outputs = {{"colors", "filter", "color"}};
	REQUIRE(Compile(batched.Graph, plan, diagnostic) == Status::Ok);
	REQUIRE(EvaluateValue(batched.Graph, plan, "colors", {}, first, diagnostic) == Status::Ok);
	CHECK(
		std::get<ArrayValue>(first.Data).Elements ==
		std::vector<ElementValue>{Colour{0, 0, 0, 255}, Colour{255, 255, 255, 255}}
	);
	const PxcxArchive nested = LiteralArrayFixture(
		"Node_Gradient_Sample",
		{::Value(gradient), ::Value("3"), ::Value("0"), ::Value("1"), ::Value("[[0,0.5],[1]]")}
	);
	PxcxImport declined;
	REQUIRE(ImportPxcxImageGraph(nested, declined, failure));
	const auto opaque =
		std::find_if(declined.Graph.Nodes.begin(), declined.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "filter";
		});
	REQUIRE(opaque != declined.Graph.Nodes.end());
	CHECK(opaque->Type == "pxcx.opaque/Node_Gradient_Sample");
	CHECK(declined.Source.OriginalBytes == nested.OriginalBytes);
}

TEST_CASE(
	"PXCX source literal arrays map to catalogue nodes without an executor", "[imagegraphio][node_processor]"
) {
	const PxcxArchive source = LiteralArrayFixture(
		"Node_Ambient_Occlusion", {Value("-4"), Value("4"), Value("true"), Value("[0.1,0.2]")}
	);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	const auto node =
		std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
			return node.Id == "filter";
		});
	REQUIRE(node != imported.Graph.Nodes.end());
	CHECK(node->Type == "pc.ambient_occlusion");
	const auto *height = FindValue(*node, "height");
	REQUIRE(height);
	CHECK(
		std::get<engine::imagegraph::ArrayValue>(height->Data).Elements ==
		std::vector<engine::imagegraph::ElementValue>{.1, .2}
	);
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
}

namespace {
	PxcxArchive SourceDriverImportFixture(
		std::string_view driver,
		bool array = false,
		bool adder = false,
		std::string_view firstTime = "2",
		std::string_view lastTime = "12",
		std::string_view firstMarker = "",
		std::string_view lastMarker = ""
	) {
		// Pinned Node_Number constructor defaults, including display-only controls.
		auto inputs = NumberInputs(false);
		inputs[3] = Value("[0,1]");
		inputs[4] = Value("0.01");
		inputs[5] = Value("false");
		inputs[7] = Value("1");
		inputs[8] = Value("false");
		inputs[9] = Value("[0,0]");
		inputs[10] = Value("1");
		inputs[13] = Value("-4");
		inputs[14] = Value("[32,32]");
		inputs[15] = Value("[0,360]");
		inputs[16] = Value("5");
		inputs[17] = Value("false");
		inputs[18] = Value("\"Value\"");
		const std::string from = array ? "[0,2,99]" : "0", to = array ? "[10,12]" : "10";
		inputs[0] = "{\"anim\":true,\"r\":[[[0," + std::string(firstTime) + "]," + from +
					",[0,1],[0,0],0,0,true," + std::string(driver) + "],[[0," + std::string(lastTime) + "]," +
					to + ",[0,1],[0,0],0,0,true,0]]}";
		if (adder || !firstMarker.empty()) {
			const std::string initial = "[0," + std::string(firstTime) + "]";
			const std::string marker =
				firstMarker.empty() ? "[1," + std::string(firstTime) + ",0]" : std::string(firstMarker);
			inputs[0].replace(inputs[0].find(initial), initial.size(), marker);
		}
		if (!lastMarker.empty()) {
			const std::string initial = "[0," + std::string(lastTime) + "]";
			inputs[0].replace(inputs[0].find(initial), initial.size(), lastMarker);
		}
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson =
			"{\"animator\":{\"frames_total\":16,\"playback\":0,\"framerate\":30},\"nodes\":[" +
			Node("number", "Node_Number", inputs) + "]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		return source;
	}
}

TEST_CASE(
	"PXC named deterministic source drivers import and replay without losing source bytes",
	"[imagegraphio][timeline_v8]"
) {
	using namespace engine::imagegraph;
	const std::array<std::pair<std::string_view, double>, 6> controls{
		{{R"JSON({"typ":"linear","spd":2})JSON", 22},
		 {R"JSON({"typ":"snap","snp":3})JSON", 6},
		 {R"JSON({"typ":"bounce","amo":3,"amp":0.5,"stp":2})JSON", 6},
		 {R"JSON({"typ":"elastic","amo":3,"amp":0.5,"stp":2})JSON", 14},
		 {R"JSON({"typ":"curve","crv":[0,1,0,0,1,0,0,0,0,0,0.3333333333333333,0.3333333333333333,-0.3333333333333333,-0.3333333333333333,1,1,0,0]})JSON",
		  6},
		 {R"JSON({"typ":"sine","fre":1,"amp":2,"phs":0,"smt":0})JSON", 6}}
	};
	for (const auto &[control, expected] : controls) {
		const auto source = SourceDriverImportFixture(control);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		std::string importNotes;
		for (const auto &note : imported.Diagnostics)
			importNotes += note.Message + "\n";
		INFO(importNotes);
		const auto valueKeys = KeyframesFor(imported.Graph, "number", "value");
		REQUIRE(valueKeys.size() == 2);
		CHECK(valueKeys.front()->SourceDriver.has_value());
		CHECK_FALSE(valueKeys.front()->SineDriver.has_value());
		CHECK(HasCanonicalStaticInput(imported.Graph, "number", "integer"));
		imported.Graph.Outputs = {{"value", "number", "number"}};
		Document parsed;
		Diagnostic diagnostic;
		Plan plan;
		REQUIRE(Read(Write(imported.Graph), parsed, diagnostic) == Status::Ok);
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		EvaluatedValue value, replay;
		EvaluationRequest request;
		request.Tick = 8;
		REQUIRE(EvaluateValue(parsed, plan, "value", request, value, diagnostic) == Status::Ok);
		CHECK(std::abs(std::get<double>(value.Data) - expected) < 1e-12);
		REQUIRE(EvaluateValue(parsed, plan, "value", request, replay, diagnostic) == Status::Ok);
		CHECK(replay.Data == value.Data);
	}
}

TEST_CASE("PXC source array keys use bounded shorter-endpoint interpolation", "[imagegraphio][timeline_v8]") {
	using namespace engine::imagegraph;
	const auto source = SourceDriverImportFixture(R"JSON({"typ":"linear","spd":2})JSON", true);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	std::string importNotes;
	for (const auto &note : imported.Diagnostics)
		importNotes += note.Message + "\n";
	INFO(importNotes);
	REQUIRE(KeyframesFor(imported.Graph, "number", "value").size() == 2);
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	imported.Graph.Outputs = {{"value", "number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	EvaluationRequest request;
	request.Tick = 8;
	REQUIRE(EvaluateValue(imported.Graph, plan, "value", request, value, diagnostic) == Status::Ok);
	CHECK((std::get<ArrayValue>(value.Data) == ArrayValue{ValueType::Scalar, {22.0, 24.0}}));
}

TEST_CASE(
	"PXC unsupported or malformed source drivers stay opaque with original archive bytes",
	"[imagegraphio][timeline_v8]"
) {
	for (const auto control :
		 {R"JSON({"typ":"wiggle","sed":12,"oct":2,"fre":4,"amp":1})JSON",
		  R"JSON({"typ":"linear","spd":"2"})JSON",
		  R"JSON({"typ":"bounce","amo":1025,"amp":0.5})JSON",
		  R"JSON({"typ":"curve","crv":[0,0,0,0,1,0,0,0,0,0,1,0,-1,0,1,1,0,0]})JSON"}) {
		const auto source = SourceDriverImportFixture(control);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes.front().Type == "pxcx.opaque/Node_Number");
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		CHECK(imported.Graph.Keyframes.empty());
		REQUIRE_FALSE(imported.Diagnostics.empty());
	}
}

TEST_CASE(
	"PXC undriven source array keys preserve real animation without an invented driver",
	"[imagegraphio][timeline_v8]"
) {
	using namespace engine::imagegraph;
	const auto source = SourceDriverImportFixture("0", true);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	std::string importNotes;
	for (const auto &note : imported.Diagnostics)
		importNotes += note.Message + "\n";
	INFO(importNotes);
	const auto valueKeys = KeyframesFor(imported.Graph, "number", "value");
	REQUIRE(valueKeys.size() == 2);
	CHECK_FALSE(valueKeys.front()->SourceDriver);
	CHECK(HasCanonicalStaticInput(imported.Graph, "number", "integer"));
	imported.Graph.Outputs = {{"value", "number", "number"}};
	Document parsed;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(Write(imported.Graph), parsed, diagnostic) == Status::Ok);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 8;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(parsed, plan, "value", request, value, diagnostic) == Status::Ok);
	CHECK((std::get<ArrayValue>(value.Data) == ArrayValue{ValueType::Scalar, {6.0, 8.0}}));
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
}

TEST_CASE(
	"PXC quaternion animation imports verified Euler default and explicit raw modes",
	"[imagegraphio][timeline_v8]"
) {
	using namespace engine::imagegraph;
	for (const bool raw : {false, true}) {
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		const std::string from = raw ? "[-0.7071067811865476,0,0,0.7071067811865476]" : "[90,0,0,0]";
		const std::string to = raw ? "[-1,0,0,0]" : "[180,0,0,0]";
		const std::string input =
			"{\"anim\":true," + std::string(raw ? "\"attri\":{\"angle_display\":0}," : "") +
			"\"r\":[[[0,2]," + from + ",[0,1],[0,0],0,0,true,0],[[0,6]," + to + ",[0,1],[0,0],0,0,true,0]]}";
		initial.GraphJson =
			"{\"nodes\":[" + ::Node("angles", "Node_Quarternion_To_Euler", {input}) + "]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Tracks.size() == 1);
		CHECK(imported.Graph.Tracks.front().QuaternionMode == std::optional<int64_t>{raw ? 0 : 1});
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		imported.Graph.Outputs = {{"angles", "angles", "euler_angles"}};
		Plan plan;
		Diagnostic diagnostic;
		REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
		for (uint64_t tick : {0, 2, 4, 6, 4}) {
			CAPTURE(raw, tick);
			EvaluatedValue value;
			EvaluationRequest request;
			request.Tick = tick;
			REQUIRE(EvaluateValue(imported.Graph, plan, "angles", request, value, diagnostic) == Status::Ok);
			CHECK(
				(std::get<Vector3>(value.Data) == Vector3{
													  double(
														  tick < 4	  ? -90
														  : tick == 4 ? -135
														  : raw		  ? 180
																	  : -180
													  ),
													  0,
													  0
												  })
			);
		}
	}
}

TEST_CASE(
	"PXC static quaternion controls convert Euler once and reject invalid mode metadata",
	"[imagegraphio][timeline_v8]"
) {
	using namespace engine::imagegraph;
	for (const auto attrs :
		 {std::string_view{},
		  std::string_view{R"JSON({"angle_display":0})JSON"},
		  std::string_view{R"JSON({"angle_display":2})JSON"},
		  std::string_view{"[]"}}) {
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		const bool raw = attrs == R"JSON({"angle_display":0})JSON";
		const bool invalid = attrs == "[]" || attrs == R"JSON({"angle_display":2})JSON";
		const std::string record =
			"{\"r\":{\"d\":" +
			std::string(raw ? "[-0.7071067811865476,0,0,0.7071067811865476]" : "[90,0,0,0]") + "}" +
			(attrs.empty() ? "" : ",\"attri\":" + std::string(attrs)) + "}";
		initial.GraphJson =
			"{\"nodes\":[" + ::Node("angles", "Node_Quarternion_To_Euler", {record}) + "]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		if (invalid) {
			CHECK(imported.Graph.Nodes.front().Type == "pxcx.opaque/Node_Quarternion_To_Euler");
			CHECK(imported.Graph.Keyframes.empty());
			CHECK(imported.Graph.Tracks.empty());
			REQUIRE_FALSE(imported.Diagnostics.empty());
		} else {
			REQUIRE(imported.Graph.Tracks.size() == 1);
			CHECK(imported.Graph.Tracks.front().QuaternionMode == std::optional<int64_t>{raw ? 0 : 1});
			REQUIRE(imported.Graph.Keyframes.size() == 1);
			CHECK(imported.Graph.Keyframes.front().Kind == KeyframeKind::Normal);
			CHECK(
				imported.Graph.Keyframes.front().Data ==
				engine::imagegraph::Value{
					raw ? Quaternion{-.7071067811865476, 0, 0, .7071067811865476} : Quaternion{90, 0, 0, 0}
				}
			);
			imported.Graph.Outputs = {{"angles", "angles", "euler_angles"}};
			Diagnostic diagnostic;
			Plan plan;
			REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
			EvaluatedValue value;
			REQUIRE(
				EvaluateValue(imported.Graph, plan, "angles", EvaluationRequest{}, value, diagnostic) ==
				Status::Ok
			);
			CHECK((std::get<Vector3>(value.Data) == Vector3{-90, 0, 0}));
		}
	}
}

TEST_CASE(
	"PXC adder key markers retain metadata and ordinary source interpolation", "[imagegraphio][key_kind]"
) {
	using namespace engine::imagegraph;
	const auto source = SourceDriverImportFixture("0", false, true);
	REQUIRE(source.GraphJson.find("[[1,2,0]") != std::string::npos);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	CHECK(imported.Graph.Nodes[0].Type == "pc.number");
	const auto valueKeys = KeyframesFor(imported.Graph, "number", "value");
	REQUIRE(valueKeys.size() == 2);
	CHECK(valueKeys[0]->Kind == KeyframeKind::Adder);
	CHECK(valueKeys[1]->Kind == KeyframeKind::Normal);
	CHECK(HasCanonicalStaticInput(imported.Graph, "number", "integer"));
	CHECK(imported.Graph.FormatVersion == 9);
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcx(imported.Source, bytes, failure));
	CHECK(bytes == source.OriginalBytes);
	imported.Graph.Outputs = {{"number", "number", "number"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored.Keyframes == imported.Graph.Keyframes);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	for (const auto &[tick, expected] :
		 std::array<std::pair<uint64_t, double>, 3>{{{2, 0}, {7, 5}, {12, 10}}}) {
		EvaluationRequest request;
		request.Tick = tick;
		EvaluatedValue value;
		REQUIRE(EvaluateValue(restored, plan, "number", request, value, diagnostic) == Status::Ok);
		CHECK(std::get<double>(value.Data) == Catch::Approx(expected));
	}
}

namespace {
	PxcxArchive TimelineRangeFixture(std::string_view bounds) {
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		initial.GraphJson =
			"{\"animator\":{\"frames_total\":12,\"playback\":0,\"framerate\":30" + std::string(bounds) +
			"},\"aRegion\":[{\"l\":\"Region\",\"c\":16777215,\"fs\":4,\"fe\":5}],\"nodes\":[" +
			Node("number", "Node_Number", NumberInputs(false)) + "]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		return source;
	}
}

// project_data.serialize saves authoring bounds verbatim; source getters subtract one. The pinned
// deserialize writes range fields back into its temporary map instead of restoring the animator.
// This verifies the saved authoring projection, not executable source-load parity.
TEST_CASE(
	"PXC saved one-based frame ranges project independently to native bounds",
	"[imagegraphio][timeline_range]"
) {
	using namespace engine::imagegraph;
	struct RangeCase {
		std::string_view Bounds;
		uint64_t First, Last;
	};
	const std::array<RangeCase, 7> cases{
		{{"", 0, 11},
		 {R"JSON(,"frame_range_start":null,"frame_range_end":null)JSON", 0, 11},
		 {R"JSON(,"frame_range_start":1,"frame_range_end":12)JSON", 0, 11},
		 {R"JSON(,"frame_range_start":3,"frame_range_end":8)JSON", 2, 7},
		 {R"JSON(,"frame_range_start":3)JSON", 2, 11},
		 {R"JSON(,"frame_range_end":8)JSON", 0, 7},
		 {R"JSON(,"frame_range_end":1)JSON", 0, 0}}
	};
	for (const auto &[bounds, first, last] : cases) {
		CAPTURE(bounds);
		const auto source = TimelineRangeFixture(bounds);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		CHECK(imported.Diagnostics.empty());
		REQUIRE(imported.Graph.Timeline);
		CHECK(imported.Graph.Timeline->First == first);
		CHECK(imported.Graph.Timeline->Last == last);
		CHECK(imported.Graph.Timeline->Frames == 12);
		imported.Graph.Outputs = {{"number", "number", "number"}};
		Document parsed;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), parsed, diagnostic) == Status::Ok);
		CHECK(parsed.Timeline == imported.Graph.Timeline);
		Plan plan;
		const auto compilation = Compile(parsed, plan, diagnostic);
		CAPTURE(diagnostic.NodeId, diagnostic.Port, diagnostic.Message);
		REQUIRE(compilation == Status::Ok);
	}
}

TEST_CASE(
	"PXC invalid or playback-ambiguous saved ranges retain opaque source metadata",
	"[imagegraphio][timeline_range]"
) {
	using namespace engine::imagegraph;
	for (const std::string_view bounds :
		 {R"JSON(,"frame_range_start":0)JSON",
		  R"JSON(,"frame_range_end":-1)JSON",
		  R"JSON(,"frame_range_start":2.5)JSON",
		  R"JSON(,"frame_range_end":"8")JSON",
		  R"JSON(,"frame_range_end":13)JSON",
		  R"JSON(,"frame_range_start":13)JSON",
		  R"JSON(,"frame_range_start":8,"frame_range_end":3)JSON",
		  R"JSON(,"frame_range_start":3,"frame_range_end":3)JSON",
		  R"JSON(,"frame_range_start":12)JSON"}) {
		CAPTURE(bounds);
		const auto source = TimelineRangeFixture(bounds);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		CHECK_FALSE(imported.Graph.Timeline);
		REQUIRE(imported.Diagnostics.size() == 1);
		CHECK(imported.Diagnostics.front().Code == Status::UnsupportedExecution);
		CHECK(imported.Diagnostics.front().Message.starts_with("PXCX timeline stays opaque:"));
		CHECK(imported.Diagnostics.front().Message.ends_with("source bytes retained"));
	}
}

TEST_CASE(
	"PXC source choice fractions survive static and keyed catalogue projection",
	"[imagegraphio][source_choice]"
) {
	using namespace engine::imagegraph;
	for (const bool animated : {false, true}) {
		PxcxArchive initial;
		initial.MetadataNumber = 121092;
		initial.MetadataText = "1.22.10.201";
		const std::string cursor =
			animated
				? R"JSON({"anim":true,"r":[[[0,0],0,[0,1],[0,0],0,0,true,0],[[0,4],1,[0,1],[0,0],0,0,true,0]]})JSON"
				: ::Value("0.5");
		initial.GraphJson =
			"{\"nodes\":[" +
			::Node(
				"window",
				"Node_Audio_Window",
				{::Value("-4"), ::Value("2"), ::Value("3"), cursor, ::Value("1"), ::Value("false")}
			) +
			"]}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(initial, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		std::string notes;
		for (const auto &note : imported.Diagnostics)
			notes += note.Message + "\n";
		INFO(notes);
		REQUIRE(imported.Graph.Nodes.front().Type == "pc.audio_window");
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		const auto *literal = FindValue(imported.Graph.Nodes.front(), "cursor_location");
		if (animated) {
			const auto cursorKeys = KeyframesFor(imported.Graph, "window", "cursor_location");
			REQUIRE(cursorKeys.size() == 2);
			CHECK(cursorKeys.front()->Data == engine::imagegraph::Value{EnumValue{0}});
		} else {
			REQUIRE(literal);
			CHECK(literal->Data == engine::imagegraph::Value{.5});
		}
		imported.Graph.Outputs = {{"window", "window", "bit_array"}};
		Document parsed;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(imported.Graph), parsed, diagnostic) == Status::Ok);
		Plan plan;
		const auto compilation = Compile(parsed, plan, diagnostic);
		CAPTURE(diagnostic.NodeId, diagnostic.Port, diagnostic.Message);
		REQUIRE(compilation == Status::Ok);
	}
}

namespace {
	std::vector<engine::imagegraph::Diagnostic> PreviewIssues(const PxcxImport &result) {
		std::vector<engine::imagegraph::Diagnostic> issues;
		for (const auto &issue : result.Diagnostics)
			if (issue.Port == "previewGrid" || issue.Port == "previewRuler") issues.push_back(issue);
		return issues;
	}
	PxcxArchive PreviewArchive(std::string_view fields) {
		PxcxArchive authored;
		authored.MetadataNumber = 121092;
		authored.MetadataText = "1.22.10.201";
		auto solid = SolidInputs();
		solid[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":0}})JSON";
		authored.GraphJson = "{\"nodes\":[" + Node("solid", "Node_Solid", solid) + "," +
							 Node("sink", "Node_Project_Output", {Wire("solid")}) + "]," +
							 std::string(fields) + "}" + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(authored, bytes, failure));
		PxcxArchive checked;
		REQUIRE(ReadPxcx(bytes, checked, failure));
		return checked;
	}
}

TEST_CASE(
	"PXC saved preview grid and ordered rulers project to native format nine without rewriting source",
	"[imagegraphio][preview_settings]"
) {
	using namespace engine::imagegraph;
	const auto source = PreviewArchive(
		R"JSON("previewGrid":{"show":true,"snap":true,"size":[0,7.5],"opacity":0.5,"color":4286672237,"future":{"mode":3}},"previewRuler":[[1,-3.25],[0,8],[1,2]],"unknown":{"keep":[1,2]})JSON"
	);
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Project);
	CHECK(imported.Graph.FormatVersion == 9);
	CHECK(imported.Graph.Project->PreviewGrid.Show);
	CHECK(imported.Graph.Project->PreviewGrid.Snap);
	CHECK(imported.Graph.Project->PreviewGrid.Size == Vector2{0, 7.5});
	const std::vector<PreviewRulerGuide> expected{
		{PreviewRulerAxis::Vertical, -3.25},
		{PreviewRulerAxis::Horizontal, 8},
		{PreviewRulerAxis::Vertical, 2}
	};
	CHECK(imported.Graph.Project->PreviewRulers == expected);
	CHECK_FALSE(imported.Graph.Project->ShowPreviewRulers);
	CHECK(imported.Source.GraphJson == source.GraphJson);
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcx(imported.Source, unchanged, failure));
	CHECK(unchanged == source.OriginalBytes);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored.Project == imported.Graph.Project);
	CHECK(restored.FormatVersion == 9);
	Plan plan;
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	Image image;
	REQUIRE(Evaluate(restored, plan, "sink", {}, image, diagnostic) == Status::Ok);
	CHECK(image.Width == 1);
	CHECK(image.Height == 1);

	PxcxImport ignoredGrid;
	REQUIRE(ImportPxcxImageGraph(source, ignoredGrid, failure, {false}));
	REQUIRE(ignoredGrid.Graph.Project);
	CHECK(ignoredGrid.Graph.Project->PreviewGrid == PreviewGridSettings{});
	CHECK(ignoredGrid.Graph.Project->PreviewRulers == expected);
	CHECK_FALSE(ignoredGrid.Graph.Project->ShowPreviewRulers);
}

TEST_CASE(
	"PXC preview setting groups validate atomically and respect grid load preference",
	"[imagegraphio][preview_settings]"
) {
	using namespace engine::imagegraph;
	std::string failure;
	const auto malformedGrid = PreviewArchive(
		R"JSON("previewGrid":{"show":true,"snap":1,"size":[4,5]},"previewRuler":[[0,-2]])JSON"
	);
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(malformedGrid, imported, failure));
	REQUIRE(imported.Graph.Project);
	CHECK(imported.Graph.Project->PreviewGrid == PreviewGridSettings{});
	CHECK(
		imported.Graph.Project->PreviewRulers ==
		std::vector<PreviewRulerGuide>{{PreviewRulerAxis::Horizontal, -2}}
	);
	REQUIRE(PreviewIssues(imported).size() == 1);
	CHECK(PreviewIssues(imported)[0].Port == "previewGrid");
	CHECK(PreviewIssues(imported)[0].Code == Status::InvalidValue);
	PxcxImport skipped;
	REQUIRE(ImportPxcxImageGraph(malformedGrid, skipped, failure, {false}));
	CHECK(PreviewIssues(skipped).empty());
	CHECK(skipped.Graph.Project == imported.Graph.Project);

	for (const auto malformed :
		 {R"JSON([[0,2],[0.5,3]])JSON",
		  R"JSON([[0,2],[2,3]])JSON",
		  R"JSON([[0,2],[1,"3"]])JSON",
		  R"JSON([[0,2],[1,3,4]])JSON",
		  R"JSON({"axis":0})JSON"}) {
		const auto source =
			PreviewArchive(std::string(R"JSON("previewGrid":{"show":true},"previewRuler":)JSON") + malformed);
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(source, result, failure));
		REQUIRE(result.Graph.Project);
		CHECK(result.Graph.Project->PreviewGrid.Show);
		CHECK(result.Graph.Project->PreviewGrid.Size == Vector2{16, 16});
		CHECK(result.Graph.Project->PreviewRulers.empty());
		REQUIRE(PreviewIssues(result).size() == 1);
		CHECK(PreviewIssues(result)[0].Port == "previewRuler");
		CHECK(result.Source.OriginalBytes == source.OriginalBytes);
	}
	for (const auto malformed :
		 {R"JSON({"size":[-1,4]})JSON",
		  R"JSON({"size":[4]})JSON",
		  R"JSON({"show":"true"})JSON",
		  R"JSON([])JSON"}) {
		const auto source = PreviewArchive(
			std::string(R"JSON("previewGrid":)JSON") + malformed + R"JSON(,"previewRuler":[])JSON"
		);
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(source, result, failure));
		REQUIRE(result.Graph.Project);
		CHECK(result.Graph.Project->PreviewGrid == PreviewGridSettings{});
		REQUIRE(PreviewIssues(result).size() == 1);
		CHECK(PreviewIssues(result)[0].Port == "previewGrid");
	}
}

TEST_CASE(
	"PXC ordered ruler projection enforces the native guide bound", "[imagegraphio][preview_settings]"
) {
	using namespace engine::imagegraph;
	for (const size_t count : {Limits::MaximumArrayElements, Limits::MaximumArrayElements + 1}) {
		std::string rulers = R"JSON("previewRuler":[)JSON";
		for (size_t index = 0; index < count; index++) {
			if (index) rulers += ',';
			rulers += "[" + std::to_string(index % 2) + "," + std::to_string(index) + "]";
		}
		const auto source = PreviewArchive(rulers + ']');
		PxcxImport result;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, result, failure));
		if (count == Limits::MaximumArrayElements) {
			REQUIRE(result.Graph.Project);
			CHECK(result.Graph.Project->PreviewRulers.size() == count);
			CHECK(result.Graph.Project->PreviewRulers.back().Position == double(count - 1));
			CHECK(PreviewIssues(result).empty());
		} else {
			CHECK_FALSE(result.Graph.Project);
			REQUIRE(PreviewIssues(result).size() == 1);
			CHECK(PreviewIssues(result)[0].Code == Status::LimitExceeded);
		}
	}
}

TEST_CASE(
	"PXC archives without preview fields retain prior projection defaults", "[imagegraphio][preview_settings]"
) {
	using namespace engine::imagegraph;
	const auto source = PreviewArchive(R"JSON("unknown":{"keep":true})JSON");
	PxcxImport result;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, result, failure));
	CHECK_FALSE(result.Graph.Project);
	CHECK(result.Graph.FormatVersion == 9);
	CHECK(PreviewIssues(result).empty());
	const ProjectSettings defaults;
	CHECK(defaults.PreviewGrid == PreviewGridSettings{});
	CHECK(defaults.PreviewGrid.Size == Vector2{16, 16});
	CHECK(defaults.PreviewRulers.empty());
	CHECK_FALSE(defaults.ShowPreviewRulers);
}

TEST_CASE(
	"PXC raw signed fractional keyframes retain source time through native evaluation and replay",
	"[imagegraphio][signed_time]"
) {
	using namespace engine::imagegraph;
	// Source timelineScrub permits unclamped Control seeks and unrounded Alt seeks. valueAnimator
	// serialize stores values[i].time verbatim as [0,time], and deserialize restores that raw value.
	const auto source = SourceDriverImportFixture("0", false, false, "-1.5", "0.5");
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	CHECK(imported.Graph.Nodes[0].Type == "pc.number");
	CHECK(imported.Graph.FormatVersion == 9);
	const auto valueKeys = KeyframesFor(imported.Graph, "number", "value");
	REQUIRE(valueKeys.size() == 2);
	CHECK(GetFrameTime(*valueKeys[0]) == FrameTime{1, .5, true});
	CHECK(GetFrameTime(*valueKeys[1]) == FrameTime{0, .5, false});
	imported.Graph.Outputs = {{"number", "number", "number"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(imported.Graph), restored, diagnostic) == Status::Ok);
	CHECK(restored.Keyframes == imported.Graph.Keyframes);
	Plan plan;
	INFO(diagnostic.Message);
	REQUIRE(Compile(restored, plan, diagnostic) == Status::Ok);
	// The source forces first-key selection at time <= 0, even with negative authored keys.
	for (const auto &[frame, expected] :
		 std::array<std::pair<double, double>, 5>{{{-2, 0}, {-1.5, 0}, {-.5, 0}, {0, 0}, {.5, 10}}}) {
		FrameTime time;
		REQUIRE(SplitFrameTime(frame, time));
		EvaluationRequest request;
		REQUIRE(SetFrameTime(request, time));
		EvaluatedValue result{"sentinel", -99.0};
		INFO(diagnostic.Message);
		REQUIRE(EvaluateValue(restored, plan, "number", request, result, diagnostic) == Status::Ok);
		CHECK(std::get<double>(result.Data) == Catch::Approx(expected));
		EvaluatedValue replay;
		REQUIRE(EvaluateValue(restored, plan, "number", request, replay, diagnostic) == Status::Ok);
		CHECK(replay.Data == result.Data);
	}
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	std::vector<std::byte> unchanged;
	REQUIRE(WritePxcx(imported.Source, unchanged, failure));
	CHECK(unchanged == source.OriginalBytes);
}

TEST_CASE(
	"PXC invalid raw key time demotes the entire animated node without partial keys",
	"[imagegraphio][signed_time]"
) {
	using namespace engine::imagegraph;
	for (const auto lastTime : {"10000001", "-10000001", "\"0.5\"", "null"}) {
		const auto source = SourceDriverImportFixture("0", false, false, "-1.5", lastTime);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes[0].Type == "pxcx.opaque/Node_Number");
		CHECK(imported.Graph.Keyframes.empty());
		CHECK(imported.Graph.Tracks.empty());
		CHECK(imported.CatalogueNodes == 0);
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		REQUIRE_FALSE(imported.Diagnostics.empty());
		CHECK(imported.Diagnostics[0].Message.find("no exact native keyframe mapping") != std::string::npos);
	}
}

namespace {
	PxcxArchive AnimatedNumbersArchive(std::initializer_list<std::string_view> ids, size_t count) {
		std::string records = "[";
		for (size_t index = 0; index < count; index++) {
			if (index) records += ',';
			records += "[[0," + std::to_string(index) + "],0,[0,1],[0,0],0,0,true,0]";
		}
		records += ']';
		PxcxArchive authored;
		authored.MetadataNumber = 121092;
		authored.MetadataText = "1.22.10.201";
		authored.GraphJson = "{\"nodes\":[";
		for (const auto id : ids) {
			if (id != *ids.begin()) authored.GraphJson += ',';
			authored.GraphJson += ::Node(id, "Node_Number_Simple", {"{\"anim\":true,\"r\":" + records + "}"});
		}
		authored.GraphJson += "]}";
		authored.GraphJson += '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(WritePxcx(authored, bytes, failure));
		PxcxArchive source;
		REQUIRE(ReadPxcx(bytes, source, failure));
		return source;
	}
}

TEST_CASE(
	"PXC animated key projection checks its count before constructing native keys",
	"[imagegraphio][signed_time]"
) {
	using namespace engine::imagegraph;
	for (const size_t count : {Limits::MaximumKeyframes, Limits::MaximumKeyframes + 1}) {
		const auto source = AnimatedNumbersArchive({"number"}, count);
		std::string failure;
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		if (count == Limits::MaximumKeyframes) {
			CHECK(imported.Graph.Nodes[0].Type == "pc.number_simple");
			CHECK(imported.Graph.Keyframes.size() == count);
			CHECK(imported.Graph.Tracks.size() == 1);
		} else {
			CHECK(imported.Graph.Nodes[0].Type == "pxcx.opaque/Node_Number_Simple");
			CHECK(imported.Graph.Keyframes.empty());
			CHECK(imported.Graph.Tracks.empty());
		}
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
	}
}

TEST_CASE(
	"PXC aggregate key limit rejects individually projectable nodes without changing caller output",
	"[imagegraphio][signed_time]"
) {
	using namespace engine::imagegraph;
	const size_t perNode = Limits::MaximumKeyframes / 2 + 1;
	std::string failure;
	const auto single = AnimatedNumbersArchive({"first"}, perNode);
	PxcxImport projected;
	REQUIRE(ImportPxcxImageGraph(single, projected, failure));
	REQUIRE(projected.Graph.Nodes.size() == 1);
	REQUIRE(projected.Graph.Nodes[0].Type == "pc.number_simple");
	REQUIRE(projected.Graph.Keyframes.size() == perNode);
	const auto combined = AnimatedNumbersArchive({"first", "second"}, perNode);
	REQUIRE(combined.Nodes.size() == 2);
	PxcxImport sentinel;
	sentinel.Graph.Nodes = {{"sentinel", "value.number", "kept", {}, {{"value", 42.0}}, {}}};
	sentinel.Graph.Outputs = {{"kept", "sentinel", "value"}};
	sentinel.Source.MetadataText = "kept source";
	sentinel.Source.GraphJson = "kept graph";
	sentinel.Source.OriginalBytes = {std::byte{0x51}};
	sentinel.NativeNodes = 17;
	sentinel.CatalogueNodes = 23;
	sentinel.Diagnostics = {{Status::InvalidValue, "sentinel", "value", "kept diagnostic"}};
	const PxcxImport before = sentinel;
	CHECK_FALSE(ImportPxcxImageGraph(combined, sentinel, failure));
	CHECK(failure == "pxcx import exceeds native keyframe limit");
	CHECK(sentinel.Graph == before.Graph);
	CHECK(sentinel.Source.MetadataText == before.Source.MetadataText);
	CHECK(sentinel.Source.GraphJson == before.Source.GraphJson);
	CHECK(sentinel.Source.OriginalBytes == before.Source.OriginalBytes);
	CHECK(sentinel.NativeNodes == before.NativeNodes);
	CHECK(sentinel.CatalogueNodes == before.CatalogueNodes);
	CHECK(sentinel.Diagnostics == before.Diagnostics);
}

TEST_CASE("PXC known key kinds ignore archived time-data tails", "[imagegraphio][key_kind]") {
	using namespace engine::imagegraph;
	for (const auto &[marker, expectedKind] : std::array<std::pair<std::string_view, KeyframeKind>, 2>{
			 {{R"JSON([1,2,{"unused":"kept"},99])JSON", KeyframeKind::Adder},
			  {R"JSON([0,2,"unused"])JSON", KeyframeKind::Normal}}
		 }) {
		const auto source = SourceDriverImportFixture("0", false, false, "2", "12", marker);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		const auto valueKeys = KeyframesFor(imported.Graph, "number", "value");
		REQUIRE(valueKeys.size() == 2);
		CHECK(valueKeys[0]->Kind == expectedKind);
		CHECK(valueKeys[0]->Tick == 2);
		CHECK(HasCanonicalStaticInput(imported.Graph, "number", "integer"));
		CHECK(imported.Graph.FormatVersion == 9);
		CHECK(imported.Source.GraphJson == source.GraphJson);
		std::vector<std::byte> unchanged;
		REQUIRE(WritePxcx(imported.Source, unchanged, failure));
		CHECK(unchanged == source.OriginalBytes);
	}
}

TEST_CASE("PXC unknown key kind leaves the whole animated node opaque", "[imagegraphio][key_kind]") {
	for (const auto marker : {"[2,2,0]", "[-1,2]", "[0.5,2]", "[\"1\",2]", "[1]"}) {
		const auto source = SourceDriverImportFixture("0", false, false, "2", "12", marker);
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		REQUIRE(imported.Graph.Nodes.size() == 1);
		CHECK(imported.Graph.Nodes[0].Type == "pxcx.opaque/Node_Number");
		CHECK(imported.Graph.Keyframes.empty());
		CHECK(imported.Graph.Tracks.empty());
		CHECK(imported.CatalogueNodes == 0);
		CHECK(imported.Graph.FormatVersion == 8);
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		REQUIRE_FALSE(imported.Diagnostics.empty());
	}
}

TEST_CASE("PXC compact scalar values do not invent discarded key-kind metadata", "[imagegraphio][key_kind]") {
	using namespace engine::imagegraph;
	PxcxArchive authored;
	authored.MetadataNumber = 121092;
	authored.MetadataText = "1.22.10.201";
	authored.GraphJson =
		"{\"nodes\":[" + ::Node("number", "Node_Number_Simple", {::Value("4")}) + "]}" + '\0';
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(WritePxcx(authored, bytes, failure));
	PxcxArchive source;
	REQUIRE(ReadPxcx(bytes, source, failure));
	PxcxImport imported;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Keyframes.size() == 1);
	CHECK(imported.Graph.Keyframes.front().Kind == KeyframeKind::Normal);
	CHECK(imported.Graph.Keyframes.front().Data == engine::imagegraph::Value{4.0});
	CHECK(imported.Graph.Keyframes.front().Tick == 0);
	CHECK(imported.Graph.Tracks.size() == 1);
	CHECK(imported.Graph.FormatVersion == 9);
	CHECK(imported.Graph.Nodes.front().SourceStaticInputs == std::vector<std::string>{"value"});
	imported.Graph.Outputs = {{"number", "number", "number"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(imported.Graph, plan, diagnostic) == Status::Ok);
	EvaluatedValue result;
	REQUIRE(EvaluateValue(imported.Graph, plan, "number", {}, result, diagnostic) == Status::Ok);
	CHECK(std::get<double>(result.Data) == 4);
	CHECK(imported.Source.OriginalBytes == bytes);
}

TEST_CASE("PXC unsupported second key does not retain the preceding adder key", "[imagegraphio][key_kind]") {
	const auto source = SourceDriverImportFixture("0", false, true, "2", "12", "", "[2,12,0]");
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	REQUIRE(imported.Graph.Nodes.size() == 1);
	CHECK(imported.Graph.Nodes[0].Type == "pxcx.opaque/Node_Number");
	CHECK(imported.Graph.Keyframes.empty());
	CHECK(imported.Graph.Tracks.empty());
	CHECK(imported.CatalogueNodes == 0);
	CHECK(imported.Graph.FormatVersion == 8);
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
}
