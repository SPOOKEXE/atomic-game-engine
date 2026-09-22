#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Accessibility.hpp>

TEST_SUITE_ID("client.accessibility")

TEST_CASE(
	"accessibility actions remain bound to the current store and collector", "[client][accessibility]"
) {
	engine::gui::DrawList list;
	engine::gui::DrawCommand command;
	command.Source = engine::ecs::Entity(11);
	command.Collector = engine::ecs::Entity(22);
	list.Commands.push_back(command);

	client::AccessibilityAction action{
		.Target = engine::ecs::Entity(11),
		.Collector = engine::ecs::Entity(22),
		.WorldEpoch = 101,
	};
	CHECK(client::IsCurrentAccessibilityAction(action, 101, list));

	action.WorldEpoch = 102;
	CHECK_FALSE(client::IsCurrentAccessibilityAction(action, 101, list));
	action.WorldEpoch = 101;
	action.Collector = engine::ecs::Entity(23);
	CHECK_FALSE(client::IsCurrentAccessibilityAction(action, 101, list));
}

TEST_CASE(
	"accessibility projection maps screen collectors and omits spatial collectors", "[client][accessibility]"
) {
	engine::gui::DrawList list;
	engine::gui::DrawCommand screen;
	screen.Collector = engine::ecs::Entity(22);
	list.Commands.push_back(screen);
	engine::gui::DrawCommand spatial;
	spatial.Collector = engine::ecs::Entity(33);
	spatial.Spatial = true;
	list.Commands.push_back(spatial);
	list.Transforms.push_back({
		.Collector = engine::ecs::Entity(22),
		.Origin = {10.0f, 20.0f},
		.Scale = {2.0f, 3.0f},
	});
	engine::gui::SemanticSnapshot snapshot;
	engine::gui::SemanticNode screenNode;
	screenNode.Instance = engine::ecs::Entity(11);
	screenNode.Collector = engine::ecs::Entity(22);
	screenNode.Bounds = {{1.0f, 2.0f}, {4.0f, 5.0f}};
	snapshot.Nodes.push_back(screenNode);
	engine::gui::SemanticNode spatialNode;
	spatialNode.Instance = engine::ecs::Entity(12);
	spatialNode.Collector = engine::ecs::Entity(33);
	spatialNode.Bounds = {{1.0f, 2.0f}, {4.0f, 5.0f}};
	snapshot.Nodes.push_back(spatialNode);

	const engine::gui::SemanticSnapshot projected = client::ProjectAccessibilitySnapshot(snapshot, list);
	REQUIRE(projected.Nodes.size() == 1);
	CHECK(projected.Nodes.front().Bounds.Min.X == 12.0f);
	CHECK(projected.Nodes.front().Bounds.Min.Y == 26.0f);
	CHECK(projected.Nodes.front().Bounds.Max.X == 18.0f);
	CHECK(projected.Nodes.front().Bounds.Max.Y == 35.0f);
}
