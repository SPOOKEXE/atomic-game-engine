// The Components panel checked through a real, backend-free ImGui frame.
//
// A context needs no window or GPU. This lets the test drive the same checkbox
// a person clicks in Studio and then read the runtime component from its store.

#include "PropertyWidgets.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/RenderFeatures.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <string_view>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <utility>
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
		static void Properties(Editor &editor, std::string filter = "Transparency") {
			editor.PropertyFilter = std::move(filter);
			editor.DrawProperties();
		}
		static bool PrepareFrameGraph(Editor &editor) {
			engine::ui::InterfaceSettings settings;
			settings.DisplayWidth = 1280;
			settings.DisplayHeight = 720;
			editor.ShowFrameGraph = true;
			editor.FrameGraphState.Interval = 0;
			editor.FrameGraphState.Mode = DiagnosticAggregation::Latest;
			return editor.Interface.Initialise(editor.Renderer, nullptr, settings);
		}
		static uint64_t FrameGraphPanel(Editor &editor) {
			editor.Interface.Begin(1.0f / 60.0f);
			ImGui::GetIO().IniFilename = nullptr;
			ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(1220, 650), ImGuiCond_Always);
			editor.DrawFrameGraph();
			editor.Interface.End();
			return editor.Interface.Signature();
		}
		static void PauseFrameGraph(Editor &editor, bool paused) {
			editor.FrameGraphState.Paused = paused;
		}
		static void FrameSample(Editor &editor, double now, float seconds) {
			editor.Statistics.Record(now, seconds);
		}
		static engine::render::FrameSummary FrameGraphSummary(const Editor &editor) {
			return editor.FrameGraphState.PresentationSummary;
		}
		static size_t FrameGraphDropped(const Editor &editor) {
			return editor.FrameGraphState.Dropped;
		}
		static size_t FrameGraphGpuDropped(const Editor &editor) {
			return editor.FrameGraphState.DroppedGpuMarks;
		}
		static size_t FrameGraphSpanCount(const Editor &editor) {
			return editor.FrameGraphState.Spans.size();
		}
		static bool EditingCollectionTag(const Editor &editor) {
			return editor.CollectionTagEditing;
		}
		static void Draw(Editor &editor) {
			editor.DrawComponents();
		}
		static void AddRenderingProfile(Editor &editor, Name name, engine::graph::PipelineDocument document) {
			editor.RenderingProfiles.Set(name, std::move(document));
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

	ImGuiID PropertyTableId(
		const ImGuiWindow &window, std::string_view property, std::string_view className = "Part"
	) {
		for (const auto &descriptor :
			 engine::ecs::Classes::Describe(engine::ecs::Classes::Find(Name(className))).Properties) {
			if (descriptor.Name == Name(property))
				return ImHashStr(
					descriptor.PropertiesTag.IsValid() ? descriptor.PropertiesTag.Text().data()
													   : "Unassigned",
					0,
					window.ID
				);
		}
		return 0;
	}

	struct ExplorerInsertOverlap {
		ImVec2 Button;
		bool Pressed = false;
		bool PopupOpen = false;

		void Frame(const Mouse &mouse = {}) {
			ImGuiIO &io = ImGui::GetIO();
			io.AddMousePosEvent(mouse.X, mouse.Y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouse.Down);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
			ImGui::Begin("Explorer");

			ImGui::SetNextItemAllowOverlap();
			ImGui::TreeNodeEx(
				"##row",
				ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
					ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Leaf,
				"InsertTarget"
			);
			const ImVec2 row = ImGui::GetItemRectMin();
			const float side = ImGui::GetFrameHeight();
			const float x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - side;
			Button = ImVec2(x + side * 0.5f, row.y + side * 0.5f);
			if (ImGui::IsItemHovered() ||
				ImGui::IsMouseHoveringRect(ImVec2(x, row.y), ImVec2(x + side, row.y + side))) {
				ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - side);
				Pressed |= ImGui::SmallButton("+##insert-hover");
				if (Pressed) {
					ImGui::OpenPopup("##insert-hover-popup");
				}
			}
			if (ImGui::BeginPopup("##insert-hover-popup")) {
				ImGui::BeginChild("##class-list", ImVec2(220.0f, 260.0f));
				ImGui::EndChild();
				PopupOpen = true;
				ImGui::EndPopup();
			}
			ImGui::End();
			ImGui::Render();
		}
	};

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

TEST_CASE("automatic LOD ratio widgets include legacy and explicit aliases", "[studio][components][lod]") {
	CHECK(studio::IsAutomaticLodRatioProperty("Lod1Ratio"));
	CHECK(studio::IsAutomaticLodRatioProperty("Lod2Ratio"));
	CHECK(studio::IsAutomaticLodRatioProperty("Lod3Ratio"));
	CHECK(studio::IsAutomaticLodRatioProperty("AutoLod1Ratio"));
	CHECK(studio::IsAutomaticLodRatioProperty("AutoLod2Ratio"));
	CHECK(studio::IsAutomaticLodRatioProperty("AutoLod3Ratio"));
	CHECK_FALSE(studio::IsAutomaticLodRatioProperty("CustomLod1Ratio"));
	CHECK_FALSE(studio::IsAutomaticLodRatioProperty("LodTargetQuadArea"));
}

TEST_CASE("the explorer insert button opens its picker over a full-width tree row", "[studio][explorer]") {
	Context context;
	ExplorerInsertOverlap overlap;
	overlap.Frame();

	// The real row is full-width. The button must remain alive when the pointer
	// enters its trailing overlap, then accept a normal click and open its picker.
	overlap.Frame(Mouse{.X = 80.0f, .Y = overlap.Button.y});
	overlap.Frame(Mouse{.X = overlap.Button.x, .Y = overlap.Button.y});
	overlap.Frame(Mouse{.X = overlap.Button.x, .Y = overlap.Button.y});
	overlap.Frame(Mouse{.X = overlap.Button.x, .Y = overlap.Button.y, .Down = true});
	overlap.Frame(Mouse{.X = overlap.Button.x, .Y = overlap.Button.y});

	CHECK(overlap.Pressed);
	CHECK(overlap.PopupOpen);
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

TEST_CASE("structural property edits survive component changes and support undo", "[studio][components]") {
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
	editor.ShowProperties = true;
	const auto anchored = [&] {
		bool result = false;
		editor.Universe->Enter(world, [&](Store &store) {
			result = !store.Has<engine::scene::Simulated>(selected);
		});
		return result;
	};
	const auto toggle = [&] {
		const bool before = anchored();
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
		studio::ComponentPanelProbe::Properties(editor, "Anchored");
		ImGui::Render();
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
		studio::ComponentPanelProbe::Properties(editor, "Anchored");
		ImGui::Render();
		const auto window = ImGui::FindWindowByName("Properties");
		REQUIRE(window != nullptr);
		const ImGuiID tableId = PropertyTableId(*window, "Anchored");
		const ImGuiTable *table = GImGui->Tables.GetByKey(tableId);
		REQUIRE(table != nullptr);
		const float x = table->Columns[1].WorkMinX + 12;
		const float y = table->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f;
		auto &io = ImGui::GetIO();
		const auto frame = [&] {
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
			studio::ComponentPanelProbe::Properties(editor, "Anchored");
			ImGui::Render();
		};
		io.AddMousePosEvent(x, y);
		frame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		frame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		frame();
		REQUIRE(anchored() != before);
	};
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
	studio::ComponentPanelProbe::Properties(editor, "Anchored");
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

TEST_CASE("component rows show attached storage and render effects default to None", "[studio][components]") {
	Context context;
	Jobs jobs;
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	engine::scene::RegisterSceneClasses();
	WorldSettings settings;
	settings.Name = Name("AttachedComponentRows");
	const WorldId world = editor.Universe->Create(settings);
	Entity part;
	Entity camera;
	Entity surfaceCamera;
	editor.Universe->Enter(world, [&](Store &store) {
		part = store.CreateInstance(engine::scene::PartClass(), "Part");
		camera = store.CreateInstance(engine::ecs::Classes::Find(Name("Camera")), "Camera");
		surfaceCamera =
			store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "SurfaceCamera");
		REQUIRE(part != NULL_ENTITY);
		REQUIRE(camera != NULL_ENTITY);
		REQUIRE(surfaceCamera != NULL_ENTITY);
		store.Set(part, engine::scene::RenderEffects{});
	});
	editor.SelectionWorld = world;
	editor.ShowComponents = true;
	editor.ShowProperties = true;

	const auto drawLog = [&](Entity selected, std::string filter, bool properties = false) {
		editor.Selection = {selected};
		studio::ComponentPanelProbe::Filter(editor, std::move(filter));
		ImGui::NewFrame();
		ImGui::LogToBuffer();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
		if (properties) {
			studio::ComponentPanelProbe::Properties(editor, "ComputeEffectNode");
		} else {
			studio::ComponentPanelProbe::Draw(editor);
		}
		const std::string log = GImGui->LogBuffer.c_str();
		ImGui::LogFinish();
		ImGui::Render();
		return log;
	};

	const std::string absentStorm = drawLog(part, {});
	CHECK(absentStorm.find("StormResponse") == std::string::npos);
	const std::string absentVegetation = drawLog(part, {});
	CHECK(absentVegetation.find("Vegetation") == std::string::npos);
	const std::string absentSurfaceCamera = drawLog(camera, {});
	CHECK(absentSurfaceCamera.find("SurfaceCamera") == std::string::npos);
	const std::string attachedSurfaceCamera = drawLog(surfaceCamera, {});
	CHECK(attachedSurfaceCamera.find("SurfaceCamera") != std::string::npos);
	const std::string attachedRenderEffects = drawLog(part, {});
	CHECK(attachedRenderEffects.find("RenderEffects") != std::string::npos);
	const std::string defaultEffect = drawLog(part, {}, true);
	CHECK(defaultEffect.find("ComputeEffectNode") != std::string::npos);
	CHECK(defaultEffect.find("None") != std::string::npos);
}

TEST_CASE("component and property effect pickers save only matching graph nodes", "[studio][components]") {
	Context context;
	Jobs jobs;
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	engine::scene::RegisterSceneClasses();
	const Name profileName("EffectPickerProfile");
	engine::graph::PipelineDocument document;
	const auto addVisualNode = [&](Name nodeName, Name kind) {
		document.Record({.Kind = engine::graph::EditKind::AddNode, .Name = nodeName, .NodeKind = kind});
		document.Record({.Kind = engine::graph::EditKind::Set, .Key = Name("attachment"), .Value = "visual"});
	};
	addVisualNode(Name("compute.only"), Name("dispatch"));
	addVisualNode(Name("raster.only"), Name("raster"));
	const Name disabledName("compute.disabled");
	addVisualNode(disabledName, Name("dispatch"));
	document.Record({.Kind = engine::graph::EditKind::Enable, .Name = disabledName, .Enabled = false});
	addVisualNode(Name("other.kind"), Name("sample"));
	studio::ComponentPanelProbe::AddRenderingProfile(editor, profileName, std::move(document));
	WorldSettings settings;
	settings.Name = Name("EffectPickerWorld");
	settings.RenderingProfile = profileName;
	const WorldId world = editor.Universe->Create(settings);
	Entity part;
	const ComponentId renderEffects = Components::Of<engine::scene::RenderEffects>();
	editor.Universe->Enter(world, [&](Store &store) {
		part = store.CreateInstance(engine::scene::PartClass(), "Part");
		REQUIRE(part != NULL_ENTITY);
		store.Set(part, engine::scene::RenderEffects{});
	});
	editor.SelectionWorld = world;
	editor.Selection = {part};
	editor.ShowComponents = true;
	editor.ShowProperties = true;
	studio::ComponentPanelProbe::Filter(editor, "RenderEffects");

	const auto panelFrame = [&](bool log = false) {
		ImGui::NewFrame();
		if (log) ImGui::LogToBuffer();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
		studio::ComponentPanelProbe::Draw(editor);
		const std::string result = log ? GImGui->LogBuffer.c_str() : std::string{};
		if (log) ImGui::LogFinish();
		ImGui::Render();
		return result;
	};
	const auto readAttachment = [&](size_t index) {
		engine::scene::RenderEffectAttachment attachment;
		editor.Universe->Enter(world, [&](Store &store) {
			const auto *effects = store.Get<engine::scene::RenderEffects>(part);
			REQUIRE(effects != nullptr);
			attachment = effects->Attachments[index];
		});
		return attachment;
	};
	const auto comboLocation = [&](size_t row) {
		panelFrame();
		const ImGuiWindow *window = ImGui::FindWindowByName("Components");
		REQUIRE(window != nullptr);
		const ImGuiID componentId = ImHashData(&renderEffects.Index, sizeof(renderEffects.Index), window->ID);
		const ImGuiTable *table = GImGui->Tables.GetByKey(ImHashStr("##native-properties", 0, componentId));
		REQUIRE(table != nullptr);
		return ImVec2(
			table->Columns[1].WorkMinX + 12.0f,
			table->OuterRect.Min.y + ImGui::GetFrameHeight() * (0.5f + static_cast<float>(row))
		);
	};
	const auto click = [&](ImVec2 point) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(point.x, point.y);
		panelFrame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		panelFrame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		panelFrame();
	};
	const auto openPicker = [&](size_t row) {
		const ImVec2 position = comboLocation(row);
		click(position);
		const std::string choices = panelFrame(true);
		REQUIRE(!GImGui->OpenPopupStack.empty());
		REQUIRE(GImGui->OpenPopupStack.back().Window != nullptr);
		const ImVec2 start = GImGui->OpenPopupStack.back().Window->DC.CursorStartPos;
		return std::pair<std::string, ImVec2>{choices, start};
	};
	const auto choose = [&](ImVec2 start, size_t row) {
		click(ImVec2(
			start.x + 12.0f, start.y + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(row) + 5.0f
		));
	};

	const auto [computeChoices, computeMenu] = openPicker(0);
	CHECK(computeChoices.find("compute.only") != std::string::npos);
	CHECK(computeChoices.find("raster.only") == std::string::npos);
	CHECK(computeChoices.find("compute.disabled") == std::string::npos);
	CHECK(computeChoices.find("other.kind") == std::string::npos);
	choose(computeMenu, 1);
	CHECK(readAttachment(0).Node == Name("compute.only"));
	CHECK(readAttachment(0).Enabled);

	const auto [postChoices, postMenu] = openPicker(1);
	CHECK(postChoices.find("raster.only") != std::string::npos);
	choose(postMenu, 1);
	CHECK(readAttachment(1).Node == Name("raster.only"));
	CHECK(readAttachment(1).Enabled);

	// The log also contains the selected compute name outside this popup. Try
	// the third row directly to prove the incompatible node is not selectable.
	const auto [postPanelLog, filteredPostMenu] = openPicker(1);
	CHECK(postPanelLog.find("raster.only") != std::string::npos);
	choose(filteredPostMenu, 2);
	CHECK(readAttachment(1).Node == Name("raster.only"));

	const auto [clearChoices, clearMenu] = openPicker(0);
	CHECK(clearChoices.find("compute.only") != std::string::npos);
	choose(clearMenu, 0);
	CHECK_FALSE(readAttachment(0).Node.IsValid());
	CHECK_FALSE(readAttachment(0).Enabled);

	const auto propertyFrame = [&](bool log = false) {
		ImGui::NewFrame();
		if (log) ImGui::LogToBuffer();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
		studio::ComponentPanelProbe::Properties(editor, "PostProcessEffectNode");
		const std::string result = log ? GImGui->LogBuffer.c_str() : std::string{};
		if (log) ImGui::LogFinish();
		ImGui::Render();
		return result;
	};
	propertyFrame();
	propertyFrame();
	const ImGuiWindow *propertiesWindow = ImGui::FindWindowByName("Properties");
	REQUIRE(propertiesWindow != nullptr);
	const ImGuiTable *propertiesTable =
		GImGui->Tables.GetByKey(PropertyTableId(*propertiesWindow, "PostProcessEffectNode"));
	REQUIRE(propertiesTable != nullptr);
	const ImVec2 propertyPicker(
		propertiesTable->Columns[1].WorkMinX + 12.0f,
		propertiesTable->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f
	);
	const auto clickProperty = [&](ImVec2 point) {
		auto &io = ImGui::GetIO();
		io.AddMousePosEvent(point.x, point.y);
		propertyFrame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		propertyFrame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		propertyFrame();
	};
	const auto openPropertyPicker = [&] {
		clickProperty(propertyPicker);
		const std::string choices = propertyFrame(true);
		REQUIRE(!GImGui->OpenPopupStack.empty());
		REQUIRE(GImGui->OpenPopupStack.back().Window != nullptr);
		return std::pair<std::string, ImVec2>{
			choices, GImGui->OpenPopupStack.back().Window->DC.CursorStartPos
		};
	};
	const auto chooseProperty = [&](ImVec2 menu, size_t row) {
		clickProperty(ImVec2(
			menu.x + 12.0f, menu.y + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(row) + 5.0f
		));
	};
	const auto [propertyChoices, initialMenu] = openPropertyPicker();
	CHECK(propertyChoices.find("raster.only") != std::string::npos);
	CHECK(propertyChoices.find("compute.only") == std::string::npos);
	chooseProperty(initialMenu, 0);
	CHECK_FALSE(readAttachment(1).Node.IsValid());
	CHECK_FALSE(readAttachment(1).Enabled);

	const auto [reselectedChoices, reselectMenu] = openPropertyPicker();
	CHECK(reselectedChoices.find("raster.only") != std::string::npos);
	chooseProperty(reselectMenu, 1);
	CHECK(readAttachment(1).Node == Name("raster.only"));
	CHECK(readAttachment(1).Enabled);

	const auto [finalChoices, finalMenu] = openPropertyPicker();
	CHECK(finalChoices.find("raster.only") != std::string::npos);
	chooseProperty(finalMenu, 0);
	CHECK_FALSE(readAttachment(1).Node.IsValid());
	CHECK_FALSE(readAttachment(1).Enabled);
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

TEST_CASE("a shared property edit commits to every selected live instance", "[studio][components]") {
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
	const ImGuiTable *table = GImGui->Tables.GetByKey(PropertyTableId(*window, "Transparency"));
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

TEST_CASE("a generic LOD distance edit materializes every distance band", "[studio][components]") {
	Context context;
	Jobs jobs;
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	engine::scene::RegisterSceneClasses();
	WorldSettings settings;
	settings.Name = Name("LodPropertyPanel");
	const WorldId world = editor.Universe->Create(settings);
	std::array<Entity, 2> selected;
	editor.Universe->Enter(world, [&](Store &store) {
		const auto meshPart = engine::ecs::Classes::Find(Name("MeshPart"));
		selected[0] = store.CreateInstance(meshPart, "First");
		selected[1] = store.CreateInstance(meshPart, "Second");
	});
	editor.SelectionWorld = world;
	editor.Selection.assign(selected.begin(), selected.end());
	editor.ShowProperties = true;
	const auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
		studio::ComponentPanelProbe::Properties(editor, "Lod1Distance");
		ImGui::Render();
	};
	const auto read = [&] {
		std::array<std::array<float, 3>, 2> distances{};
		editor.Universe->Enter(world, [&](Store &store) {
			for (size_t index = 0; index < selected.size(); ++index) {
				REQUIRE(store.GetProperty(
					selected[index], Name("Lod1Distance"), &distances[index][0], sizeof(float)
				));
				REQUIRE(store.GetProperty(
					selected[index], Name("Lod2Distance"), &distances[index][1], sizeof(float)
				));
				REQUIRE(store.GetProperty(
					selected[index], Name("Lod3Distance"), &distances[index][2], sizeof(float)
				));
			}
		});
		return distances;
	};
	frame();
	frame();
	const ImGuiWindow *window = ImGui::FindWindowByName("Properties");
	REQUIRE(window != nullptr);
	const ImGuiTable *table = GImGui->Tables.GetByKey(PropertyTableId(*window, "Lod1Distance", "MeshPart"));
	REQUIRE(table != nullptr);
	auto &io = ImGui::GetIO();
	io.AddMousePosEvent(
		table->Columns[1].WorkMinX + 12, table->OuterRect.Min.y + ImGui::GetFrameHeight() * 0.5f
	);
	frame();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddMouseButtonEvent(0, true);
	frame();
	io.AddMouseButtonEvent(0, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	frame();
	io.AddInputCharactersUTF8("15");
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, false);
	frame();
	CHECK(
		read() == std::array<std::array<float, 3>, 2>{
					  std::array<float, 3>{15.0f, 60.0f, 120.0f},
					  std::array<float, 3>{15.0f, 60.0f, 120.0f},
				  }
	);
}

TEST_CASE(
	"collection tag renames preserve mixed memberships and undo atomically", "[studio][properties][tags]"
) {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	Universe universe;
	WorldSettings settings;
	settings.Name = Name("CollectionTagUndo");
	const WorldId world = universe.Create(settings);
	studio::CommandLog commands(universe);
	std::array<Entity, 3> selected;
	universe.Enter(world, [&](Store &store) {
		for (size_t i = 0; i < selected.size(); i++)
			selected[i] = store.CreateInstance(engine::scene::PartClass(), "Tagged");
		REQUIRE(engine::scene::AddTag(store, selected[0], Name("old")));
		REQUIRE(engine::scene::AddTag(store, selected[1], Name("old")));
		REQUIRE(engine::scene::AddTag(store, selected[1], Name("existing")));
		REQUIRE(engine::scene::AddTag(store, selected[2], Name("existing")));
		REQUIRE(
			studio::ApplyCollectionTagEdit(
				store, world, selected, {Name("old"), Name("existing"), true}, &commands
			)
		);
		for (const Entity entity : selected)
			CHECK(engine::scene::HasTag(store, entity, Name("existing")));
		for (const Entity entity : selected)
			CHECK_FALSE(engine::scene::HasTag(store, entity, Name("old")));
	});
	REQUIRE(commands.Undo());
	universe.Enter(world, [&](Store &store) {
		CHECK(engine::scene::HasTag(store, selected[0], Name("old")));
		CHECK_FALSE(engine::scene::HasTag(store, selected[0], Name("existing")));
		CHECK(engine::scene::HasTag(store, selected[1], Name("old")));
		CHECK(engine::scene::HasTag(store, selected[1], Name("existing")));
		CHECK_FALSE(engine::scene::HasTag(store, selected[2], Name("old")));
		CHECK(engine::scene::HasTag(store, selected[2], Name("existing")));
	});
	REQUIRE(commands.Redo());
	universe.Enter(world, [&](Store &store) {
		REQUIRE(
			studio::ApplyCollectionTagEdit(store, world, selected, {Name("existing"), {}, true}, &commands)
		);
		for (const Entity entity : selected)
			CHECK_FALSE(engine::scene::HasTag(store, entity, Name("existing")));
	});
	REQUIRE(commands.Undo());
	universe.Enter(world, [&](Store &store) {
		for (const Entity entity : selected)
			CHECK(engine::scene::HasTag(store, entity, Name("existing")));
		for (size_t i = engine::scene::TagsOf(store).Names.size(); i < engine::scene::TagTable::MAXIMUM; i++)
			REQUIRE(engine::scene::TagsOf(store).Register(Name("fill-" + std::to_string(i))) != 0);
		CHECK_FALSE(
			studio::ApplyCollectionTagEdit(
				store, world, selected, {Name("existing"), Name("overflow"), true}, &commands
			)
		);
		for (const Entity entity : selected)
			CHECK(engine::scene::HasTag(store, entity, Name("existing")));
	});
}

TEST_CASE(
	"the Collection Tags divider adds editable rows and cancels empty drafts",
	"[studio][properties][tags][input]"
) {
	Context context;
	Jobs jobs;
	const auto previousConfig = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-collection-tag-input";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct Restore {
		std::filesystem::path Previous;
		~Restore() {
			studio::SetConfigRoot(Previous);
		}
	} restore{previousConfig};
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	WorldSettings settings;
	settings.Name = Name("CollectionTagInput");
	const WorldId world = editor.Universe->Create(settings);
	Entity selected;
	editor.Universe->Enter(world, [&](Store &store) {
		selected = store.CreateInstance(engine::scene::PartClass(), "Selected");
	});
	editor.SelectionWorld = world;
	editor.Selection = {selected};
	editor.ShowProperties = true;
	const auto frame = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Always);
		studio::ComponentPanelProbe::Properties(editor, "nothing-matches-this-filter");
		const auto *window = ImGui::FindWindowByName("Properties");
		REQUIRE(window != nullptr);
		// The '+' is the final row. End restores the parent last-item record,
		// while the inspector window retains the row's actual submitted position.
		const ImVec2 plus(
			window->DC.CursorPosPrevLine.x -
				(ImGui::CalcTextSize("+").x + ImGui::GetStyle().FramePadding.x * 2.0f) * .5f,
			window->DC.CursorPosPrevLine.y + ImGui::GetTextLineHeight() * .5f
		);
		ImGui::Render();
		return plus;
	};
	auto &io = ImGui::GetIO();
	frame();
	const auto add = [&] {
		const ImVec2 plus = frame();
		io.AddMousePosEvent(plus.x, plus.y);
		frame();
		io.AddMouseButtonEvent(0, true);
		frame();
		io.AddMouseButtonEvent(0, false);
		frame();
		frame();
		frame();
		REQUIRE(studio::ComponentPanelProbe::EditingCollectionTag(editor));
	};
	const auto key = [&](ImGuiKey value) {
		io.AddKeyEvent(value, true);
		frame();
		io.AddKeyEvent(value, false);
		frame();
	};
	add();
	key(ImGuiKey_Enter);
	CHECK_FALSE(studio::ComponentPanelProbe::EditingCollectionTag(editor));
	add();
	io.AddInputCharactersUTF8("cancelled");
	frame();
	key(ImGuiKey_Escape);
	CHECK_FALSE(studio::ComponentPanelProbe::EditingCollectionTag(editor));
	add();
	io.AddMousePosEvent(580, 470);
	frame();
	io.AddMouseButtonEvent(0, true);
	frame();
	io.AddMouseButtonEvent(0, false);
	frame();
	CHECK_FALSE(studio::ComponentPanelProbe::EditingCollectionTag(editor));
	add();
	io.AddInputCharactersUTF8("new-tag");
	frame();
	key(ImGuiKey_Enter);
	CHECK_FALSE(studio::ComponentPanelProbe::EditingCollectionTag(editor));
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK(engine::scene::HasTag(store, selected, Name("new-tag")));
		CHECK_FALSE(engine::scene::HasTag(store, selected, Name("cancelled")));
	});
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](Store &store) {
		CHECK_FALSE(engine::scene::HasTag(store, selected, Name("new-tag")));
	});
}

TEST_CASE(
	"a paused Frame Graph panel keeps its actual draw signature while live statistics change",
	"[studio][components][diagnostics][paused]"
) {
	Context context;
	const auto previousConfig = studio::ConfigRoot();
	const auto scratch = std::filesystem::temp_directory_path() / "studio-paused-frame-graph";
	std::filesystem::create_directories(scratch);
	studio::SetConfigRoot(scratch);
	struct RestoreConfig {
		std::filesystem::path Previous;
		~RestoreConfig() {
			studio::SetConfigRoot(Previous);
		}
	} restoreConfig{previousConfig};
	using engine::core::FrameGraph;
	const bool wasEnabled = FrameGraph::IsEnabled();
	FrameGraph::SetEnabled(true);
	struct RestoreGraph {
		bool Enabled;
		~RestoreGraph() {
			FrameGraph::SetEnabled(Enabled);
		}
	} restoreGraph{wasEnabled};
	studio::Editor editor;
	REQUIRE(studio::ComponentPanelProbe::PrepareFrameGraph(editor));
	FrameGraph::BeginFrame();
	FrameGraph::Report("paused-panel-baseline", engine::core::ProfileCategory::Render, 0.25f);
	FrameGraph::EndFrame();
	studio::ComponentPanelProbe::FrameSample(editor, 1.0, 1.0f / 60.0f);
	for (size_t i = 0; i < 4; i++)
		studio::ComponentPanelProbe::FrameGraphPanel(editor);
	const auto summary = studio::ComponentPanelProbe::FrameGraphSummary(editor);
	const size_t spans = studio::ComponentPanelProbe::FrameGraphSpanCount(editor);
	const size_t dropped = studio::ComponentPanelProbe::FrameGraphDropped(editor);
	const size_t gpuDropped = studio::ComponentPanelProbe::FrameGraphGpuDropped(editor);
	REQUIRE(summary.CurrentMilliseconds > 16.0f);
	REQUIRE(spans > 0);
	studio::ComponentPanelProbe::PauseFrameGraph(editor, true);
	for (size_t i = 0; i < 3; i++)
		studio::ComponentPanelProbe::FrameGraphPanel(editor);
	const uint64_t paused = studio::ComponentPanelProbe::FrameGraphPanel(editor);
	REQUIRE(paused != 0);

	// Overflow the real collector's depth, so both the live tree and its
	// dropped-span warning disagree with the retained paused snapshot.
	FrameGraph::BeginFrame();
	const auto deepScope = [&](auto &&self, size_t depth) -> void {
		FrameGraph::Scope scope("changed-live-depth", engine::core::ProfileCategory::ECS);
		if (depth > 0) self(self, depth - 1);
	};
	deepScope(deepScope, FrameGraph::MAXIMUM_DEPTH + 2);
	FrameGraph::EndFrame();
	REQUIRE(FrameGraph::Dropped() > dropped);
	for (size_t i = 0; i < 8; i++) {
		studio::ComponentPanelProbe::FrameSample(
			editor, 2.0 + static_cast<double>(i), 0.001f + static_cast<float>(i) * 0.0001f
		);
		CHECK(studio::ComponentPanelProbe::FrameGraphPanel(editor) == paused);
		const auto retained = studio::ComponentPanelProbe::FrameGraphSummary(editor);
		CHECK(retained.Current == summary.Current);
		CHECK(retained.CurrentMilliseconds == summary.CurrentMilliseconds);
		CHECK(retained.Minimum == summary.Minimum);
		CHECK(retained.Average == summary.Average);
		CHECK(retained.Maximum == summary.Maximum);
		CHECK(retained.Jitter == summary.Jitter);
		CHECK(studio::ComponentPanelProbe::FrameGraphDropped(editor) == dropped);
		CHECK(studio::ComponentPanelProbe::FrameGraphGpuDropped(editor) == gpuDropped);
		CHECK(studio::ComponentPanelProbe::FrameGraphSpanCount(editor) == spans);
	}
	studio::ComponentPanelProbe::PauseFrameGraph(editor, false);
	const uint64_t resumed = studio::ComponentPanelProbe::FrameGraphPanel(editor);
	CHECK(resumed != paused);
	CHECK(studio::ComponentPanelProbe::FrameGraphSummary(editor).CurrentMilliseconds < 2.0f);
	CHECK(studio::ComponentPanelProbe::FrameGraphDropped(editor) == FrameGraph::Dropped());
	CHECK(studio::ComponentPanelProbe::FrameGraphSpanCount(editor) == FrameGraph::Spans().size());
}
