#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceAxisTransition.hpp>
#include <engine/imagegraph/SourceInputProcessingObserver.hpp>
#include <engine/imagegraphio/PxcxEdit.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_combined_vec2_tuple")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace pxcx_combined_tuple {
	using Json = nlohmann::json;
	Json Row(double frame, double value) {
		return Json::array(
			{Json::array({0, frame, "marker"}),
			 value,
			 Json::array({0, 1}),
			 Json::array({0, 0}),
			 0,
			 0,
			 true,
			 0,
			 16777215,
			 Json{{"future", "keep"}}}
		);
	}
	PxcxImport Import(const engine::bake::PxcxArchive &archive) {
		PxcxImport result;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(archive, result, failure));
		return result;
	}
	struct Observer : SourceInputProcessingObserver {
		std::vector<AuthoredValue> Inputs;
		std::string_view NodeId() const noexcept override {
			return "mirror";
		}
		uint64_t RetainedBytes() const noexcept override {
			return 0;
		}
		Status Observe(
			FrameTime,
			std::span<const EvaluationInputValue> values,
			std::span<const EvaluationInputImage>,
			std::span<const SnapshotImageArray>,
			Diagnostic &,
			uint64_t
		) override {
			Inputs.clear();
			for (const auto &value : values)
				Inputs.push_back({value.Port, value.Data});
			return Status::Ok;
		}
	};
	GroupReplayState Bind(const Document &graph, Diagnostic &error) {
		GroupReplayState empty, declared, bound;
		REQUIRE(RebindGroupReplay(graph, empty, 1, declared, error) == Status::Ok);
		REQUIRE(BindGroupReplay(graph, {}, declared, 1, bound, error) == Status::Ok);
		return bound;
	}
}
TEST_CASE(
	"PXC Combine numeric pair keys survive projection and re-separation",
	"[pxcx_vec2_axes][source_axis_transition]"
) {
	using namespace pxcx_combined_tuple;
	const auto *entry = FindCatalogueEntry("pc.mirror_polar");
	REQUIRE(entry);
	const auto *center = FindCatalogueInput(*entry, "center");
	REQUIRE(center);
	const auto *surface = FindCatalogueInput(*entry, "surface_in");
	REQUIRE(surface);
	Json inputs = Json::array();
	for (int32_t index = 0; index <= std::max(center->SourceIndex, surface->SourceIndex); ++index)
		inputs.push_back(Json::object());
	inputs[surface->SourceIndex] = {{"from_node", "source"}, {"from_index", 0}};
	inputs[center->SourceIndex] = {
		{"anim", true},
		{"sep_axis", true},
		{"r", {{"d", Json::array({.1, .2})}}},
		{"global_key", "value + self.center"},
		{"global_use", true},
		{"animators",
		 Json::array(
			 {Json::array({Row(0, .1), Row(10, .9)}),
			  Json::array({Row(0, .2), Row(10, .8)}),
			  Json{{"future_axis", "keep"}}}
		 )},
		{"future_input", "keep"}
	};
	const auto *solid = FindCatalogueEntry("pc.solid");
	REQUIRE(solid);
	const auto *dimension = FindCatalogueInput(*solid, "dimension");
	REQUIRE(dimension);
	Json solidInputs = Json::array();
	for (int32_t i = 0; i <= dimension->SourceIndex; ++i)
		solidInputs.push_back(Json::object());
	solidInputs[dimension->SourceIndex] = {{"r", {{"d", Json::array({1, 1})}}}};
	Json root = {
		{"animator", {{"frames_total", 11}, {"playback", 0}, {"framerate", 30}}},
		{"nodes",
		 Json::array(
			 {Json{
				  {"id", "source"}, {"type", solid->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", solidInputs}
			  },
			  Json{{"id", "mirror"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}},
			  Json{
				  {"id", "sink"},
				  {"type", "Node_Project_Output"},
				  {"x", 0},
				  {"y", 0},
				  {"inputs", Json::array({Json{{"from_node", "mirror"}, {"from_index", 0}}})}
			  }}
		 )}
	};
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.22.10.201";
	archive.GraphJson = root.dump() + '\0';
	std::vector<std::byte> original;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, original, failure));
	REQUIRE(engine::bake::ReadPxcx(original, archive, failure));
	auto imported = Import(archive);
	REQUIRE(imported.Graph.Nodes[1].Type == "pc.mirror_polar");
	Diagnostic error;
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const std::array<PxcxEdit, 1> staticEdit{PxcxInputValueEdit{"mirror", "center", Vector2{.3, .4}}};
	std::vector<std::byte> refusedBytes{std::byte{0x7b}};
	CHECK_FALSE(WritePxcxEdits(imported, originalBytes, staticEdit, refusedBytes, error));
	CHECK(refusedBytes == std::vector<std::byte>{std::byte{0x7b}});
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);

	Plan plan;
	REQUIRE(Compile(imported.Graph, plan, error) == Status::Ok);
	auto replay = Bind(imported.Graph, error);
	EvaluationRequest request;
	request.GroupReplay = &replay;
	request.GroupAuthoringRevision = 1;
	Observer observer;
	request.SourceInputObserver = &observer;
	Image image;
	REQUIRE_FALSE(imported.Graph.Outputs.empty());
	REQUIRE(
		Evaluate(imported.Graph, plan, imported.Graph.Outputs.front().Id, request, image, error) == Status::Ok
	);
	REQUIRE_FALSE(observer.Inputs.empty());
	request.SourceInputObserver = nullptr;
	SourceAxisTransition combine{"mirror", "center", false};
	combine.ObservedInputs = observer.Inputs;
	combine.ObservedInputOwner = "mirror";
	Document combined;
	GroupReplayState changed;
	REQUIRE(
		ToggleSourceAxes(imported.Graph, replay, 1, combine, request, combined, changed, error) == Status::Ok
	);
	REQUIRE_FALSE(combined.Keyframes.empty());
	for (const auto &key : combined.Keyframes)
		if (key.NodeId == "mirror" && key.Port == "center")
			REQUIRE(std::holds_alternative<ArrayValue>(key.Data));
	Plan combinedPlan;
	REQUIRE(Compile(combined, combinedPlan, error) == Status::Ok);
	Observer expectedBetween;
	EvaluationRequest middle;
	middle.Tick = 5;
	middle.SourceInputObserver = &expectedBetween;
	REQUIRE(
		Evaluate(combined, combinedPlan, combined.Outputs.front().Id, middle, image, error) == Status::Ok
	);
	std::vector<std::byte> saved;
	const bool written = WritePxcxProjection(imported, combined, {}, saved, error);
	INFO(error.Message);
	REQUIRE(written);
	REQUIRE(engine::bake::ReadPxcx(saved, archive, failure));
	auto reopened = Import(archive);
	REQUIRE(Compile(reopened.Graph, plan, error) == Status::Ok);
	request.GroupReplay = nullptr;
	request.Tick = 5;
	Observer between;
	request.SourceInputObserver = &between;
	REQUIRE(
		Evaluate(reopened.Graph, plan, reopened.Graph.Outputs.front().Id, request, image, error) == Status::Ok
	);
	REQUIRE_FALSE(between.Inputs.empty());
	CHECK(between.Inputs == expectedBetween.Inputs);
	const auto json = Json::parse(std::string_view(archive.GraphJson.data(), archive.GraphJson.size() - 1));
	const auto &record = json["nodes"][1]["inputs"][center->SourceIndex];
	CHECK(record["future_input"] == "keep");
	CHECK(record["global_use"] == true);
	CHECK(record["global_key"] == "value + self.center");
	CHECK(record["animators"][2] == inputs[center->SourceIndex]["animators"][2]);
	CHECK(record["animators"][0] == inputs[center->SourceIndex]["animators"][0]);
	CHECK(record["animators"][1] == inputs[center->SourceIndex]["animators"][1]);
	auto restoredReplay = Bind(reopened.Graph, error);
	request.GroupReplay = &restoredReplay;
	request.SourceInputObserver = nullptr;
	SourceAxisTransition separate{"mirror", "center", true};
	Document separated;
	GroupReplayState finalReplay;
	REQUIRE(
		ToggleSourceAxes(
			reopened.Graph, restoredReplay, 1, separate, request, separated, finalReplay, error
		) == Status::Ok
	);
	REQUIRE(separated.Nodes[1].SourceSeparatedVec2Animators);
	CHECK(separated.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Separated);
}

TEST_CASE(
	"PXC solid Dimension Combine numeric pair keys survive projection and re-separation",
	"[pxcx_vec2_axes][source_axis_transition]"
) {
	using namespace pxcx_combined_tuple;
	const auto *entry = FindCatalogueEntry("pc.solid");
	REQUIRE(entry);
	const auto *center = FindCatalogueInput(*entry, "dimension");
	REQUIRE(center);
	Json inputs = Json::array();
	for (int32_t index = 0; index <= center->SourceIndex; ++index)
		inputs.push_back(Json::object());
	inputs[center->SourceIndex] = {
		{"anim", true},
		{"sep_axis", true},
		{"r", {{"d", Json::array({2, 3})}}},
		{"global_key", "value + self.dimension"},
		{"global_use", true},
		{"animators",
		 Json::array(
			 {Json::array({Row(0, 2), Row(10, 4)}),
			  Json::array({Row(0, 3), Row(10, 5)}),
			  Json{{"future_axis", "keep"}}}
		 )},
		{"future_input", "keep"}
	};
	const auto *solid = FindCatalogueEntry("pc.solid");
	REQUIRE(solid);
	const auto *dimension = FindCatalogueInput(*solid, "dimension");
	REQUIRE(dimension);
	Json solidInputs = Json::array();
	for (int32_t i = 0; i <= dimension->SourceIndex; ++i)
		solidInputs.push_back(Json::object());
	solidInputs[dimension->SourceIndex] = {{"r", {{"d", Json::array({1, 1})}}}};
	Json root = {
		{"animator", {{"frames_total", 11}, {"playback", 0}, {"framerate", 30}}},
		{"nodes",
		 Json::array(
			 {Json{
				  {"id", "source"}, {"type", solid->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", solidInputs}
			  },
			  Json{{"id", "mirror"}, {"type", entry->SourceNode}, {"x", 0}, {"y", 0}, {"inputs", inputs}},
			  Json{
				  {"id", "sink"},
				  {"type", "Node_Project_Output"},
				  {"x", 0},
				  {"y", 0},
				  {"inputs", Json::array({Json{{"from_node", "mirror"}, {"from_index", 0}}})}
			  }}
		 )}
	};
	engine::bake::PxcxArchive archive;
	archive.MetadataNumber = 121092;
	archive.MetadataText = "1.22.10.201";
	archive.GraphJson = root.dump() + '\0';
	std::vector<std::byte> original;
	std::string failure;
	REQUIRE(engine::bake::WritePxcx(archive, original, failure));
	REQUIRE(engine::bake::ReadPxcx(original, archive, failure));
	auto imported = Import(archive);
	REQUIRE(imported.Graph.Nodes[1].Type == "pc.solid");
	Diagnostic error;
	const auto originalGraph = imported.Graph;
	const auto originalBytes = imported.Source.OriginalBytes;
	const std::array<PxcxEdit, 1> staticEdit{PxcxInputValueEdit{"mirror", "dimension", Vector2{.3, .4}}};
	std::vector<std::byte> refusedBytes{std::byte{0x7b}};
	CHECK_FALSE(WritePxcxEdits(imported, originalBytes, staticEdit, refusedBytes, error));
	CHECK(refusedBytes == std::vector<std::byte>{std::byte{0x7b}});
	CHECK(imported.Graph == originalGraph);
	CHECK(imported.Source.OriginalBytes == originalBytes);

	Plan plan;
	REQUIRE(Compile(imported.Graph, plan, error) == Status::Ok);
	auto replay = Bind(imported.Graph, error);
	EvaluationRequest request;
	request.GroupReplay = &replay;
	request.GroupAuthoringRevision = 1;
	Observer observer;
	request.SourceInputObserver = &observer;
	Image image;
	REQUIRE_FALSE(imported.Graph.Outputs.empty());
	REQUIRE(
		Evaluate(imported.Graph, plan, imported.Graph.Outputs.front().Id, request, image, error) == Status::Ok
	);
	REQUIRE_FALSE(observer.Inputs.empty());
	request.SourceInputObserver = nullptr;
	SourceAxisTransition combine{"mirror", "dimension", false};
	combine.ObservedInputs = observer.Inputs;
	combine.ObservedInputOwner = "mirror";
	Document combined;
	GroupReplayState changed;
	REQUIRE(
		ToggleSourceAxes(imported.Graph, replay, 1, combine, request, combined, changed, error) == Status::Ok
	);
	REQUIRE_FALSE(combined.Keyframes.empty());
	for (const auto &key : combined.Keyframes)
		if (key.NodeId == "mirror" && key.Port == "dimension") {
			REQUIRE(std::holds_alternative<Vector2>(key.Data));
			const auto pair = std::get<Vector2>(key.Data);
			CHECK(std::isfinite(pair.X));
			CHECK(std::isfinite(pair.Y));
		}
	Plan combinedPlan;
	REQUIRE(Compile(combined, combinedPlan, error) == Status::Ok);
	Observer expectedBetween;
	EvaluationRequest middle;
	middle.Tick = 5;
	middle.SourceInputObserver = &expectedBetween;
	REQUIRE(
		Evaluate(combined, combinedPlan, combined.Outputs.front().Id, middle, image, error) == Status::Ok
	);
	std::vector<std::byte> saved;
	const bool written = WritePxcxProjection(imported, combined, {}, saved, error);
	INFO(error.Message);
	REQUIRE(written);
	REQUIRE(engine::bake::ReadPxcx(saved, archive, failure));
	auto reopened = Import(archive);
	REQUIRE(Compile(reopened.Graph, plan, error) == Status::Ok);
	request.GroupReplay = nullptr;
	request.Tick = 5;
	Observer between;
	request.SourceInputObserver = &between;
	REQUIRE(
		Evaluate(reopened.Graph, plan, reopened.Graph.Outputs.front().Id, request, image, error) == Status::Ok
	);
	REQUIRE_FALSE(between.Inputs.empty());
	CHECK(between.Inputs == expectedBetween.Inputs);
	const auto json = Json::parse(std::string_view(archive.GraphJson.data(), archive.GraphJson.size() - 1));
	const auto &record = json["nodes"][1]["inputs"][center->SourceIndex];
	CHECK(record["future_input"] == "keep");
	CHECK(record["global_use"] == true);
	CHECK(record["global_key"] == "value + self.dimension");
	CHECK(record["animators"][2] == inputs[center->SourceIndex]["animators"][2]);
	CHECK(record["animators"][0] == inputs[center->SourceIndex]["animators"][0]);
	CHECK(record["animators"][1] == inputs[center->SourceIndex]["animators"][1]);
	auto restoredReplay = Bind(reopened.Graph, error);
	request.GroupReplay = &restoredReplay;
	request.SourceInputObserver = nullptr;
	SourceAxisTransition separate{"mirror", "dimension", true};
	Document separated;
	GroupReplayState finalReplay;
	REQUIRE(
		ToggleSourceAxes(
			reopened.Graph, restoredReplay, 1, separate, request, separated, finalReplay, error
		) == Status::Ok
	);
	REQUIRE(separated.Nodes[1].SourceSeparatedVec2Animators);
	CHECK(separated.Nodes[1].SourceSeparatedVec2Animators->Inputs.front().Separated);
}
