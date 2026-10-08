// Source Markov Gradient controls, keys and palette metadata remain editable across PXC saves.
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.source_markov_gradient")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}};
	}
	Json Wire(std::string id, int index = 0) {
		return {{"from_node", std::move(id)}, {"from_index", index}, {"from_tag", 0}};
	}
	Json Record(std::string id, const char *type, Json inputs) {
		return {
			{"id", std::move(id)},
			{"type", type},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", "keep"}
		};
	}
	Json Row(double time, Json value, const char *identity) {
		return Json::array(
			{Json::array({0, time, "marker"}),
			 value,
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 16777215,
			 Json{{"future_key", identity}}}
		);
	}
	Json UvRecord(std::string id, int width, int height, int64_t depth) {
		Json inputs = std::vector<Json>(12, Json::object());
		inputs[0] = Fixed(Json::array({width, height}));
		inputs[0]["attri"] = {{"use_project_dimension", 0}};
		auto node = Record(std::move(id), "Node_UV_Cartesian", inputs);
		node["attri"] = {{"color_depth", depth}};
		return node;
	}
	Json NumberRecord(std::string id, double value) {
		return Record(std::move(id), "Node_Number_Simple", Json::array({Fixed(value)}));
	}
	Json BooleanRecord(std::string id, bool value) {
		Json inputs = std::vector<Json>(3, Json::object());
		inputs[0] = Fixed(value);
		return Record(std::move(id), "Node_Boolean", inputs);
	}
	Json Project(bool mapped = true, bool linked = false, bool grouped = false, bool animated = false) {
		Json controls = Json::array(
			{Wire("uv"),
			 Fixed(17),
			 Fixed(true),
			 Fixed(Json::array({4278190335ULL, 4278255360ULL, 4294901760ULL})),
			 Fixed(2.),
			 Fixed(mapped ? Json::array({.1, .9}) : Json(.5)),
			 Wire("map")}
		);
		controls[5]["attri"] = {{"mapped", mapped}, {"future_map", "keep"}};
		if (animated)
			controls[4] = {{"anim", true}, {"r", Json::array({Row(0, .1, "first"), Row(10, 2, "last")})}};
		if (linked) {
			for (const auto &[index, id] : std::array<std::pair<size_t, const char *>, 5>{
					 {{1, "seed"}, {2, "active"}, {3, "palette"}, {4, "threshold"}, {5, "chance"}}
				 }) {
				controls[index]["from_node"] = id;
				controls[index]["from_index"] = 0;
				controls[index]["from_tag"] = 0;
			}
		}
		for (auto &input : controls)
			input["future_input"] = "keep";
		auto effect = Record("tile", "Node_Markov_Gradient", controls);
		effect["attri"] = {{"process", true}, {"array_process", 0}, {"future_attribute", "keep"}};
		Json nodes = Json::array(
			{UvRecord("uv", 8, 4, 4),
			 std::move(effect),
			 UvRecord("map", 3, 2, 3),
			 NumberRecord("seed", 17),
			 BooleanRecord("active", true),
			 Record(
				 "palette",
				 "Node_Palette",
				 Json::array(
					 {Fixed(Json::array({4278190335ULL, 4278255360ULL, 4294901760ULL})),
					  Fixed(Json::array({0, 1}))}
				 )
			 ),
			 NumberRecord("threshold", 2),
			 NumberRecord("chance", .5)}
		);
		if (grouped) {
			for (auto &child : nodes)
				child["group"] = "group";
			auto group = Record("group", "Node_Group", Json::array());
			group["attri"] = {{"custom_input_list", Json::array()}, {"custom_output_list", Json::array()}};
			nodes.push_back(std::move(group));
		}
		return {
			{"attributes",
			 {{"surface_dimension", Json::array({12, 6})},
			  {"palette", Json::array({4278190335ULL, 4278255360ULL, 4294901760ULL})}}},
			{"animator", {{"frames_total", 20}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", "keep"},
			{"nodes", std::move(nodes)}
		};
	}
	engine::bake::PxcxArchive Archive(const Json &project) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = project.dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		return archive;
	}
	PxcxImport Import(const Json &project) {
		PxcxImport imported;
		std::string failure;
		const bool accepted = ImportPxcxImageGraph(Archive(project), imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	PxcxImport Reopen(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		REQUIRE(ImportPxcxImageGraph(archive, imported, failure));
		return imported;
	}
	Node &Native(Document &document, std::string_view id) {
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &value) {
			return value.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	const Value &Authored(const Node &node, std::string_view port) {
		for (const auto &value : node.Values)
			if (value.Port == port) return value.Data;
		FAIL("missing mapped Markov Gradient input: " << port);
		return node.Values.front().Data;
	}
	Json Source(const PxcxImport &imported) {
		return Json::parse(
			std::string_view(imported.Source.GraphJson.data(), imported.Source.GraphJson.size() - 1)
		);
	}
	Image Sample(Document document, int64_t tick = 0, double subframe = 0) {
		document.Outputs = {{"image", "tile", "surface_out"}};
		Plan plan;
		Diagnostic error;
		auto status = Compile(document, plan, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		request.Subframe = subframe;
		Image image;
		status = Evaluate(document, plan, "image", request, image, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	Document Edit(const PxcxImport &imported, std::span<const PxcxEdit> edits) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxEdits(imported, imported.Source.OriginalBytes, edits, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		return Reopen(bytes).Graph;
	}
	PxcxImport Save(const PxcxImport &imported, const Document &desired) {
		std::vector<std::byte> bytes;
		Diagnostic error;
		const bool saved = WritePxcxProjection(imported, desired, {}, bytes, error);
		INFO(error.Message);
		REQUIRE(saved);
		return Reopen(bytes);
	}
	void NativeRoundtrip(const Document &document, int64_t tick = 0) {
		Document reopened;
		Diagnostic error;
		REQUIRE(Read(Write(document), reopened, error) == Status::Ok);
		CHECK(reopened == document);
		CHECK(Sample(reopened, tick) == Sample(document, tick));
	}
	void Set(Document &document, std::string_view port, Value value) {
		auto &node = Native(document, "tile");
		for (auto &input : node.Values)
			if (input.Port == port) {
				input.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::string(port), std::move(value)});
	}
	ArrayValue Palette(std::initializer_list<Colour> colours) {
		ArrayValue value{ValueType::Colour, {}};
		for (const auto &colour : colours)
			value.Elements.emplace_back(colour);
		return value;
	}
	ImageArray Rows(Document document, uint64_t tick = 0) {
		document.Outputs = {{"image", "tile", "surface_out"}};
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(document, plan, error) == Status::Ok);
		ImageArray images;
		EvaluationRequest request;
		request.Tick = tick;
		const auto status = EvaluateArray(document, plan, "image", request, images, error);
		INFO(error.Message << " " << error.NodeId << ":" << error.Port);
		REQUIRE(status == Status::Ok);
		return images;
	}
	void Unsupported(Document document, std::string_view port, uint64_t tick = 0) {
		document.Outputs = {{"image", "tile", "surface_out"}};
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(document, plan, error) == Status::Ok);
		ImageArray sentinel;
		sentinel.Images.push_back(Image{1, 1, {71, 72, 73, 74}, 123});
		const auto previous = sentinel;
		EvaluationRequest request;
		request.Tick = tick;
		CHECK(
			EvaluateArray(document, plan, "image", request, sentinel, error) == Status::UnsupportedExecution
		);
		INFO(error.Message);
		CHECK(error.Port == port);
		CHECK(sentinel.Images == previous.Images);
		CHECK(sentinel.Items == previous.Items);
	}
}

TEST_CASE(
	"PXC Markov Gradient retains all source controls and mapped chance range", "[source_markov_gradient]"
) {
	const auto project = Project();
	auto imported = Import(project);
	const auto &node = Native(imported.Graph, "tile");
	REQUIRE(node.Type == "pc.markov_gradient");
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	CHECK(std::count_if(entry->Inputs.begin(), entry->Inputs.end(), [](const auto &input) {
			  return input.SourceIndex >= 0;
		  }) == 7);
	CHECK(Authored(node, "seed") == Value{17.});
	CHECK(Authored(node, "active") == Value{true});
	CHECK(Authored(node, "threshold") == Value{2.});
	CHECK(Authored(node, "colors") == Value{Palette({{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}})});
	CHECK(Authored(node, "replace_chance_mapped") == Value{true});
	CHECK(Authored(node, "replace_chance_map_range") == Value{Vector2{.1, .9}});
	const auto image = Sample(imported.Graph);
	CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
	auto scalarChance = imported.Graph;
	Set(scalarChance, "replace_chance", .1);
	Set(scalarChance, "replace_chance_map_range", Vector2{.1, .1});
	CHECK(Sample(scalarChance) != image);
	NativeRoundtrip(imported.Graph);
	auto desired = imported.Graph;
	Native(desired, "tile").Position = {21, 13};
	auto reopened = Save(imported, desired);
	CHECK(Sample(reopened.Graph) == image);
	CHECK(Source(reopened)["nodes"][1]["inputs"] == project["nodes"][1]["inputs"]);
	CHECK(Source(reopened)["nodes"][1]["attri"] == project["nodes"][1]["attri"]);
	CHECK(Source(reopened)["attributes"] == project["attributes"]);
}

TEST_CASE(
	"PXC Markov Gradient physical edits retain metadata and replay global frame pixels",
	"[source_markov_gradient]"
) {
	const auto imported = Import(Project(false));
	const std::array<PxcxEdit, 5> edits{
		PxcxInputValueEdit{"tile", "seed", 29.},
		PxcxInputValueEdit{"tile", "active", true},
		PxcxInputValueEdit{"tile", "colors", Palette({{0, 0, 255, 255}, {255, 0, 0, 255}})},
		PxcxInputValueEdit{"tile", "threshold", 1.5},
		PxcxInputValueEdit{"tile", "replace_chance", .375}
	};
	const auto desired = Edit(imported, edits);
	const auto reopened = Save(imported, desired);
	for (int64_t tick : {0, 5, 27}) {
		CAPTURE(tick);
		CHECK(Sample(reopened.Graph, tick, .25) == Sample(desired, tick, .25));
		NativeRoundtrip(desired, tick);
		const std::array<PxcxEdit, 1> offsetSeed{PxcxInputValueEdit{"tile", "seed", 29. + tick + .25}};
		const auto equivalent = Edit(reopened, offsetSeed);
		CHECK(Sample(desired, tick, .25) == Sample(equivalent));
	}
	const auto source = Source(reopened);
	CHECK(source["nodes"][1]["inputs"][1]["r"]["d"] == 29.);
	CHECK(source["nodes"][1]["inputs"][3]["r"]["d"] == Json::array({4294901760ULL, 4278190335ULL}));
	for (const auto &input : source["nodes"][1]["inputs"])
		CHECK(input["future_input"] == "keep");
	CHECK(source["nodes"][1]["inputs"][5]["attri"] == Source(imported)["nodes"][1]["inputs"][5]["attri"]);
}

TEST_CASE(
	"PXC Markov Gradient linked physical chance overrides its captured mapped range in groups",
	"[source_markov_gradient]"
) {
	const auto ordinary = Import(Project(true, true));
	auto grouped = Import(Project(true, true, true));
	REQUIRE(Native(grouped.Graph, "tile").Type == "pc.markov_gradient");
	CHECK(grouped.Graph.Links.size() == 7);
	CHECK(Sample(grouped.Graph) == Sample(ordinary.Graph));
	auto captured = ordinary.Graph;
	for (auto &link : captured.Links)
		if (link.ToNode == "tile" && link.ToPort == "replace_chance") link.FromNode = "threshold";
	CHECK(Sample(captured) != Sample(ordinary.Graph));
	const std::array<PxcxEdit, 3> edits{
		PxcxInputValueEdit{"seed", "value", 31.},
		PxcxInputValueEdit{"chance", "value", .75},
		PxcxInputValueEdit{"threshold", "value", 1.25}
	};
	const auto desired = Edit(grouped, edits);
	auto reopened = Save(grouped, desired);
	CHECK(Sample(reopened.Graph, 9) == Sample(Edit(ordinary, edits), 9));
	CHECK(Native(reopened.Graph, "tile").GroupId == "group");
	CHECK(reopened.Graph.Links == grouped.Graph.Links);
	CHECK(Source(reopened)["nodes"][1] == Source(grouped)["nodes"][1]);
	NativeRoundtrip(reopened.Graph, 9);
}

TEST_CASE("PXC Markov Gradient threshold key edits preserve unknown key fields", "[source_markov_gradient]") {
	const auto project = Project(true, false, true, true);
	const auto imported = Import(project);
	auto desired = imported.Graph;
	unsigned changed = 0;
	for (auto &key : desired.Keyframes)
		if (key.NodeId == "tile" && key.Port == "threshold" && key.Tick == 10) {
			key.Tick = 12;
			key.Data = 1.25;
			++changed;
		}
	REQUIRE(changed == 1);
	const auto reopened = Save(imported, desired);
	for (int64_t tick : {0, 5, 12}) {
		CHECK(Sample(reopened.Graph, tick) == Sample(desired, tick));
		NativeRoundtrip(desired, tick);
	}
	const auto source = Source(reopened);
	const auto &keys = source["nodes"][1]["inputs"][4]["r"];
	CHECK(keys[1][0][1] == 12);
	CHECK(keys[1][1] == 1.25);
	CHECK(keys[0][9] == project["nodes"][1]["inputs"][4]["r"][0][9]);
	CHECK(keys[1][9] == project["nodes"][1]["inputs"][4]["r"][1][9]);
}

TEST_CASE(
	"PXC Markov Gradient processor modes preserve source arrays and native row images",
	"[source_markov_gradient]"
) {
	for (int64_t mode : {0, 1, 2, 3}) {
		CAPTURE(mode);
		auto project = Project(false, false, true);
		project["nodes"][1]["attri"]["array_process"] = mode;
		project["nodes"][1]["inputs"][1] = Fixed(Json::array({11., 29.}));
		project["nodes"][1]["inputs"][4] = Fixed(Json::array({.1, 1., 2.}));
		auto imported = Import(project);
		REQUIRE(Native(imported.Graph, "tile").Type == "pc.markov_gradient");
		const auto expected = Rows(imported.Graph);
		CHECK(expected.Images.size() == (mode < 2 ? 3 : 6));
		Document native;
		Diagnostic error;
		REQUIRE(Read(Write(imported.Graph), native, error) == Status::Ok);
		CHECK(Rows(native).Images == expected.Images);
		CHECK(Rows(native).Items == expected.Items);
		auto desired = imported.Graph;
		Native(desired, "tile").Position.X = 20;
		const auto reopened = Save(imported, desired);
		CHECK(Rows(reopened.Graph).Images == expected.Images);
		CHECK(Rows(reopened.Graph).Items == expected.Items);
		CHECK(Source(reopened)["nodes"][1]["attri"] == project["nodes"][1]["attri"]);
	}
}

TEST_CASE(
	"PXC Markov Gradient inactive arrays retain their whole source format and skip missing seed",
	"[source_markov_gradient]"
) {
	auto project = Project(false);
	project["nodes"][1]["inputs"][1] = Json::object();
	project["nodes"][1]["inputs"][2] = Fixed(false);
	project["nodes"][1]["inputs"][4] = Fixed(Json::array({.1, 1., 2.}));
	project["nodes"][1]["attri"]["array_process"] = 2;
	project["nodes"][0]["inputs"][0] = Fixed(Json::array({Json::array({4, 2}), Json::array({8, 4})}));
	project["nodes"][0]["inputs"][0]["attri"] = {{"use_project_dimension", 0}};
	auto imported = Import(project);
	const auto expected = Rows(imported.Graph);
	CHECK(expected.Images.size() == 2);
	for (const auto &image : expected.Images)
		CHECK(image.Format == SurfaceFormat::RGBA16Float);
	auto sourceOnly = imported.Graph;
	for (auto &output : sourceOnly.Outputs)
		output.NodeId = "uv";
	sourceOnly.Outputs = {{"source", "uv", "surface_out"}};
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(sourceOnly, plan, error) == Status::Ok);
	ImageArray sourceImages;
	REQUIRE(EvaluateArray(sourceOnly, plan, "source", {}, sourceImages, error) == Status::Ok);
	CHECK(expected.Images == sourceImages.Images);
	CHECK(expected.Items == sourceImages.Items);
	Document native;
	REQUIRE(Read(Write(imported.Graph), native, error) == Status::Ok);
	CHECK(Rows(native).Images == expected.Images);
	auto desired = imported.Graph;
	Native(desired, "tile").Position.X = 33;
	const auto reopened = Save(imported, desired);
	CHECK(Rows(reopened.Graph).Images == expected.Images);
	CHECK(Rows(reopened.Graph).Items == expected.Items);
	const std::array<PxcxEdit, 1> activate{PxcxInputValueEdit{"tile", "active", true}};
	Unsupported(Edit(reopened, activate), "seed");
}

TEST_CASE(
	"PXC Markov Gradient uses durable project palette only when stored colours are absent",
	"[source_markov_gradient]"
) {
	auto missing = Project(false);
	missing["nodes"][1]["inputs"][3] = Json::object();
	auto imported = Import(missing);
	REQUIRE(imported.Graph.Project);
	REQUIRE(
		imported.Graph.Project->Palette ==
		std::vector<Colour>{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}}
	);
	CHECK(Sample(imported.Graph) == Sample(Import(Project(false)).Graph));
	NativeRoundtrip(imported.Graph);
	auto reopened = Save(imported, imported.Graph);
	CHECK(Source(reopened)["attributes"]["palette"] == missing["attributes"]["palette"]);
	auto explicitColours = Import(Project(false));
	const auto expected = Sample(explicitColours.Graph);
	REQUIRE(explicitColours.Graph.Project);
	explicitColours.Graph.Project->Palette = {{0, 0, 0, 255}, {255, 255, 255, 255}};
	CHECK(Sample(explicitColours.Graph) == expected);
	NativeRoundtrip(explicitColours.Graph);
}

TEST_CASE("PXC Markov Gradient refuses unrepresented source layouts atomically", "[source_markov_gradient]") {
	auto project = Project();
	std::string_view layout;
	SECTION("bad palette leaf") {
		layout = "palette string leaf";
		project["nodes"][1]["inputs"][3] = Fixed(Json::array({"unrepresented"}));
	}
	SECTION("bad mapped chance record") {
		layout = "mapped chance object has no numeric scalar or range meaning";
		project["nodes"][1]["inputs"][5] = Fixed(Json{{"unrepresented", 1}});
		project["nodes"][1]["inputs"][5]["attri"]["mapped"] = true;
	}
	SECTION("bad seed") {
		layout = "seed string";
		project["nodes"][1]["inputs"][1] = Fixed("unknown");
	}
	SECTION("unknown input") {
		layout = "physical input index seven";
		project["nodes"][1]["inputs"].push_back(Fixed(1));
	}
	INFO(layout);
	auto imported = Import(project);
	CHECK(Native(imported.Graph, "tile").Type == "pxcx.opaque/Node_Markov_Gradient");
	CHECK_FALSE(imported.Diagnostics.empty());
	Diagnostic error;
	std::vector<std::byte> bytes;
	REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, error));
	CHECK(bytes == imported.Source.OriginalBytes);
	auto desired = imported.Graph;
	Native(desired, "tile").Type = "pc.markov_gradient";
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	bytes = sentinel;
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
	CHECK(bytes == sentinel);
}

TEST_CASE(
	"PXC Markov Gradient synthetic and stale edits preserve archive and output sentinels",
	"[source_markov_gradient]"
) {
	const auto imported = Import(Project());
	const auto original = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const std::vector<std::byte> sentinel{std::byte{0x61}};
	Diagnostic error;
	for (const auto &[port, value] : std::array<AuthoredValue, 4>{
			 {{"replace_chance_mapped", false},
			  {"replace_chance_map_range", Vector2{.2, .8}},
			  {"attribute_process", false},
			  {"attribute_array_process", EnumValue{1}}}
		 }) {
		const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"tile", port, value}};
		auto bytes = sentinel;
		CHECK_FALSE(WritePxcxEdits(imported, originalBytes, edits, bytes, error));
		CHECK(bytes == sentinel);
	}
	const std::array<PxcxEdit, 1> edits{PxcxInputValueEdit{"tile", "seed", 31.}};
	auto bytes = sentinel;
	CHECK_FALSE(WritePxcxEdits(imported, sentinel, edits, bytes, error));
	CHECK(bytes == sentinel);
	auto desired = original;
	Set(desired, "seed", std::string{"unrepresented"});
	CHECK_FALSE(WritePxcxProjection(imported, desired, {}, bytes, error));
	CHECK(bytes == sentinel);
	CHECK(imported.Graph == original);
	CHECK(imported.Source.OriginalBytes == originalBytes);
}

TEST_CASE(
	"PXC Markov Gradient nested palette axes retain packed source colours across key edits",
	"[source_markov_gradient]"
) {
	const auto first = Json::array(
		{Json::array({4278190335ULL, 4278255360ULL}), Json::array({4294901760ULL, 4278190335ULL})}
	);
	const auto last = Json::array(
		{Json::array({4278255360ULL, 4294901760ULL}), Json::array({4278190335ULL, 4294901760ULL})}
	);
	for (bool animated : {false, true}) {
		CAPTURE(animated);
		auto project = Project(false, false, true);
		project["nodes"][1]["inputs"][3] =
			animated
				? Json{{"anim", true}, {"r", Json::array({Row(0, first, "first"), Row(10, last, "last")})}}
				: Fixed(first);
		auto imported = Import(project);
		REQUIRE(Native(imported.Graph, "tile").Type == "pc.markov_gradient");
		CHECK(Rows(imported.Graph).Images.size() == 2);
		auto desired = imported.Graph;
		if (animated) {
			unsigned changed = 0;
			for (auto &key : desired.Keyframes)
				if (key.NodeId == "tile" && key.Port == "colors" && key.Tick == 10) {
					key.Tick = 12;
					++changed;
				}
			REQUIRE(changed == 1);
		} else
			Native(desired, "tile").Position.X = 31;
		Document native;
		Diagnostic error;
		REQUIRE(Read(Write(desired), native, error) == Status::Ok);
		CHECK(Rows(native).Images == Rows(desired).Images);
		CHECK(Rows(native).Items == Rows(desired).Items);
		const auto reopened = Save(imported, desired);
		CHECK(Rows(reopened.Graph).Images == Rows(desired).Images);
		CHECK(Rows(reopened.Graph).Items == Rows(desired).Items);
		for (uint64_t tick : {0ULL, 12ULL}) {
			CHECK(Rows(native, tick).Images == Rows(desired, tick).Images);
			CHECK(Rows(reopened.Graph, tick).Images == Rows(desired, tick).Images);
			CHECK(Rows(reopened.Graph, tick).Items == Rows(desired, tick).Items);
		}
		const auto source = Source(reopened);
		if (animated) {
			auto endpointProject = project;
			endpointProject["nodes"][1]["inputs"][3] = Fixed(first);
			CHECK(Rows(imported.Graph, 0).Images == Rows(Import(endpointProject).Graph, 0).Images);
			endpointProject["nodes"][1]["inputs"][3] = Fixed(last);
			const auto lastEndpoint = Import(endpointProject);
			CHECK(Rows(imported.Graph, 10).Images == Rows(lastEndpoint.Graph, 10).Images);
			CHECK(Rows(desired, 12).Images == Rows(lastEndpoint.Graph, 12).Images);
			Unsupported(imported.Graph, "colors", 6);
			Unsupported(desired, "colors", 6);
			Unsupported(native, "colors", 6);
			Unsupported(reopened.Graph, "colors", 6);
			const auto &keys = source["nodes"][1]["inputs"][3]["r"];
			CHECK(keys[0][1] == first);
			CHECK(keys[1][1] == last);
			CHECK(keys[1][0][1] == 12);
			CHECK(keys[0][9] == project["nodes"][1]["inputs"][3]["r"][0][9]);
			CHECK(keys[1][9] == project["nodes"][1]["inputs"][3]["r"][1][9]);
		} else
			CHECK(source["nodes"][1]["inputs"][3]["r"]["d"] == first);
	}
}

TEST_CASE("PXC Markov Gradient admits source palettes at one and 256 colours", "[source_markov_gradient]") {
	for (size_t size : {size_t{1}, size_t{256}}) {
		CAPTURE(size);
		auto project = Project(false);
		Json colours = Json::array();
		for (size_t index = 0; index < size; ++index)
			colours.push_back(index % 2 ? 4278255360ULL : 4278190335ULL);
		project["nodes"][1]["inputs"][3] = Fixed(colours);
		auto imported = Import(project);
		REQUIRE(Native(imported.Graph, "tile").Type == "pc.markov_gradient");
		const auto expected = Sample(imported.Graph);
		NativeRoundtrip(imported.Graph);
		auto desired = imported.Graph;
		Native(desired, "tile").Position.X = 32;
		const auto reopened = Save(imported, desired);
		CHECK(Sample(reopened.Graph) == expected);
		CHECK(Source(reopened)["nodes"][1]["inputs"][3]["r"]["d"] == colours);
	}
}

TEST_CASE(
	"PXC Markov Gradient malformed nested packed colours remain opaque and lossless",
	"[source_markov_gradient]"
) {
	for (int64_t colour : {-1LL, 4294967296LL}) {
		CAPTURE(colour);
		auto project = Project(false);
		project["nodes"][1]["inputs"][3] = Fixed(Json::array({Json::array({255, colour})}));
		auto imported = Import(project);
		CHECK(Native(imported.Graph, "tile").Type == "pxcx.opaque/Node_Markov_Gradient");
		CHECK_FALSE(imported.Diagnostics.empty());
		Diagnostic error;
		std::vector<std::byte> bytes;
		REQUIRE(WritePxcxProjection(imported, imported.Graph, {}, bytes, error));
		CHECK(bytes == imported.Source.OriginalBytes);
		CHECK(Source(imported) == project);
	}
}

TEST_CASE(
	"PXC Markov Gradient nested palette allocation refusal retains the previous import",
	"[source_markov_gradient]"
) {
	auto previous = Import(Project(false));
	const auto graph = previous.Graph;
	const auto bytes = previous.Source.OriginalBytes;
	const auto metadata = previous.Source.MetadataText;
	auto project = Project(false);
	project["nodes"][1]["inputs"][3] = Fixed(Json::array({Json::array({4278190335ULL, 4278255360ULL})}));
	PxcxImportOptions options;
	options.MaximumOperationBytes = 1;
	std::string failure;
	CHECK_FALSE(ImportPxcxImageGraph(Archive(project), previous, failure, options));
	CHECK_FALSE(failure.empty());
	CHECK(previous.Graph == graph);
	CHECK(previous.Source.OriginalBytes == bytes);
	CHECK(previous.Source.MetadataText == metadata);
}

TEST_CASE(
	"PXC Markov Gradient active single channel inputs remain raw red in RGBA8", "[source_markov_gradient]"
) {
	auto project = Project(false);
	project["nodes"][0]["attri"]["color_depth"] = 6;
	project["nodes"][1]["inputs"][5] = Fixed(-1.);
	project["nodes"][1]["inputs"][5]["attri"]["mapped"] = false;
	auto imported = Import(project);
	const auto output = Sample(imported.Graph);
	CHECK(output.Format == SurfaceFormat::RGBA8Unorm);
	for (size_t offset = 0; offset < output.Pixels.size(); offset += 4) {
		CHECK(output.Pixels[offset + 1] == 0);
		CHECK(output.Pixels[offset + 2] == 0);
		CHECK(output.Pixels[offset + 3] == 255);
	}
	CHECK(std::any_of(output.Pixels.begin(), output.Pixels.end(), [](auto value) {
		return value > 0 && value < 255;
	}));
	NativeRoundtrip(imported.Graph);
	CHECK(Sample(Save(imported, imported.Graph).Graph) == output);
}

TEST_CASE("PXC Markov Gradient process metadata leaves scalar drawing enabled", "[source_markov_gradient]") {
	auto project = Project();
	project["nodes"][1]["attri"]["process"] = false;
	auto imported = Import(project);
	CHECK(Sample(imported.Graph) == Sample(Import(Project()).Graph));
	NativeRoundtrip(imported.Graph);
	auto desired = imported.Graph;
	Native(desired, "tile").Position.X = 35;
	const auto reopened = Save(imported, desired);
	CHECK(Sample(reopened.Graph) == Sample(desired));
	CHECK(Source(reopened)["nodes"][1]["attri"]["process"] == false);
}
