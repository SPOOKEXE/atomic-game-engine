#include "Fixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.angview.layout")
TEST_DEPENDS("engine.angview.theme")

using namespace engine::angview;
using engine::core::Vector2;

TEST_CASE("nodes size to their ports and ports sit on the edges", "[angview][layout]") {
	const auto registry = tests::MakeRegistry();
	engine::ang::Graph graph;
	tests::Add(graph, "add", "math.add", {100.0f, 50.0f});
	Layout layout;
	layout.Build(graph, registry, Theme::Dark());
	const NodeLayout *node = layout.Find("add");
	REQUIRE(node != nullptr);
	CHECK(node->Bounds.Max.X == 260.0f);
	CHECK(node->Bounds.Max.Y == 50.0f + 24.0f + 12.0f + 40.0f);
	REQUIRE(node->Inputs.size() == 2);
	CHECK(node->Inputs[1].Centre.Y == tests::InputPort({100.0f, 50.0f}, 1).Y);
	CHECK(node->Outputs[0].Centre.X == 260.0f);
	CHECK(node->Find("b", engine::ang::Direction::Input) != nullptr);
	CHECK(node->Find("b", engine::ang::Direction::Output) == nullptr);
	CHECK(node->Title == "Add");
}

TEST_CASE("unknown nodes keep the ports their links name", "[angview][layout]") {
	const auto registry = tests::MakeRegistry();
	engine::ang::Graph graph;
	tests::Add(graph, "ghost", "plugin.gone", {});
	tests::Add(graph, "add", "math.add", {300.0f, 0.0f});
	REQUIRE(graph.Apply(engine::ang::edit::AddLink{{"ghost", "out", "add", "a"}}).Ok);
	Layout layout;
	layout.Build(graph, registry, Theme::Dark());
	const NodeLayout *ghost = layout.Find("ghost");
	REQUIRE(ghost != nullptr);
	CHECK(!ghost->Known);
	REQUIRE(ghost->Outputs.size() == 1);
	CHECK(ghost->Outputs[0].Name == "out");
}

TEST_CASE("a drag preview offsets only the moving nodes and bounds cover them", "[angview][layout]") {
	const auto registry = tests::MakeRegistry();
	engine::ang::Graph graph;
	tests::Add(graph, "a", "math.number", {});
	tests::Add(graph, "b", "math.number", {0.0f, 200.0f});
	Layout layout;
	layout.Build(graph, registry, Theme::Dark(), {"a"}, {50.0f, 0.0f});
	CHECK(layout.Find("a")->Bounds.Min.X == 50.0f);
	CHECK(layout.Find("b")->Bounds.Min.X == 0.0f);
	const std::vector<std::string> only{"b"};
	CHECK(layout.Bounds(&only).Min.Y == 200.0f);
	CHECK(layout.Bounds().Min.Y == 0.0f);
}

TEST_CASE("wire distance follows each style", "[angview][layout]") {
	const WireCurve curve = CurveBetween({0.0f, 0.0f}, {200.0f, 100.0f}, 1.0f);
	CHECK(DistanceToWire(curve, WireStyle::Bezier, {0.0f, 0.0f}) < 0.5f);
	CHECK(DistanceToWire(curve, WireStyle::Bezier, {200.0f, 100.0f}) < 0.5f);
	CHECK(DistanceToWire(curve, WireStyle::Straight, {100.0f, 50.0f}) < 0.01f);
	// The orthogonal path bends at x = 100.
	CHECK(DistanceToWire(curve, WireStyle::Orthogonal, {100.0f, 50.0f}) < 0.01f);
	CHECK(DistanceToWire(curve, WireStyle::Orthogonal, {50.0f, 50.0f}) > 40.0f);
}
