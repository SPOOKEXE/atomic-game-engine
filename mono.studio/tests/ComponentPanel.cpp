// The Components panel checked through a real, backend-free ImGui frame.
//
// A context needs no window or GPU. This lets the test drive the same checkbox
// a person clicks in Studio and then read the runtime component from its store.

#include <engine/ecs/Classes.hpp>
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
		static void Draw(Editor &editor) {
			editor.DrawComponents();
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
