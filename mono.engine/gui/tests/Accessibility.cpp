#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Accessibility.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_SUITE_ID("engine.gui.accessibility")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Store;
using namespace engine::gui;

TEST_CASE("semantic snapshot follows the visible compiled tree", "[gui][accessibility]") {
	RegisterGuiClasses();
	Store store("gui.accessibility");
	const auto root = store.CreateInstance(Classes::Find(Name("Instance")), "StarterGui");
	const auto collector = store.CreateInstance(GuiClass("ScreenGui"), "Screen");
	REQUIRE(store.SetParent(collector, root));
	store.Set(collector, Layer{});

	const auto button = store.CreateInstance(GuiClass("TextButton"), "Go");
	REQUIRE(store.SetParent(button, collector));
	Element element;
	element.Size = engine::core::UDim2{0.0f, 100.0f, 0.0f, 40.0f};
	store.Set(button, element);
	Label label;
	label.Text = "<b>Start game</b>";
	label.Rich = true;
	store.Set(button, label);

	const auto hidden = store.CreateInstance(GuiClass("TextLabel"), "Hidden");
	REQUIRE(store.SetParent(hidden, collector));
	element.Visible = false;
	store.Set(hidden, element);
	store.Set(hidden, label);

	const auto group = store.CreateInstance(GuiClass("Frame"), "Settings");
	REQUIRE(store.SetParent(group, collector));
	element.Visible = true;
	element.Active = true;
	element.Position = engine::core::UDim2{0.0f, 0.0f, 0.0f, 100.0f};
	store.Set(group, element);
	const auto child = store.CreateInstance(GuiClass("TextLabel"), "Hint");
	REQUIRE(store.SetParent(child, group));
	element.Active = false;
	element.Position = {};
	store.Set(child, element);
	store.Set(child, label);
	const auto field = store.CreateInstance(GuiClass("TextBox"), "Input");
	REQUIRE(store.SetParent(field, collector));
	element.Position = engine::core::UDim2{0.0f, 0.0f, 0.0f, 200.0f};
	store.Set(field, element);
	label.Text = "Typed value";
	label.Rich = false;
	store.Set(field, label);
	store.Set(field, Entry{});

	CompileRequest request;
	request.Display.Width = 800.0f;
	request.Display.Height = 600.0f;
	Compiled compiled;
	REQUIRE(compiled.Rebuild(store, request));
	const auto snapshot = CompileSemantics(store, compiled.Commands());
	const auto &nodes = snapshot.Nodes;
	CHECK_FALSE(snapshot.Truncated);
	REQUIRE(nodes.size() == 4);
	CHECK(nodes[0].Instance == button);
	CHECK(nodes[0].Collector == collector);
	CHECK(nodes[0].Role == SemanticRole::Button);
	CHECK(nodes[0].Name == "Start game");
	CHECK(nodes[0].Bounds.Max.X > nodes[0].Bounds.Min.X);
	CHECK(nodes[1].Instance == group);
	CHECK(nodes[1].Parent == engine::ecs::NULL_ENTITY);
	CHECK(nodes[2].Instance == child);
	CHECK(nodes[2].Parent == group);
	CHECK(nodes[3].Instance == field);
	CHECK(nodes[3].Role == SemanticRole::TextField);
	CHECK(nodes[3].Name == "Input");
	CHECK(nodes[3].Value == "Typed value");
	CHECK(
		nodes[3].Actions ==
		static_cast<SemanticActionSet>(
			static_cast<uint8_t>(SemanticActionSet::Focus) | static_cast<uint8_t>(SemanticActionSet::Edit)
		)
	);
	CHECK(nodes[3].Editable);
	CHECK(nodes[3].ReadingOrder == 3);
}

TEST_CASE("semantic snapshots expose viewer language and protect password text", "[gui][accessibility]") {
	RegisterGuiClasses();
	Store store("gui.accessibility.password");
	const auto box = store.CreateInstance(GuiClass("TextBox"), "Password");
	Element element;
	element.Size = engine::core::UDim2{0.0f, 200.0f, 0.0f, 50.0f};
	store.Set(box, element);
	Label label;
	label.Text = "never expose this";
	store.Set(box, label);
	Entry entry;
	entry.PlaceholderText = "Account password";
	entry.Password = true;
	store.Set(box, entry);

	DrawList list;
	DrawCommand command;
	command.Source = box;
	command.Bounds = {{0.0f, 0.0f}, {200.0f, 50.0f}};
	command.Clip = command.Bounds;
	list.Commands.push_back(command);

	const SemanticSnapshot snapshot = CompileSemantics(store, list, "en-AU");
	REQUIRE(snapshot.Nodes.size() == 1);
	const SemanticNode &node = snapshot.Nodes.front();
	CHECK(node.Language == "en-AU");
	CHECK(node.Description == "Account password");
	CHECK(node.Password);
	CHECK(node.Value.empty());
	CHECK(
		node.Actions ==
		static_cast<SemanticActionSet>(
			static_cast<uint8_t>(SemanticActionSet::Focus) | static_cast<uint8_t>(SemanticActionSet::Edit)
		)
	);
}

TEST_CASE("semantic enumeration reports its source ceiling", "[gui][accessibility][limit]") {
	Store store("gui.accessibility.limit");
	DrawList list;
	for (uint64_t index = 1; index <= 65537; ++index) {
		DrawCommand command;
		command.Source = engine::ecs::Entity(index);
		command.Bounds = {{0.0f, 0.0f}, {1.0f, 1.0f}};
		command.Clip = command.Bounds;
		list.Commands.push_back(std::move(command));
	}
	const auto snapshot = CompileSemantics(store, list);
	CHECK(snapshot.Truncated);
	CHECK(snapshot.Nodes.empty());
}

TEST_CASE("semantic names use compiled visible text", "[gui][accessibility]") {
	RegisterGuiClasses();
	Store store("gui.accessibility.painted_text");
	const auto labelEntity = store.CreateInstance(GuiClass("TextLabel"), "Message");
	store.Set(labelEntity, Element{});
	Label label;
	label.Text = "Source text";
	store.Set(labelEntity, label);

	DrawList list;
	DrawCommand background;
	background.Source = labelEntity;
	background.Bounds = {{0.0f, 0.0f}, {100.0f, 30.0f}};
	background.Clip = background.Bounds;
	list.Commands.push_back(background);
	DrawCommand text = background;
	text.Kind = DrawKind::Text;
	text.Text = "Visible translation";
	list.Commands.push_back(text);

	const auto snapshot = CompileSemantics(store, list);
	REQUIRE(snapshot.Nodes.size() == 1);
	CHECK(snapshot.Nodes[0].Name == "Visible translation");
}

TEST_CASE("semantic audit reports actionable control evidence per collector", "[gui][accessibility]") {
	SemanticSnapshot snapshot;
	const engine::ecs::Entity first(1);
	const engine::ecs::Entity second(2);
	const engine::ecs::Entity third(3);
	const engine::ecs::Entity collector(4);
	const engine::ecs::Entity otherCollector(5);
	SemanticNode button;
	button.Instance = first;
	button.Collector = collector;
	button.Role = SemanticRole::Button;
	button.Name = "Play";
	button.Bounds = {{0.0f, 0.0f}, {30.0f, 30.0f}};
	snapshot.Nodes.push_back(button);
	button.Instance = second;
	button.Bounds = {{40.0f, 0.0f}, {100.0f, 60.0f}};
	snapshot.Nodes.push_back(button);
	button.Instance = third;
	button.Collector = otherCollector;
	snapshot.Nodes.push_back(button);
	const SemanticAudit audit = AuditSemantics(snapshot);
	REQUIRE(audit.Issues.size() == 2);
	CHECK(audit.Issues[0].Kind == SemanticIssueKind::SmallTarget);
	CHECK(audit.Issues[0].Instance == first);
	CHECK(audit.Issues[1].Kind == SemanticIssueKind::DuplicateControlName);
	CHECK(audit.Issues[1].Instance == second);
}
