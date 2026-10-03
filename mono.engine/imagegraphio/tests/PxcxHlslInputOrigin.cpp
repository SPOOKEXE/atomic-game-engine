#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
TEST_SUITE_ID("engine.imagegraphio.pxcx_hlsl_input_origin")
TEST_DEPENDS("engine.imagegraphio.pxcxstructureedit")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	using Json = nlohmann::ordered_json;
	Json Record(Json data, int marker) {
		return {{"r", {{"d", std::move(data)}}}, {"future", marker}};
	}
	Json Shader(bool empty = false) {
		Json inputs = Json::array(
			{Record("", 0),
			 Record("output.color=float4(1,0,0,1);", 1),
			 Json::object(),
			 Record("", 3),
			 Record("", 4)}
		);
		for (int group = 0; group < 3; ++group) {
			inputs.push_back(Record("repeated", 10 + group * 3));
			inputs.push_back(Record(0, 11 + group * 3));
			auto value = Record(1.25, 12 + group * 3);
			value["attri"] = {{"future_attribute", group}};
			if (empty) value["r"] = Json::array();
			inputs.push_back(std::move(value));
		}
		return {
			{"id", "shader"},
			{"type", "Node_HLSL"},
			{"x", 0},
			{"y", 0},
			{"inputs", inputs},
			{"future_node", 123}
		};
	}
	PxcxImport Import(Json root) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = root.dump();
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		const auto decoded = ImportPxcxImageGraph(checked, result, failure);
		INFO(failure);
		REQUIRE(decoded);
		return result;
	}
	std::string Remap(std::string port, size_t removed) {
		constexpr std::string_view names[]{"argument_name_", "argument_type_", "argument_value_"};
		for (auto prefix : names)
			if (port.starts_with(prefix)) {
				const auto index = std::stoull(port.substr(prefix.size()));
				if (index == removed) return {};
				if (index > removed)
					return std::string(prefix) + std::to_string(index - 1) +
						   (port.ends_with(".bypass") ? ".bypass" : "");
			}
		return port;
	}
	Document Remove(const Document &document, size_t group) {
		auto result = document;
		auto &node = result.Nodes.front();
		std::erase_if(node.DynamicInputs, [&](auto &input) {
			input.Id = Remap(input.Id, group);
			return input.Id.empty();
		});
		std::erase_if(node.Values, [&](auto &value) {
			value.Port = Remap(value.Port, group);
			return value.Port.empty();
		});
		for (auto *list : {&node.SourceStaticInputs, &node.SourceAnimatedInputs})
			std::erase_if(*list, [&](auto &port) {
				port = Remap(port, group);
				return port.empty();
			});
		std::erase_if(node.SourceInputExpressions, [&](auto &value) {
			value.Port = Remap(value.Port, group);
			return value.Port.empty();
		});
		std::erase_if(node.InstanceOverrides, [&](auto &port) {
			port = Remap(port, group);
			return port.empty();
		});
		std::erase_if(result.Keyframes, [&](auto &key) {
			if (key.NodeId != node.Id) return false;
			key.Port = Remap(key.Port, group);
			return key.Port.empty();
		});
		std::erase_if(result.Tracks, [&](auto &track) {
			if (track.NodeId != node.Id) return false;
			track.Port = Remap(track.Port, group);
			return track.Port.empty();
		});
		std::erase_if(result.Links, [&](auto &link) {
			if (link.ToNode == node.Id) link.ToPort = Remap(link.ToPort, group);
			if (link.FromNode == node.Id) link.FromPort = Remap(link.FromPort, group);
			return link.ToPort.empty() || link.FromPort.empty();
		});
		return result;
	}
	PxcxImport Save(const PxcxImport &imported, const Document &document, std::vector<std::byte> &bytes) {
		Diagnostic diagnostic;
		const bool written = WritePxcxProjection(imported, document, {}, bytes, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(written);
		engine::bake::PxcxArchive checked;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		PxcxImport result;
		REQUIRE(ImportPxcxImageGraph(checked, result, failure));
		return result;
	}
}
TEST_CASE(
	"HLSL middle removal moves complete source records and successive adoption rebases identity",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	for (bool empty : {false, true}) {
		CAPTURE(empty);
		const auto shader = Shader(empty);
		const auto original = Import({{"nodes", Json::array({shader})}});
		REQUIRE(original.Graph.Nodes.front().Type == "pc.hlsl");
		REQUIRE(original.Graph.Nodes.front().DynamicInputs.size() == 9);
		CHECK(original.Graph.FormatVersion == 9);
		const auto desired = Remove(original.Graph, 1);
		const auto unchanged = desired;
		CHECK(desired.Nodes.front().DynamicInputs[3].SourceInputId == "pxc:input:11");
		std::vector<std::byte> bytes;
		const auto saved = Save(original, desired, bytes);
		CHECK(desired == unchanged);
		const auto source = Json::parse(saved.Source.GraphJson.c_str());
		REQUIRE(source["nodes"][0]["inputs"].size() == 11);
		for (size_t i = 0; i < 8; ++i)
			CHECK(source["nodes"][0]["inputs"][i] == shader["inputs"][i]);
		for (size_t i = 0; i < 3; ++i)
			CHECK(source["nodes"][0]["inputs"][8 + i] == shader["inputs"][11 + i]);
		CHECK(saved.Graph.Nodes.front().DynamicInputs[3].SourceInputId == "pxc:input:8");
		Document native;
		Diagnostic diagnostic;
		const auto encoded = Write(saved.Graph);
		REQUIRE_FALSE(encoded.empty());
		REQUIRE(Read(encoded, native, diagnostic) == Status::Ok);
		CHECK(native == saved.Graph);
		const auto final = Save(saved, Remove(native, 0), bytes);
		const auto finalSource = Json::parse(final.Source.GraphJson.c_str());
		REQUIRE(finalSource["nodes"][0]["inputs"].size() == 8);
		for (size_t i = 0; i < 3; ++i)
			CHECK(finalSource["nodes"][0]["inputs"][5 + i] == shader["inputs"][11 + i]);
		CHECK(final.Graph.Nodes.front().DynamicInputs[0].SourceInputId == "pxc:input:5");
		auto forged = desired;
		forged.Nodes.front().DynamicInputs[3].SourceInputId = "pxc:input:5";
		const auto previous = bytes;
		CHECK_FALSE(WritePxcxProjection(original, forged, {}, bytes, diagnostic));
		CHECK(bytes == previous);
		forged = desired;
		forged.Nodes.front().DynamicInputs[3].SourceInputId = "pxc:input:999";
		CHECK_FALSE(WritePxcxProjection(original, forged, {}, bytes, diagnostic));
		CHECK(bytes == previous);
		forged = desired;
		forged.Nodes.front().DynamicInputs.pop_back();
		CHECK_FALSE(WritePxcxProjection(original, forged, {}, bytes, diagnostic));
		CHECK(bytes == previous);
	}
}
TEST_CASE(
	"HLSL record compaction retains signed keys disabled source programs and sampler routes",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	auto shader = Shader();
	auto &inputs = shader["inputs"];
	const auto key = [](double time, double value, int tail) {
		return Json::array(
			{Json::array({0, time}),
			 value,
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 4294967295u,
			 tail}
		);
	};
	inputs[7]["anim"] = true;
	inputs[7]["r"] = Json::array({key(-2.5, 1.25, 201), key(4.75, 8.5, 202)});
	inputs[7]["global_key"] = "value + 3";
	inputs[7]["global_use"] = false;
	inputs[12]["r"]["d"] = 7;
	inputs[13]["r"]["d"] = -4;
	inputs[13]["from_node"] = "image";
	inputs[13]["from_index"] = 0;
	auto image = Shader();
	image["id"] = "image";
	image["inputs"].erase(image["inputs"].begin() + 5, image["inputs"].end());
	const auto imported = Import({{"nodes", Json::array({shader, image})}});
	REQUIRE(imported.Graph.Nodes.front().Type == "pc.hlsl");
	REQUIRE(imported.Graph.Links.size() == 1);
	REQUIRE(imported.Graph.Nodes.front().SourceInputExpressions.size() == 1);
	auto desired = Remove(imported.Graph, 1);
	CHECK(desired.Links.front().ToPort == "argument_value_1");
	std::vector<std::byte> bytes;
	const auto saved = Save(imported, desired, bytes);
	const auto json = Json::parse(saved.Source.GraphJson.c_str());
	CHECK(json["nodes"][0]["inputs"][7] == shader["inputs"][7]);
	CHECK(json["nodes"][0]["inputs"][10] == shader["inputs"][13]);
	REQUIRE(saved.Graph.Links.size() == 1);
	CHECK(saved.Graph.Links.front().ToPort == "argument_value_1");
	CHECK(saved.Graph.Links.front().FromNode == "image");
	REQUIRE(saved.Graph.Nodes.front().SourceInputExpressions.size() == 1);
	CHECK(saved.Graph.Nodes.front().SourceInputExpressions.front().Code == "value + 3");
	CHECK_FALSE(saved.Graph.Nodes.front().SourceInputExpressions.front().Enabled);
	size_t signedKeys = 0;
	for (const auto &original : imported.Graph.Keyframes)
		if (original.NodeId == "shader" && original.Port == "argument_value_0") {
			const auto found = std::find_if(
				saved.Graph.Keyframes.begin(), saved.Graph.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == original.NodeId && key.Port == original.Port &&
						   GetFrameTime(key) == GetFrameTime(original);
				}
			);
			REQUIRE(found != saved.Graph.Keyframes.end());
			CHECK(*found == original);
			++signedKeys;
		}
	CHECK(signedKeys == 2);
	auto limited = imported;
	limited.Options.MaximumOperationBytes = 1;
	const auto prior = bytes;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(limited, desired, {}, bytes, diagnostic));
	CHECK(bytes == prior);
	auto cloned = desired;
	cloned.Nodes.push_back(cloned.Nodes.front());
	cloned.Nodes.back().Id = "clone";
	CHECK_FALSE(WritePxcxProjection(imported, cloned, {}, bytes, diagnostic));
	CHECK(bytes == prior);
}
TEST_CASE(
	"HLSL fresh source triples acquire independent identity with preadmitted encoding",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	const auto imported = Import({{"nodes", Json::array({Shader()})}});
	auto desired = imported.Graph;
	auto &inputs = desired.Nodes.front().DynamicInputs;
	inputs.push_back({"argument_name_3", ValueType::Text, std::string("new_value")});
	inputs.push_back({"argument_type_3", ValueType::Enum, EnumValue{0}});
	inputs.push_back({"argument_value_3", ValueType::Scalar, 1.5});
	const auto caller = desired;
	std::vector<std::byte> bytes;
	const auto saved = Save(imported, desired, bytes);
	CHECK(desired == caller);
	REQUIRE(saved.Graph.Nodes.front().DynamicInputs.size() == 12);
	CHECK(saved.Graph.Nodes.front().DynamicInputs[9].SourceInputId == "pxc:input:14");
	CHECK(saved.Graph.Nodes.front().DynamicInputs[11].SourceInputId == "pxc:input:16");
	const auto json = Json::parse(saved.Source.GraphJson.c_str());
	CHECK(json["nodes"][0]["inputs"][14]["r"]["d"] == "new_value");
	CHECK(json["nodes"][0]["inputs"][16]["r"]["d"] == 1.5);
	auto limited = imported;
	limited.Options.MaximumOperationBytes = 64 * 1024;
	desired.Nodes.front().DynamicInputs[9].Default = std::string(32000, 'a');
	const auto previous = bytes;
	Diagnostic diagnostic;
	CHECK_FALSE(WritePxcxProjection(limited, desired, {}, bytes, diagnostic));
	CHECK(
		diagnostic.Message == "native animator construction exceeds operation bounds: static compact "
							  "animator storage exceeds operation bounds"
	);
	CHECK(bytes == previous);
}
TEST_CASE(
	"HLSL record compaction moves surviving bypass routes and retires deleted routes",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	const auto shader = Shader();
	Json number{
		{"id", "consumer"},
		{"type", "Node_Number_Simple"},
		{"x", 0},
		{"y", 0},
		{"inputs", Json::array({Json{{"from_node", "shader"}, {"from_index", 1013}, {"future", 51}}})}
	};
	const auto imported = Import({{"nodes", Json::array({shader, number})}});
	REQUIRE(imported.Graph.Links.size() == 1);
	CHECK(imported.Graph.Links.front().FromPort == "argument_value_2.bypass");
	auto desired = Remove(imported.Graph, 1);
	CHECK(desired.Links.front().FromPort == "argument_value_1.bypass");
	std::vector<std::byte> bytes;
	const auto saved = Save(imported, desired, bytes);
	auto json = Json::parse(saved.Source.GraphJson.c_str());
	CHECK(json["nodes"][1]["inputs"][0]["from_index"] == 1010);
	CHECK(saved.Graph.Links == desired.Links);
	auto removed = Remove(saved.Graph, 1);
	REQUIRE(removed.Links.empty());
	const auto retired = Save(saved, removed, bytes);
	json = Json::parse(retired.Source.GraphJson.c_str());
	CHECK_FALSE(json["nodes"][1]["inputs"][0].contains("from_node"));
	CHECK_FALSE(json["nodes"][1]["inputs"][0].contains("from_index"));
	CHECK(json["nodes"][1]["inputs"][0]["future"] == 51);
	CHECK(retired.Graph.Links.empty());
	auto retargeted = removed;
	retargeted.Links = saved.Graph.Links;
	retargeted.Links.front().FromPort = "argument_value_0.bypass";
	const auto redirected = Save(saved, retargeted, bytes);
	json = Json::parse(redirected.Source.GraphJson.c_str());
	CHECK(json["nodes"][1]["inputs"][0]["from_index"] == 1007);
	CHECK(redirected.Graph.Links == retargeted.Links);
	auto dangling = removed;
	dangling.Links = saved.Graph.Links;
	Diagnostic diagnostic;
	const auto previous = bytes;
	CHECK_FALSE(WritePxcxProjection(saved, dangling, {}, bytes, diagnostic));
	CHECK(bytes == previous);
}

TEST_CASE(
	"HLSL compact defaults synchronize inactive source keys without changing caller state",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	const auto shader = Shader();
	const auto imported = Import({{"nodes", Json::array({shader})}});
	auto desired = imported.Graph;
	desired.Nodes.front().DynamicInputs[2].Default = 4.75;
	const auto caller = desired;
	std::vector<std::byte> bytes;
	const auto saved = Save(imported, desired, bytes);
	CHECK(desired == caller);
	CHECK(saved.Graph.Nodes.front().DynamicInputs[2].Default == std::optional<Value>{4.75});
	const auto key =
		std::find_if(saved.Graph.Keyframes.begin(), saved.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "shader" && key.Port == "argument_value_0";
		});
	REQUIRE(key != saved.Graph.Keyframes.end());
	CHECK(key->Data == Value{4.75});
	CHECK(key->SourceKeyId == "pxc:compact");
	const auto json = Json::parse(saved.Source.GraphJson.c_str());
	auto expected = shader;
	expected["inputs"][7]["r"]["d"] = 4.75;
	CHECK(json["nodes"][0] == expected);
	CHECK(
		saved.Graph.Nodes.front().DynamicInputs[2].SourceInputId ==
		imported.Graph.Nodes.front().DynamicInputs[2].SourceInputId
	);
	auto conflicting = desired;
	for (auto &key : conflicting.Keyframes)
		if (key.NodeId == "shader" && key.Port == "argument_value_0") key.Data = 7.5;
	Diagnostic diagnostic;
	const auto previous = bytes;
	CHECK_FALSE(WritePxcxProjection(imported, conflicting, {}, bytes, diagnostic));
	CHECK(diagnostic.Port == "argument_value_0");
	CHECK(diagnostic.Message == "PXC compact animator conflicts with authored literal");
	CHECK(bytes == previous);
}

TEST_CASE(
	"HLSL fixed replay projections save and reload complete source records",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	auto shader = Shader();
	for (size_t group = 0; group < 3; ++group)
		shader["inputs"][5 + group * 3]["r"]["d"] = "gain" + std::to_string(group);
	const auto imported = Import({{"nodes", Json::array({shader})}});
	// A native fixed control need not retain its inactive compact source key.
	auto authored = imported.Graph;
	std::erase_if(authored.Keyframes, [](const auto &key) {
		return key.NodeId == "shader" && key.Port == "argument_value_0";
	});
	GroupReplayState empty, local, bound, edited;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(authored, empty, 1, local, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(authored, {}, local, 1, bound, diagnostic) == Status::Ok);
	const Value value = 6.25;
	GroupRefreshEvent event;
	event.NodeId = "shader";
	event.Reason = GroupRefreshReason::Edit;
	event.EditedPort = "argument_value_0";
	event.LocalValue = &value;
	event.LocalAnimated = false;
	REQUIRE(ReplayGroupAnimatorEdits(authored, {&event, 1}, bound, 1, edited, diagnostic) == Status::Ok);
	Document projected;
	REQUIRE(ProjectGroupReplay(authored, edited, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Nodes.front().DynamicInputs[2].Default == std::optional<Value>{value});
	CHECK(std::none_of(projected.Keyframes.begin(), projected.Keyframes.end(), [](const auto &key) {
		return key.NodeId == "shader" && key.Port == "argument_value_0";
	}));
	const auto caller = projected;
	auto preview = projected;
	preview.Outputs = {{"preview", "shader", "surface"}};
	Plan plan;
	const auto status = Compile(preview, plan, diagnostic);
	INFO(diagnostic.Port << ": " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	std::vector<std::byte> bytes;
	const auto saved = Save(imported, projected, bytes);
	CHECK(projected == caller);
	CHECK(saved.Graph.Nodes.front().DynamicInputs[2].Default == std::optional<Value>{value});
	const auto key =
		std::find_if(saved.Graph.Keyframes.begin(), saved.Graph.Keyframes.end(), [](const auto &key) {
			return key.NodeId == "shader" && key.Port == "argument_value_0";
		});
	REQUIRE(key != saved.Graph.Keyframes.end());
	CHECK(key->Data == value);
	CHECK(key->SourceKeyId == "pxc:compact");
	const auto json = Json::parse(saved.Source.GraphJson.c_str());
	auto expected = shader;
	expected["inputs"][7]["r"]["d"] = 6.25;
	CHECK(json["nodes"][0] == expected);
	const auto unchanged = bytes;
	REQUIRE(WritePxcxProjection(saved, saved.Graph, {}, bytes, diagnostic));
	CHECK(bytes == unchanged);
}

TEST_CASE(
	"Many static HLSL literal edits admit compact synchronization work before scanning",
	"[imagegraphio][pxcx_hlsl_input_origin]"
) {
	Json nodes = Json::array();
	for (size_t node = 0; node < 8; ++node) {
		auto shader = Shader();
		shader["id"] = "shader" + std::to_string(node);
		auto &inputs = shader["inputs"];
		inputs.erase(inputs.begin() + 5, inputs.end());
		for (size_t group = 0; group < 21; ++group) {
			inputs.push_back(Record("gain" + std::to_string(group), 10 + int(group * 3)));
			inputs.push_back(Record(0, 11 + int(group * 3)));
			inputs.push_back(Record(1.25, 12 + int(group * 3)));
		}
		nodes.push_back(std::move(shader));
	}
	// A removed source node still contributes to the original key index traversed by literal writes.
	auto removed = Shader();
	removed["id"] = "removed";
	removed["inputs"].erase(removed["inputs"].begin() + 5, removed["inputs"].end());
	auto keys = Json::array();
	for (size_t tick = 0; tick < 20000; ++tick)
		keys.push_back(
			Json::array(
				{Json::array({0, tick}),
				 "",
				 Json::array({0, 1}),
				 Json::array({0, 0}),
				 0,
				 0,
				 true,
				 0,
				 4294967295u}
			)
		);
	removed["inputs"][4]["anim"] = true;
	removed["inputs"][4]["r"] = std::move(keys);
	nodes.push_back(std::move(removed));
	const auto imported = Import({{"nodes", nodes}});
	REQUIRE(imported.Graph.Nodes.size() == 9);
	REQUIRE(imported.Graph.Keyframes.size() == 8 * 67 + 20003);
	auto desired = imported.Graph;
	desired.Nodes.pop_back();
	desired.Keyframes.clear();
	std::erase_if(desired.Tracks, [](const auto &track) { return track.NodeId == "removed"; });
	for (auto &node : desired.Nodes)
		for (auto &input : node.DynamicInputs) {
			if (input.Id.starts_with("argument_name_"))
				input.Default = std::get<std::string>(*input.Default) + "_edited";
			if (input.Id.starts_with("argument_value_")) input.Default = 2.75;
		}
	auto preview = desired;
	preview.Outputs = {{"preview", "shader0", "surface"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(preview, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	const auto caller = desired;
	std::vector<std::byte> bytes{std::byte{0x29}, std::byte{0x71}};
	const auto previous = bytes;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, diagnostic));
	CHECK(diagnostic.Message == "PXC compact animator synchronization exceeds transaction work limit");
	CHECK(bytes == previous);
	CHECK(desired == caller);
}
