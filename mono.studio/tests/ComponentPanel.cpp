// The Components panel checked through a real, backend-free ImGui frame.
//
// A context needs no window or GPU. This lets the test drive the same checkbox
// a person clicks in Studio and then read the runtime component from its store.

#include "PropertyWidgets.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <string_view>
#include <studio/Editor.hpp>
#include <vector>

TEST_SUITE_ID("studio.component-panel")
TEST_DEPENDS("engine.scene.part")

using engine::core::Name;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::FieldDescriptor;
using engine::ecs::FieldSpec;
using engine::ecs::NULL_ENTITY;
using engine::ecs::PropertyType;
using engine::ecs::Schema;
using engine::ecs::Schemas;
using engine::ecs::Store;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace studio {
	struct ComponentPanelProbe {
		static void Filter(Editor &editor, std::string filter) {
			editor.ComponentFilter = std::move(filter);
		}
		static void Properties(Editor &editor) {
			editor.PropertyFilter = "Transparency";
			editor.DrawProperties();
		}
		static void Draw(Editor &editor) {
			editor.DrawComponents();
		}
		static void
		SeedSurface(Editor &editor, WorldId world, Entity entity, const engine::core::CFrame &before) {
			editor.SurfaceDragging.Active = true;
			editor.SurfaceDragging.Moved = true;
			editor.SurfaceDragging.World = world;
			editor.SurfaceDragging.Instances = {entity};
			editor.SurfaceDragging.Before = {before};
			editor.SurfaceGesture.Active = true;
			editor.SurfaceGesture.World = world;
			editor.BoxSelection.Active = true;
		}
		static void DrawOverlays(Editor &editor) {
			editor.DrawViewportOverlays();
		}
		static bool SurfaceCleared(const Editor &editor) {
			return !editor.SurfaceDragging.Active && !editor.SurfaceGesture.Active &&
				   !editor.BoxSelection.Active;
		}
	};
}

namespace {
	class Context {
	  public:
		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	class Jobs {
	  public:
		Jobs() {
			engine::parallel::Jobs::Start(1);
		}

		~Jobs() {
			engine::parallel::Jobs::Stop();
		}

		Jobs(const Jobs &) = delete;
		Jobs &operator=(const Jobs &) = delete;
	};

	struct Mouse {
		float X = -1.0f;
		float Y = -1.0f;
		bool Down = false;
	};

	void Frame(studio::Editor &editor, const Mouse &mouse = {}) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.X, mouse.Y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouse.Down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
		studio::ComponentPanelProbe::Draw(editor);
		ImGui::Render();
	}

	bool ReadEnabled(const studio::Editor &editor, WorldId world, Entity instance, ComponentId component) {
		bool enabled = false;
		editor.Universe->Enter(world, [&](Store &store) {
			const Schema *schema = Schemas::Of(component);
			REQUIRE(schema != nullptr);
			const FieldDescriptor *field = schema->Find("Enabled");
			REQUIRE(field != nullptr);
			const void *value = store.GetComponent(instance, component);
			REQUIRE(value != nullptr);
			alignas(8) std::array<std::byte, 8> scratch{};
			const void *read = Schemas::ReadField(value, *field, scratch.data());
			REQUIRE(read != nullptr);
			enabled = *static_cast<const bool *>(read);
		});
		return enabled;
	}
}

TEST_CASE("a removed viewport restores and cancels a surface gesture", "[studio][components][viewport]") {
	Context context;
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	engine::scene::RegisterSceneComponents();
	const WorldId world = editor.Universe->Create(WorldSettings{.Name = Name("gesture")});
	Entity entity;
	const engine::core::CFrame before(engine::core::Vector3{1.0f, 2.0f, 3.0f});
	editor.Universe->Enter(world, [&](Store &store) {
		entity = store.Create();
		store.Set<engine::scene::Transform>(entity, engine::scene::Transform{before});
		store.GetMutable<engine::scene::Transform>(entity)->Frame =
			engine::core::CFrame(engine::core::Vector3{9.0f, 2.0f, 3.0f});
	});
	studio::ComponentPanelProbe::SeedSurface(editor, world, entity, before);
	ImGui::NewFrame();
	studio::ComponentPanelProbe::DrawOverlays(editor);
	ImGui::EndFrame();
	CHECK(studio::ComponentPanelProbe::SurfaceCleared(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(store.Get<engine::scene::Transform>(entity)->Frame.Position == before.Position);
	});
}

TEST_CASE("the Components panel shows metadata and edits exposed values", "[studio][components]") {
	Context context;
	Jobs jobs;

	const std::string componentName = "studio.component-panel.test.settings";
	const FieldSpec fields[] = {
		{"Enabled", PropertyType::Bool},
		{"Count", PropertyType::Int32},
	};
	const Schemas::Result registered = Schemas::Register(componentName, fields);
	REQUIRE(registered.Why == Schemas::Status::Ok);
	const ComponentId component = registered.Id;
	const std::string_view componentTags[]{"experiment"};
	const std::string_view fieldTags[]{"constant"};
	REQUIRE(Schemas::SetTags(component, componentTags));
	REQUIRE(Schemas::SetFieldTags(component, Name("Enabled"), fieldTags));
	REQUIRE(Schemas::SetFieldExposed(component, Name("Enabled"), true));

	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();

	WorldSettings settings;
	settings.Name = Name("ComponentPanelScene");
	const WorldId world = editor.Universe->Create(settings);
	REQUIRE(world.IsValid());

	Entity selected;
	editor.Universe->Enter(world, [&](Store &store) {
		selected = store.CreateInstance(engine::scene::PartClass(), "SelectedPart");
		const Schema *schema = Schemas::Of(component);
		REQUIRE(schema != nullptr);
		const auto &descriptor = Components::Describe(component);
		std::vector<std::byte> value(schema->Size());
		descriptor.DefaultConstruct(value.data(), 1);
		store.SetComponent(selected, component, value.data());
		descriptor.Destruct(value.data(), 1);
	});
	REQUIRE(selected != NULL_ENTITY);

	editor.SelectionWorld = world;
	editor.Selection = {selected};
	editor.ShowComponents = true;

	ImGui::NewFrame();
	ImGui::LogToBuffer();
	ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
	studio::ComponentPanelProbe::Draw(editor);
	const std::string log = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();

	CHECK(log.find("Exposed Configs") != std::string::npos);
	CHECK(log.find(componentName + ".Enabled") != std::string::npos);
	CHECK(log.find("Count") != std::string::npos);
	CHECK(log.find("[experiment]") != std::string::npos);
	CHECK(log.find("[constant]") != std::string::npos);
	CHECK_FALSE(ReadEnabled(editor, world, selected, component));

	bool changed = false;
	for (float y = 85.0f; y <= 145.0f && !changed; y += 4.0f) {
		for (float x = 260.0f; x <= 350.0f && !changed; x += 6.0f) {
			Frame(editor, Mouse{.X = x, .Y = y});
			Frame(editor, Mouse{.X = x, .Y = y, .Down = true});
			Frame(editor, Mouse{.X = x, .Y = y});
			changed = ReadEnabled(editor, world, selected, component);
		}
	}

	CHECK(changed);
}

TEST_CASE("native component edits survive structural changes and support undo", "[studio][components]") {
	Context context;
	Jobs jobs;
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	engine::scene::RegisterSceneClasses();
	WorldSettings settings;
	settings.Name = Name("NativeComponentPanel");
	const WorldId world = editor.Universe->Create(settings);
	Entity selected;
	editor.Universe->Enter(world, [&](Store &store) {
		selected = store.CreateInstance(engine::scene::PartClass(), "SelectedPart");
	});
	editor.SelectionWorld = world;
	editor.Selection = {selected};
	editor.ShowComponents = true;
	studio::ComponentPanelProbe::Filter(
		editor, std::string(Components::Describe(Components::Of<engine::scene::Simulated>()).Name.Text())
	);
	const auto anchored = [&] {
		bool result = false;
		editor.Universe->Enter(world, [&](Store &store) {
			result = !store.Has<engine::scene::Simulated>(selected);
		});
		return result;
	};
	const auto toggle = [&] {
		const bool before = anchored();
		Frame(editor);
		Frame(editor);
		const auto component = Components::Of<engine::scene::Simulated>();
		const auto window = ImGui::FindWindowByName("Components");
		REQUIRE(window != nullptr);
		const ImGuiID componentId = ImHashData(&component.Index, sizeof(component.Index), window->ID);
		const ImGuiID tableId = ImHashStr("##native-properties", 0, componentId);
		const ImGuiTable *table = GImGui->Tables.GetByKey(tableId);
		REQUIRE(table != nullptr);
		const float x = table->Columns[1].WorkMinX + 5;
		const float y = table->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f;
		Frame(editor, Mouse{.X = x, .Y = y});
		Frame(editor, Mouse{.X = x, .Y = y, .Down = true});
		Frame(editor, Mouse{.X = x, .Y = y});
		REQUIRE(anchored() != before);
	};
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
	studio::ComponentPanelProbe::Draw(editor);
	const std::string panelText = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	INFO(panelText);
	const bool initial = anchored();
	toggle();
	REQUIRE(editor.Commands->CanUndo());
	REQUIRE(editor.Commands->Undo());
	CHECK(anchored() == initial);
	REQUIRE(editor.Commands->Redo());
	CHECK(anchored() != initial);
	toggle();
	CHECK(anchored() == initial);
}

namespace {
	struct ValueWidget {
		Store World{"value_widget"};
		FieldDescriptor Field;
		engine::game::PropertyValue Value;
		std::string Draft;
		ImVec2 Minimum;
		ImVec2 Maximum;
		bool Changed = false;

		void Frame() {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
			ImGui::Begin("Value");
			ImGui::SetNextItemWidth(400);
			Changed |= studio::DrawSchemaValue(World, Field, Value, Draft);
			Minimum = ImGui::GetItemRectMin();
			Maximum = ImGui::GetItemRectMax();
			ImGui::End();
			ImGui::Render();
		}

		void Click(ImVec2 point) {
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y);
			Frame();
			io.AddMouseButtonEvent(0, true);
			Frame();
			io.AddMouseButtonEvent(0, false);
			Frame();
		}

		void Choose(size_t row) {
			Frame();
			Frame();
			Click(ImVec2(Minimum.x + 12, (Minimum.y + Maximum.y) * 0.5f));
			Frame();
			ImGuiWindow *popup = ImGui::FindWindowByName("##Combo_00");
			REQUIRE(popup != nullptr);
			const ImVec2 start = popup->DC.CursorStartPos;
			Click(ImVec2(start.x + 12, start.y + ImGui::GetTextLineHeightWithSpacing() * row + 5));
		}

		void Enter(std::string_view text, bool numeric) {
			Frame();
			Frame();
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(Minimum.x + 12, (Minimum.y + Maximum.y) * 0.5f);
			io.AddKeyEvent(ImGuiMod_Ctrl, numeric);
			Frame();
			io.AddMouseButtonEvent(0, true);
			Frame();
			io.AddMouseButtonEvent(0, false);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			Frame();
			io.AddKeyEvent(ImGuiMod_Ctrl, true);
			io.AddKeyEvent(ImGuiKey_A, true);
			Frame();
			io.AddKeyEvent(ImGuiKey_A, false);
			io.AddKeyEvent(ImGuiMod_Ctrl, false);
			Frame();
			io.AddInputCharactersUTF8(std::string(text).c_str());
			Frame();
			io.AddKeyEvent(ImGuiKey_Enter, true);
			Frame();
			io.AddKeyEvent(ImGuiKey_Enter, false);
			Frame();
		}
	};
}

TEST_CASE("value controls commit complete text and preserve numeric width", "[studio][components]") {
	struct Example {
		PropertyType Type;
		const char *Text;
		bool Numeric = false;
	};
	const Example examples[]{
		{PropertyType::Int32, "1234567", true},
		{PropertyType::Int64, "9007199254740993", true},
		{PropertyType::Float, "12.25", true},
		{PropertyType::Double, "1.23456789012345", true},
		{PropertyType::Name, "new-name"},
		{PropertyType::String, "hello world"},
		{PropertyType::UDim, "0.5, -12"},
		{PropertyType::UDim2, "0.5, -12, 0.25, 24"},
		{PropertyType::Rect, "1, 2, 30, 40"},
		{PropertyType::NumberRange, "2, 8"},
		{PropertyType::NumberSequence, "0, 2, 0; 1, 5, 0"},
		{PropertyType::ColorSequence, "0, 1, 0, 0; 1, 0, 0, 1"},
	};
	for (const auto &example : examples) {
		DYNAMIC_SECTION(engine::ecs::Describe(example.Type)) {
			Context context;
			ValueWidget widget;
			widget.Field.Type = example.Type;
			widget.Value.Type = example.Type;
			engine::game::PropertyValue expected;
			std::string error;
			REQUIRE(engine::game::ParseValue(example.Type, example.Text, expected, error));
			widget.Enter(example.Text, example.Numeric);
			CHECK(widget.Changed);
			CHECK(engine::game::FormatValue(widget.Value) == engine::game::FormatValue(expected));
			if (example.Type == PropertyType::Int64) CHECK(widget.Value.Int64 == INT64_C(9007199254740993));
			if (example.Type == PropertyType::Double) CHECK(widget.Value.Double == 1.23456789012345);
		}
	}
}

TEST_CASE("transform control edits orientation while retaining position", "[studio][components]") {
	Context context;
	ValueWidget widget;
	widget.Field.Type = PropertyType::CFrame;
	widget.Value.Type = PropertyType::CFrame;
	widget.Value.CFrame.Position = {3, 4, 5};
	widget.Enter("45", true);
	CHECK(widget.Changed);
	CHECK(widget.Value.CFrame.Position == engine::core::Vector3{3, 4, 5});
	CHECK(std::abs(glm::degrees(widget.Value.CFrame.ToAngles().X) - 45.0f) < 0.001f);
}

TEST_CASE("vector and colour controls edit individual coordinates", "[studio][components]") {
	for (const auto type : {PropertyType::Vector2, PropertyType::Vector3, PropertyType::Color3}) {
		DYNAMIC_SECTION(engine::ecs::Describe(type)) {
			Context context;
			ValueWidget widget;
			widget.Field.Type = type;
			widget.Value.Type = type;
			widget.Enter("0.25", true);
			CHECK(widget.Changed);
			if (type == PropertyType::Vector2) CHECK(widget.Value.Vector2.X == 0.25f);
			if (type == PropertyType::Vector3) CHECK(widget.Value.Vector3.X == 0.25f);
			if (type == PropertyType::Color3) CHECK(widget.Value.Color3.R == 0.25f);
		}
	}
}

TEST_CASE("value pickers edit flags enums and same-world references", "[studio][components]") {
	SECTION("boolean") {
		Context context;
		ValueWidget widget;
		widget.Field.Type = PropertyType::Bool;
		widget.Value.Type = PropertyType::Bool;
		widget.Frame();
		widget.Frame();
		widget.Click(ImVec2(widget.Minimum.x + 5, widget.Minimum.y + 5));
		CHECK(widget.Changed);
		CHECK(widget.Value.Bool);
	}
	SECTION("enum") {
		Context context;
		ValueWidget widget;
		engine::ecs::EnumTable::Register("PanelEnum", "First");
		engine::ecs::EnumTable::Register("PanelEnum", "Second");
		widget.Field.Type = PropertyType::Enum;
		widget.Field.Enum = Name("PanelEnum");
		widget.Value.Type = PropertyType::Enum;
		widget.Value.Name = Name("First");
		widget.Choose(1);
		CHECK(widget.Changed);
		CHECK(widget.Value.Name == Name("Second"));
	}
	SECTION("reference and clear") {
		Context context;
		ValueWidget widget;
		engine::scene::RegisterSceneClasses();
		const Entity target = widget.World.CreateInstance(engine::scene::PartClass(), "Target");
		widget.Field.Type = PropertyType::Reference;
		widget.Value.Type = PropertyType::Reference;
		widget.Choose(1);
		CHECK(widget.Changed);
		CHECK(widget.Value.Reference == target);
		widget.Choose(0);
		CHECK(widget.Value.Reference == NULL_ENTITY);
	}
	SECTION("opaque remains read only") {
		Context context;
		ValueWidget widget;
		widget.Frame();
		CHECK_FALSE(widget.Changed);
	}
}

TEST_CASE("invalid compound input leaves the value unchanged", "[studio][components]") {
	Context context;
	ValueWidget widget;
	widget.Field.Type = PropertyType::UDim2;
	widget.Value.Type = PropertyType::UDim2;
	const std::string before = engine::game::FormatValue(widget.Value);
	widget.Enter("not a dimension", false);
	CHECK_FALSE(widget.Changed);
	CHECK(engine::game::FormatValue(widget.Value) == before);
}

TEST_CASE("mixed property values commit once the complete edit is entered", "[studio][components]") {
	Context context;
	Jobs jobs;
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	engine::scene::RegisterSceneClasses();
	WorldSettings settings;
	settings.Name = Name("MixedPropertyPanel");
	const WorldId world = editor.Universe->Create(settings);
	std::array<Entity, 2> selected;
	editor.Universe->Enter(world, [&](Store &store) {
		selected[0] = store.CreateInstance(engine::scene::PartClass(), "First");
		selected[1] = store.CreateInstance(engine::scene::PartClass(), "Second");
		const float initial = 0.5f;
		REQUIRE(store.SetProperty(selected[1], Name("Transparency"), &initial, sizeof(initial)));
	});
	editor.SelectionWorld = world;
	editor.Selection.assign(selected.begin(), selected.end());
	editor.ShowProperties = true;
	const auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
		studio::ComponentPanelProbe::Properties(editor);
		ImGui::Render();
	};
	const auto read = [&] {
		std::array<float, 2> values{};
		editor.Universe->Enter(world, [&](Store &store) {
			for (size_t index = 0; index < selected.size(); ++index)
				REQUIRE(
					store.GetProperty(selected[index], Name("Transparency"), &values[index], sizeof(float))
				);
		});
		return values;
	};
	frame();
	frame();
	const auto window = ImGui::FindWindowByName("Properties");
	REQUIRE(window != nullptr);
	const ImGuiTable *table = GImGui->Tables.GetByKey(ImHashStr("BasePart", 0, window->ID));
	REQUIRE(table != nullptr);
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(
		table->Columns[1].WorkMinX + 12, table->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f
	);
	frame();
	io.AddMouseButtonEvent(0, true);
	frame();
	io.AddMouseButtonEvent(0, false);
	frame();
	io.AddInputCharactersUTF8("0.");
	frame();
	CHECK(read() == std::array<float, 2>{0.0f, 0.5f});
	io.AddInputCharactersUTF8("75");
	frame();
	CHECK(read() == std::array<float, 2>{0.0f, 0.5f});
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, false);
	frame();
	CHECK(read() == std::array<float, 2>{0.75f, 0.75f});
}
