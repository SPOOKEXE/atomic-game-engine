#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nodegraph/Editor.hpp>
#include <string>
#include <studio/ImageGraph.hpp>
#include <utility>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.path_bake_persistence")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.source_path_bake")

using namespace engine::imagegraph;

namespace {
	std::array<EvaluatedValue, 2> Evaluate(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		INFO(diagnostic.Message << " " << diagnostic.NodeId << ":" << diagnostic.Port);
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		EvaluatedValue segments, position;
		REQUIRE(EvaluateValue(document, plan, "baked-segments", {}, segments, diagnostic) == Status::Ok);
		REQUIRE(EvaluateValue(document, plan, "sampled-position", {}, position, diagnostic) == Status::Ok);
		return {std::move(segments), std::move(position)};
	}

	std::vector<Vector3> BakedSamples(const ArrayValue &segments) {
		std::vector<Vector3> points;
		for (const auto &sample : segments.Items) {
			const auto *coordinates = std::get_if<std::vector<SourceArrayItem>>(&sample.Data);
			REQUIRE(coordinates);
			REQUIRE(coordinates->size() == 3);
			std::array<double, 3> components{};
			for (size_t index = 0; index < components.size(); ++index) {
				const auto *element = std::get_if<ElementValue>(&(*coordinates)[index].Data);
				REQUIRE(element);
				const auto *number = std::get_if<double>(element);
				REQUIRE(number);
				components[index] = *number;
			}
			points.push_back({components[0], components[1], components[2]});
		}
		return points;
	}

	Document BakeGraph() {
		Path2D path;
		path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, 7}, {1, 9}};

		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"bake",
			 "pc.path_bake",
			 "",
			 {0, 0},
			 {{"path", path}, {"sample_type", EnumValue{0}}, {"segment_length", 4.}},
			 {}},
			{"sample", "pc.path_sample", "", {180, 0}, {{"ratio", .5}}, {}}
		};
		document.Outputs = {
			{"baked-segments", "bake", "segments"}, {"sampled-position", "sample", "position"}
		};
		return document;
	}
}

TEST_CASE(
	"Studio canvas save and native reopen preserve composed Path Bake evaluation",
	"[studio][imagegraph][path_bake]"
) {
	const Document source = BakeGraph();
	nodegraph::Graph canvas;
	studio::ImageGraphCanvasIds ids;
	std::string error;
	REQUIRE(studio::LoadImageGraphCanvas(source, canvas, ids, error));
	const auto bake = ids.ToCanvas.at("bake");
	const auto sample = ids.ToCanvas.at("sample");
	CHECK(canvas.Connect(bake, "path", sample, "path") == nodegraph::LinkResult::Made);

	Document canvasSaved;
	REQUIRE(studio::SaveImageGraphCanvas(canvas, source, ids, canvasSaved, error));
	REQUIRE(canvasSaved.Links == std::vector<Link>{{"bake", "path", "sample", "path"}});

	Document reopened;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(canvasSaved), reopened, diagnostic) == Status::Ok);
	CHECK(reopened == canvasSaved);

	const auto before = Evaluate(canvasSaved);
	const auto after = Evaluate(reopened);
	CHECK(before == after);
	const auto &segments = std::get<ArrayValue>(after[0].Data);
	CHECK(BakedSamples(segments) == std::vector<Vector3>{{0, 0, 0}, {4, 0, .4}, {8, 0, .8}});
	CHECK(std::get<Vector2>(after[1].Data) == Vector2{4, 0});
}
