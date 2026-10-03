#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
TEST_SUITE_ID("engine.imagegraphio.pxcx_hlsl_arguments")
TEST_DEPENDS("engine.imagegraphio.pxcxedit")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::ordered_json;
	Json Input(Json data) {
		return {{"r", {{"d", std::move(data)}}}, {"future_input", 7}};
	}
	Json Data(int mode) {
		constexpr int counts[]{0, 0, 2, 3, 4, 9, 16, 0, 0};
		if (counts[mode]) {
			Json value = Json::array();
			for (int i = 0; i < counts[mode]; i++)
				value.push_back(i ? Json(double(i) + .25) : Json(int64_t(9007199254740993ll)));
			return value;
		}
		if (mode == 7) return -4;
		if (mode == 8) return 0x44332211;
		return 1.25;
	}
	Json Shader(std::string id = "shader") {
		Json inputs = Json::array(
			{Input(""), Input("output.color=float4(1,0,0,1);"), Json::object(), Input(""), Input("")}
		);
		for (int mode = 0; mode < 9; mode++) {
			inputs.push_back(Input("value" + std::to_string(mode)));
			inputs.push_back(Input(mode));
			inputs.push_back(Input(Data(mode)));
		}
		return {
			{"id", id}, {"type", "Node_HLSL"}, {"x", 0}, {"y", 0}, {"inputs", inputs}, {"future_shader", 11}
		};
	}
	PxcxImport Import(Json graph) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = graph.dump();
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(checked, imported, failure));
		return imported;
	}
	const DynamicInput &Argument(const Node &node, int mode) {
		return node.DynamicInputs[size_t(mode) * 3 + 2];
	}
}
TEST_CASE(
	"PXC HLSL all nine types retain source numeric defaults and exact tuple widths",
	"[imagegraphio][pxcx_hlsl_arguments]"
) {
	const auto imported = Import({{"nodes", Json::array({Shader()})}});
	REQUIRE(imported.Graph.Nodes.size() == 1);
	const auto &node = imported.Graph.Nodes.front();
	std::string diagnostics;
	for (const auto &diagnostic : imported.Diagnostics)
		diagnostics += diagnostic.Message + "; ";
	INFO(diagnostics);
	REQUIRE(node.Type == "pc.hlsl");
	REQUIRE(node.DynamicInputs.size() == 27);
	const ValueType expected[]{
		ValueType::Scalar,
		ValueType::Integer,
		ValueType::Array,
		ValueType::Array,
		ValueType::Array,
		ValueType::Array,
		ValueType::Array,
		ValueType::Image,
		ValueType::Colour
	};
	for (int mode = 0; mode < 9; mode++) {
		const auto &argument = Argument(node, mode);
		CHECK(argument.Id == "argument_value_" + std::to_string(mode));
		CHECK(argument.Type == expected[mode]);
		REQUIRE(argument.Default);
		if (mode >= 2 && mode <= 6) {
			const auto &array = std::get<ArrayValue>(*argument.Default);
			CHECK(std::get<int64_t>(array.Elements.front()) == 9007199254740993ll);
			CHECK(std::get<double>(array.Elements.back()) == double(array.Elements.size() - 1) + .25);
		} else if (mode < 2)
			CHECK(std::get<double>(*argument.Default) == 1.25);
		else
			CHECK(std::get<int64_t>(*argument.Default) == (mode == 7 ? -4 : 0x44332211));
	}
}

TEST_CASE(
	"PXC HLSL typed edits preserve exact source tuples and opaque input fields",
	"[imagegraphio][pxcx_hlsl_arguments]"
) {
	const auto imported = Import({{"nodes", Json::array({Shader()})}});
	auto authored = imported.Graph;
	auto &node = authored.Nodes.front();
	REQUIRE(node.Type == "pc.hlsl");
	node.Position.X = 12;
	for (int mode = 0; mode < 9; ++mode) {
		auto &argument = node.DynamicInputs[size_t(mode) * 3 + 2];
		REQUIRE(argument.Default);
		if (mode < 2)
			argument.Default = double(mode) + .75;
		else if (mode == 7)
			argument.Default = int64_t{-4};
		else if (mode == 8)
			argument.Default = Colour{1, 127, 254, 255};
		else {
			auto array = std::get<ArrayValue>(*argument.Default);
			array.Elements.back() = double(mode) + .5;
			argument.Default = std::move(array);
		}
		for (auto &key : authored.Keyframes)
			if (key.NodeId == node.Id && key.Port == argument.Id) key.Data = *argument.Default;
	}
	const auto untouched = authored;
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool written = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	CHECK(authored == untouched);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	auto source = Json::parse(std::string_view(archive.GraphJson.data(), archive.GraphJson.size() - 1));
	CHECK(source["nodes"][0]["future_shader"] == 11);
	const auto &inputs = source["nodes"][0]["inputs"];
	REQUIRE(inputs.size() == 32);
	for (int mode = 0; mode < 9; ++mode) {
		const auto &record = inputs[7 + mode * 3];
		CHECK(record["future_input"] == 7);
		const auto &data = record["r"]["d"];
		if (mode >= 2 && mode <= 6) {
			CHECK(data[0].get<int64_t>() == 9007199254740993ll);
			CHECK(data.back().get<double>() == double(mode) + .5);
		} else if (mode < 2)
			CHECK(data.get<double>() == double(mode) + .75);
		else
			CHECK(data.get<int64_t>() == (mode == 7 ? -4 : int64_t{0xfffe7f01u}));
	}
	PxcxImport reimported;
	REQUIRE(ImportPxcxImageGraph(archive, reimported, failure));
	REQUIRE(reimported.Graph.Nodes.front().Type == "pc.hlsl");
	for (int mode = 0; mode < 9; ++mode) {
		const auto &actual = Argument(reimported.Graph.Nodes.front(), mode);
		REQUIRE(actual.Default);
		if (mode == 8)
			CHECK(std::get<int64_t>(*actual.Default) == int64_t{0xfffe7f01u});
		else
			CHECK(actual.Default == Argument(authored.Nodes.front(), mode).Default);
		for (const auto &key : reimported.Graph.Keyframes)
			if (key.NodeId == "shader" && key.Port == actual.Id) CHECK(key.Data == *actual.Default);
	}
	auto invalid = authored;
	auto &bad = invalid.Nodes.front().DynamicInputs[8];
	auto array = std::get<ArrayValue>(*bad.Default);
	array.Elements.pop_back();
	bad.Default = std::move(array);
	for (auto &key : invalid.Keyframes)
		if (key.NodeId == "shader" && key.Port == bad.Id) key.Data = *bad.Default;
	const auto previous = bytes;
	CHECK_FALSE(WritePxcxProjection(imported, invalid, {}, bytes, diagnostic));
	CHECK(bytes == previous);
}
TEST_CASE(
	"PXC HLSL packed colours preserve every byte channel value", "[imagegraphio][pxcx_hlsl_arguments]"
) {
	const auto imported = Import({{"nodes", Json::array({Shader()})}});
	for (unsigned value = 0; value < 256; ++value) {
		auto authored = imported.Graph;
		auto &input = authored.Nodes.front().DynamicInputs[26];
		const Colour colour{uint8_t(value), uint8_t(value + 37), uint8_t(value * 17), uint8_t(255 - value)};
		input.Default = colour;
		for (auto &key : authored.Keyframes)
			if (key.NodeId == "shader" && key.Port == input.Id) key.Data = colour;
		std::vector<std::byte> bytes;
		Diagnostic diagnostic;
		const bool written = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		CAPTURE(value);
		REQUIRE(written);
		engine::bake::PxcxArchive source;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, source, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(source, result, failure));
		const auto packed = int64_t(
			uint32_t(colour.Red) | uint32_t(colour.Green) << 8 | uint32_t(colour.Blue) << 16 |
			uint32_t(colour.Alpha) << 24
		);
		REQUIRE(Argument(result.Graph.Nodes.front(), 8).Default);
		CHECK(std::get<int64_t>(*Argument(result.Graph.Nodes.front(), 8).Default) == packed);
		for (const auto &key : result.Graph.Keyframes)
			if (key.NodeId == "shader" && key.Port == input.Id) CHECK(std::get<int64_t>(key.Data) == packed);
	}
}
TEST_CASE(
	"PXC HLSL instances retain local raw defaults and explicit source overrides",
	"[imagegraphio][pxcx_hlsl_arguments]"
) {
	auto base = Shader("base"), instance = Shader("instance");
	instance["instanceBase"] = "base";
	instance["inputs"][7]["r"]["d"] = 3.25;
	instance["inputs"][7]["attri"] = {{"override_instance", false}};
	instance["inputs"][10]["r"]["d"] = 2.75;
	instance["inputs"][10]["attri"] = {{"override_instance", true}, {"future_attribute", 17}};
	const auto imported = Import({{"nodes", Json::array({base, instance})}});
	REQUIRE(imported.Graph.Nodes.size() == 2);
	const auto &node = imported.Graph.Nodes[1];
	REQUIRE(node.Type == "pc.hlsl");
	CHECK(node.InstanceBase == "base");
	CHECK(node.InstanceOverrides == std::vector<std::string>{"argument_value_1"});
	CHECK(std::get<double>(*Argument(node, 0).Default) == 3.25);
	CHECK(std::get<double>(*Argument(node, 1).Default) == 2.75);
	auto desired = imported.Graph;
	auto &edited = desired.Nodes[1];
	edited.Position.Y = 13;
	edited.DynamicInputs[5].Default = 5.5;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "instance" && key.Port == "argument_value_1") key.Data = 5.5;
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	const auto json = Json::parse(archive.GraphJson.c_str());
	CHECK(json["nodes"][1]["instanceBase"] == "base");
	CHECK(json["nodes"][1]["inputs"][10]["attri"]["override_instance"] == true);
	CHECK(json["nodes"][1]["inputs"][10]["attri"]["future_attribute"] == 17);
	CHECK(json["nodes"][1]["inputs"][10]["r"]["d"] == 5.5);
	PxcxImport saved;
	REQUIRE(ImportPxcxImageGraph(archive, saved, failure));
	CHECK(saved.Graph.Nodes[1].InstanceBase == "base");
	CHECK(saved.Graph.Nodes[1].InstanceOverrides == node.InstanceOverrides);
	for (int mode = 0; mode < 9; ++mode)
		CHECK(Argument(saved.Graph.Nodes[1], mode).Type == Argument(node, mode).Type);
}
TEST_CASE(
	"PXC HLSL native vectors serialize as source numeric tuples", "[imagegraphio][pxcx_hlsl_arguments]"
) {
	const auto imported = Import({{"nodes", Json::array({Shader()})}});
	auto authored = imported.Graph;
	const Value vectors[]{Vector2{1.25, -2.5}, Vector3{3.25, -4.5, 5.75}, Vector4{6.25, -7.5, 8.75, 9.5}};
	for (int mode = 2; mode <= 4; ++mode) {
		auto &input = authored.Nodes.front().DynamicInputs[size_t(mode) * 3 + 2];
		input.Default = vectors[mode - 2];
		for (auto &key : authored.Keyframes)
			if (key.NodeId == "shader" && key.Port == input.Id) key.Data = *input.Default;
	}
	const auto untouched = authored;
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool written = WritePxcxProjection(imported, authored, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	CHECK(authored == untouched);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	const auto json = Json::parse(archive.GraphJson.c_str());
	CHECK(json["nodes"][0]["inputs"][13]["r"]["d"] == Json::array({1.25, -2.5}));
	CHECK(json["nodes"][0]["inputs"][16]["r"]["d"] == Json::array({3.25, -4.5, 5.75}));
	CHECK(json["nodes"][0]["inputs"][19]["r"]["d"] == Json::array({6.25, -7.5, 8.75, 9.5}));
	PxcxImport saved;
	REQUIRE(ImportPxcxImageGraph(archive, saved, failure));
	for (int mode = 2; mode <= 4; ++mode) {
		const auto &input = Argument(saved.Graph.Nodes.front(), mode);
		REQUIRE(input.Default);
		const auto &array = std::get<ArrayValue>(*input.Default);
		CHECK(array.Elements.size() == size_t(mode));
		for (const auto &key : saved.Graph.Keyframes)
			if (key.NodeId == "shader" && key.Port == input.Id) CHECK(key.Data == *input.Default);
	}
}
TEST_CASE(
	"PXC HLSL ordered processor uniform rows roundtrip with exact numeric leaves",
	"[imagegraphio][pxcx_hlsl_arguments]"
) {
	auto source = Shader();
	for (int mode = 2; mode <= 6; ++mode) {
		auto row = Data(mode), second = row;
		second[0] = -7.25;
		source["inputs"][7 + mode * 3]["r"]["d"] = Json::array({row, second});
	}
	const auto imported = Import({{"nodes", Json::array({source})}});
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.hlsl");
	auto desired = imported.Graph;
	desired.Nodes.front().Position.Y = 17;
	for (int mode = 2; mode <= 6; ++mode) {
		auto &input = desired.Nodes.front().DynamicInputs[size_t(mode) * 3 + 2];
		REQUIRE(input.Default);
		auto &array = std::get<ArrayValue>(*input.Default);
		REQUIRE(array.Nested.size() == 2);
		CHECK(array.Elements.empty());
		CHECK(std::get<int64_t>(array.Nested.front().front()) == 9007199254740993ll);
		array.Nested.back().back() = double(mode) + .75;
		for (auto &key : desired.Keyframes)
			if (key.NodeId == "shader" && key.Port == input.Id) key.Data = *input.Default;
	}
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	const bool written = WritePxcxProjection(imported, desired, {}, bytes, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(written);
	engine::bake::PxcxArchive archive;
	std::string failure;
	REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
	PxcxImport saved;
	REQUIRE(ImportPxcxImageGraph(archive, saved, failure));
	for (int mode = 2; mode <= 6; ++mode)
		CHECK(
			Argument(saved.Graph.Nodes.front(), mode).Default == Argument(desired.Nodes.front(), mode).Default
		);
	auto invalid = desired;
	auto &bad = invalid.Nodes.front().DynamicInputs[8];
	std::get<ArrayValue>(*bad.Default).Nested.front().pop_back();
	const auto old = bytes;
	CHECK_FALSE(WritePxcxProjection(imported, invalid, {}, bytes, diagnostic));
	CHECK(bytes == old);
}
TEST_CASE(
	"PXC HLSL malformed selector shapes stay opaque and invalid edits preserve prior bytes",
	"[imagegraphio][pxcx_hlsl_arguments]"
) {
	for (const auto &bad : {Json(9), Json(-1), Json(2.5)}) {
		auto source = Shader();
		source["inputs"][6]["r"]["d"] = bad;
		const auto imported = Import({{"nodes", Json::array({source})}});
		CHECK(imported.Graph.Nodes.front().Type.starts_with("pxcx.opaque/"));
		CHECK_FALSE(imported.Diagnostics.empty());
	}
	const auto imported = Import({{"nodes", Json::array({Shader()})}});
	auto changed = imported.Graph;
	changed.Nodes.front().Position.X = 9;
	std::vector<std::byte> bytes;
	Diagnostic diagnostic;
	REQUIRE(WritePxcxProjection(imported, changed, {}, bytes, diagnostic));
	const auto previous = bytes;
	auto invalid = changed;
	auto &input = invalid.Nodes.front().DynamicInputs[8];
	ArrayValue wrong{ValueType::Scalar, {}};
	wrong.Nested = {{{1.0}, {2.0}}, {{3.0}}};
	input.Default = wrong;
	for (auto &key : invalid.Keyframes)
		if (key.NodeId == "shader" && key.Port == input.Id) key.Data = wrong;
	CHECK_FALSE(WritePxcxProjection(imported, invalid, {}, bytes, diagnostic));
	CHECK(bytes == previous);
}
