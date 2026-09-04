// The persistent relations behind the NodeCanvas GUI object.

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/NodeCanvas.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.gui.node_canvas")
TEST_DEPENDS("engine.gui.registration")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using namespace engine::gui;

namespace {
	struct GraphFixture {
		Store World{"gui_node_canvas"};
		Entity Canvas;

		GraphFixture() {
			RegisterGuiClasses();
			Canvas = World.CreateInstance(GuiClass("NodeCanvas"), "Graph");
		}

		Entity Node(const char *id) {
			return Node(Canvas, id);
		}

		Entity Node(Entity parent, const char *id) {
			const Entity node = World.CreateInstance(GuiClass("NodeCanvasNode"), id);
			REQUIRE(World.SetParent(node, parent));
			NodeCanvasNode state;
			state.Id = Name(id);
			state.Type = Name("test.Value");
			World.Set(node, state);
			return node;
		}

		Entity Port(Entity node, const char *id, NodePortDirection direction, const char *type = "number") {
			const Entity port = World.CreateInstance(GuiClass("NodeCanvasPort"), id);
			REQUIRE(World.SetParent(port, node));
			World.Set(port, NodeCanvasPort{Name(id), Name(type), direction});
			return port;
		}

		Entity Group(NodeGroupLayout layout) {
			const Entity group = World.CreateInstance(GuiClass("NodeCanvasGroup"), "Group");
			REQUIRE(World.SetParent(group, Canvas));
			NodeCanvasGroup state;
			state.Id = Name("group");
			state.Layout = layout;
			state.Padding = {10.0f, 10.0f};
			World.Set(group, state);
			return group;
		}
	};
}

TEST_CASE("a typed output links to an input in the same canvas", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity source = graph.Node("source");
	const Entity destination = graph.Node("destination");
	const Entity output = graph.Port(source, "value", NodePortDirection::Output);
	const Entity input = graph.Port(destination, "value", NodePortDirection::Input);

	Entity made;
	REQUIRE(ConnectNodePorts(graph.World, graph.Canvas, output, input, made) == NodeLinkResult::Made);
	REQUIRE(made != engine::ecs::NULL_ENTITY);

	const NodeCanvasLink *link = graph.World.Get<NodeCanvasLink>(made);
	REQUIRE(link != nullptr);
	CHECK(link->FromNode == Name("source"));
	CHECK(link->FromPort == Name("value"));
	CHECK(link->FromDirection == NodePortDirection::Output);
	CHECK(link->ToNode == Name("destination"));
	CHECK(link->ToPort == Name("value"));
	CHECK(link->ToDirection == NodePortDirection::Input);
	CHECK(link->LineColor.R == 0.75f);
	CHECK(link->LineTransparency == 0.0f);
	CHECK(link->LineThickness == 2.0f);
}

TEST_CASE("node links require direct, unique endpoint identities", "[gui][nodecanvas]") {
	SECTION("a port nested below a node is not an endpoint") {
		GraphFixture graph;
		const Entity source = graph.Node("source");
		const Entity destination = graph.Node("destination");
		const Entity wrapper = graph.World.CreateInstance(GuiClass("Frame"), "Wrapper");
		REQUIRE(graph.World.SetParent(wrapper, source));
		const Entity nestedOutput = graph.Port(wrapper, "out", NodePortDirection::Output);
		const Entity input = graph.Port(destination, "in", NodePortDirection::Input);

		Entity made;
		CHECK(
			ConnectNodePorts(graph.World, graph.Canvas, nestedOutput, input, made) == NodeLinkResult::NotAPort
		);
	}

	SECTION("duplicate node and port ids are rejected instead of resolving arbitrarily") {
		GraphFixture graph;
		const Entity source = graph.Node("source");
		const Entity destination = graph.Node("destination");
		const Entity output = graph.Port(source, "out", NodePortDirection::Output);
		const Entity input = graph.Port(destination, "in", NodePortDirection::Input);
		graph.Node("source");

		Entity made;
		CHECK(ConnectNodePorts(graph.World, graph.Canvas, output, input, made) == NodeLinkResult::MissingId);

		GraphFixture ports;
		const Entity portSource = ports.Node("source");
		const Entity portDestination = ports.Node("destination");
		const Entity portOutput = ports.Port(portSource, "out", NodePortDirection::Output);
		const Entity portInput = ports.Port(portDestination, "in", NodePortDirection::Input);
		ports.Port(portSource, "out", NodePortDirection::Output);
		CHECK(
			ConnectNodePorts(ports.World, ports.Canvas, portOutput, portInput, made) ==
			NodeLinkResult::MissingId
		);
	}
}

TEST_CASE("repeating an unlimited node link returns the existing wire", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity source = graph.Node("source");
	const Entity destination = graph.Node("destination");
	const Entity output = graph.Port(source, "out", NodePortDirection::Output);
	const Entity input = graph.Port(destination, "in", NodePortDirection::Input);
	NodeCanvasPort state = *graph.World.Get<NodeCanvasPort>(input);
	state.MaxConnections = 0;
	graph.World.Set(input, state);

	Entity first;
	Entity repeated;
	REQUIRE(ConnectNodePorts(graph.World, graph.Canvas, output, input, first) == NodeLinkResult::Made);
	REQUIRE(ConnectNodePorts(graph.World, graph.Canvas, output, input, repeated) == NodeLinkResult::Made);
	CHECK(repeated == first);

	std::vector<Entity> links;
	CHECK(NodeCanvasLinks(graph.World, graph.Canvas, links) == 1);
}

TEST_CASE("node links refuse incompatible directions, types, and cycles", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity first = graph.Node("first");
	const Entity second = graph.Node("second");
	const Entity third = graph.Node("third");
	const Entity firstOutput = graph.Port(first, "out", NodePortDirection::Output);
	const Entity secondInput = graph.Port(second, "in", NodePortDirection::Input);
	const Entity secondOutput = graph.Port(second, "out", NodePortDirection::Output);
	const Entity thirdInput = graph.Port(third, "in", NodePortDirection::Input);
	const Entity wrongType = graph.Port(third, "text", NodePortDirection::Input, "text");
	const Entity firstInput = graph.Port(first, "in", NodePortDirection::Input);

	Entity made;
	CHECK(
		ConnectNodePorts(graph.World, graph.Canvas, secondInput, firstOutput, made) ==
		NodeLinkResult::WrongDirection
	);
	CHECK(
		ConnectNodePorts(graph.World, graph.Canvas, firstOutput, wrongType, made) ==
		NodeLinkResult::TypeMismatch
	);
	REQUIRE(
		ConnectNodePorts(graph.World, graph.Canvas, firstOutput, secondInput, made) == NodeLinkResult::Made
	);
	REQUIRE(
		ConnectNodePorts(graph.World, graph.Canvas, secondOutput, thirdInput, made) == NodeLinkResult::Made
	);
	CHECK(
		ConnectNodePorts(graph.World, graph.Canvas, secondOutput, firstInput, made) ==
		NodeLinkResult::WouldCycle
	);
}

TEST_CASE("node links accept any at either typed boundary", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity numberSource = graph.Node("number_source");
	const Entity anySource = graph.Node("any_source");
	const Entity anyDestination = graph.Node("any_destination");
	const Entity numberDestination = graph.Node("number_destination");
	const Entity stringDestination = graph.Node("string_destination");
	const Entity numberOutput = graph.Port(numberSource, "number", NodePortDirection::Output, "number");
	const Entity anyOutput = graph.Port(anySource, "any", NodePortDirection::Output, "any");
	const Entity anyInput = graph.Port(anyDestination, "any", NodePortDirection::Input, "any");
	const Entity numberInput = graph.Port(numberDestination, "number", NodePortDirection::Input, "number");
	const Entity stringInput = graph.Port(stringDestination, "string", NodePortDirection::Input, "string");

	CHECK(NodeCanvasTypesCompatible(Name("number"), Name("any")));
	CHECK(NodeCanvasTypesCompatible(Name("any"), Name("number")));
	CHECK_FALSE(NodeCanvasTypesCompatible(Name("number"), Name("string")));

	Entity made;
	CHECK(ConnectNodePorts(graph.World, graph.Canvas, numberOutput, anyInput, made) == NodeLinkResult::Made);
	CHECK(ConnectNodePorts(graph.World, graph.Canvas, anyOutput, numberInput, made) == NodeLinkResult::Made);
	CHECK(
		ConnectNodePorts(graph.World, graph.Canvas, numberOutput, stringInput, made) ==
		NodeLinkResult::TypeMismatch
	);
}

TEST_CASE("a new wire replaces the previous input wire", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity left = graph.Node("left");
	const Entity right = graph.Node("right");
	const Entity destination = graph.Node("destination");
	const Entity leftOutput = graph.Port(left, "out", NodePortDirection::Output);
	const Entity rightOutput = graph.Port(right, "out", NodePortDirection::Output);
	const Entity input = graph.Port(destination, "in", NodePortDirection::Input);

	Entity first;
	Entity second;
	REQUIRE(ConnectNodePorts(graph.World, graph.Canvas, leftOutput, input, first) == NodeLinkResult::Made);
	REQUIRE(ConnectNodePorts(graph.World, graph.Canvas, rightOutput, input, second) == NodeLinkResult::Made);

	std::vector<Entity> links;
	REQUIRE(NodeCanvasLinks(graph.World, graph.Canvas, links) == 1);
	CHECK(links.front() == second);
	CHECK(DisconnectNodeInput(graph.World, graph.Canvas, input));
	CHECK(NodeCanvasLinks(graph.World, graph.Canvas, links) == 0);
}

TEST_CASE("an input observes its declared connection limit", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity first = graph.Node("first");
	const Entity second = graph.Node("second");
	const Entity third = graph.Node("third");
	const Entity destination = graph.Node("destination");
	const Entity firstOutput = graph.Port(first, "out", NodePortDirection::Output);
	const Entity secondOutput = graph.Port(second, "out", NodePortDirection::Output);
	const Entity thirdOutput = graph.Port(third, "out", NodePortDirection::Output);
	const Entity input = graph.Port(destination, "in", NodePortDirection::Input);
	NodeCanvasPort inputState = *graph.World.Get<NodeCanvasPort>(input);
	inputState.MaxConnections = 2;
	graph.World.Set(input, inputState);

	Entity made;
	CHECK(ConnectNodePorts(graph.World, graph.Canvas, firstOutput, input, made) == NodeLinkResult::Made);
	CHECK(ConnectNodePorts(graph.World, graph.Canvas, secondOutput, input, made) == NodeLinkResult::Made);
	CHECK(ConnectNodePorts(graph.World, graph.Canvas, thirdOutput, input, made) == NodeLinkResult::InputFull);

	std::vector<Entity> links;
	CHECK(NodeCanvasLinks(graph.World, graph.Canvas, links) == 2);

	inputState.MaxConnections = 0;
	graph.World.Set(input, inputState);
	CHECK(ConnectNodePorts(graph.World, graph.Canvas, thirdOutput, input, made) == NodeLinkResult::Made);
	CHECK(NodeCanvasLinks(graph.World, graph.Canvas, links) == 3);
}

TEST_CASE("a negative input limit is refused", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity source = graph.Node("source");
	const Entity destination = graph.Node("destination");
	const Entity output = graph.Port(source, "out", NodePortDirection::Output);
	const Entity input = graph.Port(destination, "in", NodePortDirection::Input);
	NodeCanvasPort state = *graph.World.Get<NodeCanvasPort>(input);
	state.MaxConnections = -1;
	graph.World.Set(input, state);

	Entity made;
	CHECK(
		ConnectNodePorts(graph.World, graph.Canvas, output, input, made) ==
		NodeLinkResult::InvalidConnectionLimit
	);
	CHECK(
		std::string_view(Describe(NodeLinkResult::InvalidConnectionLimit)) ==
		"input connection limit cannot be negative"
	);
}

TEST_CASE("a bypass declares a locally compatible input-to-output mapping", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity node = graph.Node("pass_through");
	graph.Port(node, "value", NodePortDirection::Input, "number");
	graph.Port(node, "result", NodePortDirection::Output, "number");
	NodeCanvasNode state = *graph.World.Get<NodeCanvasNode>(node);
	state.Enabled = false;
	state.BypassMode = NodeBypassMode::Bypass;
	state.BypassInput = Name("value");
	state.BypassOutput = Name("result");
	graph.World.Set(node, state);

	CHECK(ValidateNodeCanvasBypass(graph.World, node) == NodeBypassResult::Valid);
	CHECK_FALSE(graph.World.Get<NodeCanvasNode>(node)->Enabled);

	state.BypassOutput = Name("value");
	graph.World.Set(node, state);
	CHECK(ValidateNodeCanvasBypass(graph.World, node) == NodeBypassResult::WrongDirection);

	state.BypassOutput = Name("missing");
	graph.World.Set(node, state);
	CHECK(ValidateNodeCanvasBypass(graph.World, node) == NodeBypassResult::MissingPort);

	graph.Port(node, "text", NodePortDirection::Output, "string");
	state.BypassOutput = Name("text");
	graph.World.Set(node, state);
	CHECK(ValidateNodeCanvasBypass(graph.World, node) == NodeBypassResult::TypeMismatch);

	state.BypassMode = NodeBypassMode::None;
	graph.World.Set(node, state);
	CHECK(ValidateNodeCanvasBypass(graph.World, node) == NodeBypassResult::NotBypassing);
}

TEST_CASE("a node group keeps its children in place while fitting their edge", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity group = graph.Group(NodeGroupLayout::AroundEdge);
	Element groupElement = *graph.World.Get<Element>(group);
	groupElement.Position = {0.0f, 100.0f, 0.0f, 50.0f};
	graph.World.Set(group, groupElement);

	const Entity first = graph.Node(group, "first");
	Element firstElement = *graph.World.Get<Element>(first);
	firstElement.Position = {0.0f, 20.0f, 0.0f, 30.0f};
	firstElement.Size = {0.0f, 80.0f, 0.0f, 40.0f};
	graph.World.Set(first, firstElement);
	const Entity second = graph.Node(group, "second");
	Element secondElement = *graph.World.Get<Element>(second);
	secondElement.Position = {0.0f, 150.0f, 0.0f, 110.0f};
	secondElement.Size = {0.0f, 50.0f, 0.0f, 20.0f};
	graph.World.Set(second, secondElement);

	REQUIRE(LayoutNodeCanvasGroups(graph.World, graph.Canvas) == 1);
	const Element *fittedGroup = graph.World.Get<Element>(group);
	REQUIRE(fittedGroup != nullptr);
	CHECK(fittedGroup->Position.X.Offset == 110.0f);
	CHECK(fittedGroup->Position.Y.Offset == 70.0f);
	CHECK(fittedGroup->Size.X.Offset == 200.0f);
	CHECK(fittedGroup->Size.Y.Offset == 120.0f);
	CHECK(graph.World.Get<Element>(first)->Position.X.Offset == 10.0f);
	CHECK(graph.World.Get<Element>(first)->Position.Y.Offset == 10.0f);
	CHECK(graph.World.Get<Element>(second)->Position.X.Offset == 140.0f);
	CHECK(graph.World.Get<Element>(second)->Position.Y.Offset == 90.0f);
}

TEST_CASE("a compact node group uses its children's smallest rectangle", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity group = graph.Group(NodeGroupLayout::SmallestSpace);
	const Entity node = graph.Node(group, "inside");
	Element element = *graph.World.Get<Element>(node);
	element.Position = {0.0f, 30.0f, 0.0f, 40.0f};
	element.Size = {0.0f, 80.0f, 0.0f, 60.0f};
	graph.World.Set(node, element);

	REQUIRE(LayoutNodeCanvasGroups(graph.World, graph.Canvas) == 1);
	const Element *fittedGroup = graph.World.Get<Element>(group);
	REQUIRE(fittedGroup != nullptr);
	CHECK(fittedGroup->Position.X.Offset == 30.0f);
	CHECK(fittedGroup->Position.Y.Offset == 40.0f);
	CHECK(fittedGroup->Size.X.Offset == 80.0f);
	CHECK(fittedGroup->Size.Y.Offset == 60.0f);
	CHECK(graph.World.Get<Element>(node)->Position.X.Offset == 0.0f);
	CHECK(graph.World.Get<Element>(node)->Position.Y.Offset == 0.0f);
}

TEST_CASE("a node separates or squashes input ports by their requested edge", "[gui][nodecanvas]") {
	GraphFixture graph;
	const Entity node = graph.Node("target");
	NodeCanvasNode nodeState = *graph.World.Get<NodeCanvasNode>(node);
	nodeState.InputLayout = InputPortLayout::Separate;
	graph.World.Set(node, nodeState);
	Element nodeElement = *graph.World.Get<Element>(node);
	nodeElement.Size = {0.0f, 100.0f, 0.0f, 60.0f};
	graph.World.Set(node, nodeElement);

	const Entity firstTop = graph.Port(node, "top_a", NodePortDirection::Input);
	const Entity secondTop = graph.Port(node, "top_b", NodePortDirection::Input);
	const Entity bottom = graph.Port(node, "bottom", NodePortDirection::Input);
	const Entity corner = graph.Port(node, "corner", NodePortDirection::Input);
	for (const Entity port : {firstTop, secondTop, bottom, corner}) {
		Element element = *graph.World.Get<Element>(port);
		element.Size = {0.0f, 10.0f, 0.0f, 10.0f};
		graph.World.Set(port, element);
	}
	NodeCanvasPort bottomState = *graph.World.Get<NodeCanvasPort>(bottom);
	bottomState.Edge = NodePortEdge::Bottom;
	graph.World.Set(bottom, bottomState);
	NodeCanvasPort cornerState = *graph.World.Get<NodeCanvasPort>(corner);
	cornerState.Edge = NodePortEdge::Corner;
	graph.World.Set(corner, cornerState);

	REQUIRE(LayoutNodeCanvasPorts(graph.World, graph.Canvas) == 4);
	CHECK(graph.World.Get<Element>(firstTop)->Position.X.Offset == 30.0f);
	CHECK(graph.World.Get<Element>(secondTop)->Position.X.Offset == 60.0f);
	CHECK(graph.World.Get<Element>(bottom)->Position.X.Offset == 45.0f);
	CHECK(graph.World.Get<Element>(bottom)->Position.Y.Offset == 50.0f);
	CHECK(graph.World.Get<Element>(corner)->Position.X.Offset == 0.0f);
	CHECK(graph.World.Get<Element>(corner)->Position.Y.Offset == 25.0f);

	nodeState.InputLayout = InputPortLayout::Squash;
	graph.World.Set(node, nodeState);
	REQUIRE(LayoutNodeCanvasPorts(graph.World, graph.Canvas) == 4);
	CHECK(graph.World.Get<Element>(firstTop)->Position.X.Offset == 8.0f);
	CHECK(graph.World.Get<Element>(secondTop)->Position.X.Offset == 20.0f);
	CHECK(graph.World.Get<Element>(corner)->Position.Y.Offset == 8.0f);
}
