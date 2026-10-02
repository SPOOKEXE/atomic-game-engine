#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
TEST_SUITE_ID("engine.imagegraphio.pxcx_group_values")
TEST_DEPENDS("engine.imagegraphio.pxcximport")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::json;
	PxcxImport Source(Json value, int type = 11, int subtype = 0, bool compactFuture = true) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		std::vector<Json> controls(16, Json::object());
		controls[0] = {{"r", {{"d", subtype}}}};
		controls[1] = {{"r", {{"d", {0, 10}}}}};
		controls[2] = {{"r", {{"d", type}}}};
		Json graph = {
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"nodes",
			 Json::array(
				 {{{"id", "group"},
				   {"type", "Node_Group"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs",
					Json::array({Json{
						{"r", {{"d", std::move(value)}, {"future_compact", {{"keep", 29}}}}},
						{"future", {{"keep", 17}}}
					}})},
				   {"attri",
					{{"custom_input_list", {"input"}},
					 {"custom_output_list", {"output"}},
					 {"color_depth", 1},
					 {"interpolate", 0},
					 {"oversample", 0}}}},
				  {{"id", "input"},
				   {"type", "Node_Group_Input"},
				   {"group", "group"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs", controls}},
				  {{"id", "output"},
				   {"type", "Node_Group_Output"},
				   {"group", "group"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs",
					Json::array({Json{{"from_node", "input"}, {"from_index", 0}, {"from_tag", 0}}})}},
				  {{"id", "sink"},
				   {"type", "Node_Project_Output"},
				   {"x", 0},
				   {"y", 0},
				   {"inputs",
					Json::array({Json{{"from_node", "group"}, {"from_index", 0}, {"from_tag", 0}}})}}}
			 )}
		};
		if (!compactFuture) graph["nodes"][0]["inputs"][0]["r"].erase("future_compact");
		archive.GraphJson = graph.dump();
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		const auto accepted = ImportPxcxImageGraph(archive, imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	PxcxImport Reload(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport result;
		const auto accepted = ImportPxcxImageGraph(archive, result, failure);
		INFO(failure);
		REQUIRE(accepted);
		return result;
	}
	void ClearSourceIds(Document &document) {
		for (auto &key : document.Keyframes)
			key.SourceKeyId.clear();
		std::sort(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &a, const auto &b) {
			return std::tie(a.NodeId, a.Port, a.NegativeFrame, a.Tick, a.Subframe) <
				   std::tie(b.NodeId, b.Port, b.NegativeFrame, b.Tick, b.Subframe);
		});
	}
	Node &Input(Document &document) {
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const auto &n) {
			return n.Id == "input";
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	Value &Parent(Document &document) {
		auto &node = Input(document);
		auto value = std::find_if(node.Values.begin(), node.Values.end(), [](const auto &v) {
			return v.Port == "parent_value";
		});
		REQUIRE(value != node.Values.end());
		return value->Data;
	}
	void SetParent(Document &document, Value value) {
		Parent(document) = value;
		for (auto &key : document.Keyframes)
			if (key.NodeId == "input" && key.Port == "parent_value") key.Data = value;
		for (auto &junction : document.Junctions)
			if (junction.Id == "input/parent-value") junction.Default = value;
	}
}
TEST_CASE(
	"PXC Any parent arrays preserve mixed leaf kinds nested shape and opaque records",
	"[imagegraphio][pxcx_group_values]"
) {
	auto source = Source(Json::array({"kept", 4, true, Json::array({2, "nested", Json::array()})}));
	auto expected = source.Graph;
	Diagnostic error;
	REQUIRE(Migrate(expected, error) == Status::Ok);
	auto array = std::get<ArrayValue>(Parent(expected));
	CHECK(array.ElementType == ValueType::Any);
	REQUIRE(array.Items.size() == 4);
	CHECK(std::get<ElementValue>(array.Items[0].Data) == ElementValue{std::string{"kept"}});
	CHECK(std::get<ElementValue>(array.Items[1].Data) == ElementValue{int64_t{4}});
	CHECK(std::get<ElementValue>(array.Items[2].Data) == ElementValue{true});
	auto &nested = std::get<std::vector<SourceArrayItem>>(array.Items[3].Data);
	REQUIRE(nested.size() == 3);
	CHECK(std::get<std::vector<SourceArrayItem>>(nested[2].Data).empty());
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(source, expected, FrameTime{}, bytes, error));
	CHECK(bytes == source.Source.OriginalBytes);
	array.Items[0] = {ElementValue{std::string{"edited"}}};
	nested.push_back({ElementValue{false}});
	SetParent(expected, array);
	const auto saved = WritePxcxProjection(source, expected, FrameTime{}, bytes, error);
	INFO(error.Message << " " << error.NodeId << " " << error.Port);
	REQUIRE(saved);
	auto reloaded = Reload(bytes);
	REQUIRE(Migrate(reloaded.Graph, error) == Status::Ok);
	CHECK(reloaded.Graph == expected);
	CHECK(reloaded.Source.GraphJson.find("\"keep\":17") != std::string::npos);
}
TEST_CASE(
	"PXC existing Range parent static edit updates owning record key and junction atomically",
	"[imagegraphio][pxcx_group_values]"
) {
	auto source = Source(Json::array({2, 8}), 1, 1);
	const Value replacement = ArrayValue{ValueType::Scalar, {3., 9.}};
	std::vector<std::byte> bytes;
	Diagnostic error;
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"input", "parent_value", replacement}};
	const auto saved = WritePxcxEdits(source, source.Source.OriginalBytes, edits, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reloaded = Reload(bytes);
	auto desired = source.Graph;
	SetParent(desired, replacement);
	CHECK(reloaded.Graph == desired);
	CHECK(reloaded.Source.GraphJson.find("\"future_compact\":{\"keep\":29}") != std::string::npos);
	REQUIRE(Migrate(desired, error) == Status::Ok);
	REQUIRE(WritePxcxProjection(source, desired, FrameTime{}, bytes, error));
	reloaded = Reload(bytes);
	REQUIRE(Migrate(reloaded.Graph, error) == Status::Ok);
	CHECK(reloaded.Graph == desired);
}
TEST_CASE(
	"PXC Group Trigger animation enable expands compact source and retains exact pulse keys",
	"[imagegraphio][pxcx_group_values]"
) {
	auto source = Source(false, 19, 0, false);
	auto desired = source.Graph;
	Diagnostic error;
	REQUIRE(Migrate(desired, error) == Status::Ok);
	auto &node = Input(desired);
	std::erase(node.SourceStaticInputs, std::string{"parent_value"});
	node.SourceAnimatedInputs.push_back("parent_value");
	std::erase_if(node.Values, [](const auto &value) { return value.Port == "parent_value"; });
	desired.Keyframes.push_back({"input", "parent_value", 7, true, "source", KeyframeEase{}});
	for (auto &junction : desired.Junctions)
		if (junction.Id == "input/parent-value") junction.Default = -1.;
	REQUIRE(Migrate(desired, error) == Status::Ok);
	std::vector<std::byte> bytes;
	const auto saved = WritePxcxProjection(source, desired, FrameTime{}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reloaded = Reload(bytes);
	REQUIRE(Migrate(reloaded.Graph, error) == Status::Ok);
	ClearSourceIds(reloaded.Graph);
	auto semanticDesired = desired;
	ClearSourceIds(semanticDesired);
	CHECK(reloaded.Graph == semanticDesired);
	CHECK(reloaded.Source.GraphJson.find("\"keep\":17") != std::string::npos);
	auto opaqueCompact = Source(false, 19);
	bytes = {std::byte{23}};
	CHECK_FALSE(WritePxcxProjection(opaqueCompact, desired, FrameTime{}, bytes, error));
	CHECK(bytes == std::vector<std::byte>{std::byte{23}});
	CHECK(error.Message == "PXC group animation expansion would discard compact source fields");
}
TEST_CASE(
	"PXC nested array operation limits preserve destination bytes and import result",
	"[imagegraphio][pxcx_group_values]"
) {
	auto source = Source(Json::array({"kept", 4, true}));
	auto desired = source.Graph;
	auto array = std::get<ArrayValue>(Parent(desired));
	SourceArrayItem item{ElementValue{false}};
	for (size_t depth = 0; depth < Limits::MaximumArrayDepth; ++depth)
		item.Data = std::vector<SourceArrayItem>{std::move(item)};
	array.Items.push_back(std::move(item));
	SetParent(desired, array);
	std::vector<std::byte> bytes{std::byte{23}};
	Diagnostic error;
	CHECK_FALSE(WritePxcxProjection(source, desired, FrameTime{}, bytes, error));
	CHECK(bytes == std::vector<std::byte>{std::byte{23}});
	CHECK(error.NodeId == "input");
	auto retained = source;
	PxcxImportOptions options;
	options.MaximumOperationBytes = 1024;
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(source.Source, retained, failure, options));
	CHECK(retained.Graph == source.Graph);
	CHECK(retained.Source.OriginalBytes == source.Source.OriginalBytes);
}
TEST_CASE(
	"PXC Group durable key provenance preserves opaque records through compound swaps moves and native "
	"reload",
	"[imagegraphio][pxcx_group_values]"
) {
	auto source = Source(false, 19, 0, false);
	auto graph = Json::parse(source.Source.GraphJson.c_str());
	auto &parent = graph["nodes"][0]["inputs"][0];
	parent["anim"] = true;
	parent["r"] = Json::array(
		{Json::array(
			 {Json::array({0, 2}),
			  false,
			  Json::array({0, 1}),
			  Json::array({0, 0}),
			  0,
			  0,
			  true,
			  0,
			  16777215,
			  Json{{"opaque_key", 43}}}
		 ),
		 Json::array(
			 {Json::array({0, 5}),
			  true,
			  Json::array({0, 1}),
			  Json::array({0, 0}),
			  0,
			  0,
			  true,
			  0,
			  16777215,
			  Json{{"opaque_key", 44}}}
		 )}
	);
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.22.10.201";
	archive.GraphJson = graph.dump();
	archive.GraphJson.push_back('\0');
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
	source = Reload(bytes);
	const auto original = source.Graph;
	auto desired = original;
	Diagnostic error;
	REQUIRE(Migrate(desired, error) == Status::Ok);
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "input" && key.Port == "parent_value") {
			if (key.Tick == 2) {
				key.Tick = 7;
				key.Data = true;
			} else {
				REQUIRE(key.Tick == 5);
				key.Tick = 8;
				key.Data = false;
			}
		}
	std::reverse(desired.Keyframes.begin(), desired.Keyframes.end());
	const auto retained = desired;
	Document nativeReload;
	REQUIRE(Read(Write(desired), nativeReload, error) == Status::Ok);
	CHECK(nativeReload == desired);
	const auto written = WritePxcxProjection(source, nativeReload, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(written);
	CHECK(source.Graph == original);
	CHECK(desired == retained);
	auto moved = Reload(bytes);
	const auto movedJson = Json::parse(moved.Source.GraphJson.c_str());
	const auto &records = movedJson["nodes"][0]["inputs"][0]["r"];
	REQUIRE(records.size() == 2);
	const auto firstRecord = std::find_if(records.begin(), records.end(), [](const auto &record) {
		return record.size() > 9 && record[9]["opaque_key"] == 43;
	});
	const auto secondRecord = std::find_if(records.begin(), records.end(), [](const auto &record) {
		return record.size() > 9 && record[9]["opaque_key"] == 44;
	});
	REQUIRE(firstRecord != records.end());
	REQUIRE(secondRecord != records.end());
	CHECK((*firstRecord)[0][1] == 7);
	CHECK((*firstRecord)[1] == true);
	CHECK((*firstRecord)[9]["opaque_key"] == 43);
	CHECK((*secondRecord)[0][1] == 8);
	CHECK((*secondRecord)[1] == false);
	CHECK((*secondRecord)[9]["opaque_key"] == 44);
	REQUIRE(Migrate(moved.Graph, error) == Status::Ok);
	ClearSourceIds(moved.Graph);
	auto semanticDesired = desired;
	ClearSourceIds(semanticDesired);
	CHECK(moved.Graph == semanticDesired);

	// Deliberately forged or duplicated identities refuse before publishing destination bytes.
	auto duplicate = desired;
	auto first = std::find_if(duplicate.Keyframes.begin(), duplicate.Keyframes.end(), [](const auto &key) {
		return key.Port == "parent_value";
	});
	REQUIRE(first != duplicate.Keyframes.end());
	auto next = std::find_if(first + 1, duplicate.Keyframes.end(), [](const auto &key) {
		return key.Port == "parent_value";
	});
	REQUIRE(next != duplicate.Keyframes.end());
	next->SourceKeyId = first->SourceKeyId;
	bytes = {std::byte{23}};
	CHECK_FALSE(WritePxcxProjection(source, duplicate, {}, bytes, error));
	CHECK(bytes == std::vector<std::byte>{std::byte{23}});
	next->SourceKeyId = "unknown-source-key";
	CHECK_FALSE(WritePxcxProjection(source, duplicate, {}, bytes, error));
	CHECK(bytes == std::vector<std::byte>{std::byte{23}});

	// An edit at the original exact source position retains that record's opaque tail.
	desired = original;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "input" && key.Port == "parent_value" && key.Tick == 2) key.Data = true;
	REQUIRE(Migrate(desired, error) == Status::Ok);
	REQUIRE(WritePxcxProjection(source, desired, {}, bytes, error));
	auto reloaded = Reload(bytes);
	REQUIRE(Migrate(reloaded.Graph, error) == Status::Ok);
	CHECK(reloaded.Graph == desired);
	CHECK(reloaded.Source.GraphJson.find("\"opaque_key\":43") != std::string::npos);
	CHECK(reloaded.Source.GraphJson.find("\"opaque_key\":44") != std::string::npos);
}

TEST_CASE(
	"PXC ordinary source animator uses provenance through simultaneous key swaps",
	"[imagegraphio][pxcx_group_values]"
) {
	const Json records = Json::array(
		{Json::array(
			 {Json::array({0, 2}),
			  1.0,
			  Json::array({0, 1}),
			  Json::array({0, 0}),
			  0,
			  0,
			  true,
			  0,
			  16777215,
			  Json{{"opaque_key", 43}}}
		 ),
		 Json::array(
			 {Json::array({0, 5}),
			  2.0,
			  Json::array({0, 1}),
			  Json::array({0, 0}),
			  0,
			  0,
			  true,
			  0,
			  16777215,
			  Json{{"opaque_key", 44}}}
		 )}
	);
	const Json graph = {
		{"nodes",
		 Json::array({Json{
			 {"id", "number"},
			 {"type", "Node_Number_Simple"},
			 {"x", 0},
			 {"y", 0},
			 {"inputs", Json::array({Json{{"anim", true}, {"r", records}}})}
		 }})}
	};
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.22.10.201";
	archive.GraphJson = graph.dump();
	archive.GraphJson.push_back('\0');
	std::vector<std::byte> bytes;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
	auto source = Reload(bytes);
	REQUIRE(source.Graph.Keyframes.size() == 2);
	auto desired = source.Graph;
	Diagnostic error;
	REQUIRE(Migrate(desired, error) == Status::Ok);
	const auto firstId = desired.Keyframes[0].SourceKeyId;
	const auto secondId = desired.Keyframes[1].SourceKeyId;
	CHECK_FALSE(firstId.empty());
	CHECK(firstId != secondId);
	desired.Keyframes[0].Tick = 5;
	desired.Keyframes[0].Data = 20.0;
	desired.Keyframes[1].Tick = 2;
	desired.Keyframes[1].Data = 10.0;
	const auto saved = WritePxcxProjection(source, desired, {}, bytes, error);
	INFO(error.Message);
	REQUIRE(saved);
	auto reloaded = Reload(bytes);
	const auto result = Json::parse(reloaded.Source.GraphJson.c_str());
	const auto &keys = result["nodes"][0]["inputs"][0]["r"];
	REQUIRE(keys.size() == 2);
	for (const auto &key : keys) {
		if (key[0][1] == 5) {
			CHECK(key[1] == 20.0);
			CHECK(key[9]["opaque_key"] == 43);
		} else {
			CHECK(key[0][1] == 2);
			CHECK(key[1] == 10.0);
			CHECK(key[9]["opaque_key"] == 44);
		}
	}
	CHECK(desired.Keyframes[0].SourceKeyId == firstId);
	CHECK(desired.Keyframes[1].SourceKeyId == secondId);
}
