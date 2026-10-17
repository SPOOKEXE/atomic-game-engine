// The UI authoring panel is checked through a backend-free ImGui frame. The
// test follows its canonical values through Studio's command log.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Animation.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Style.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>
#include <studio/Commands.hpp>
#include <studio/Editor.hpp>
#include <type_traits>
#include <vector>

TEST_SUITE_ID("studio.ui-authoring")
TEST_DEPENDS("engine.gui.services")

namespace studio {
	struct UiAuthoringProbe {
		static void Draw(Editor &editor) {
			editor.DrawUiAuthoring();
		}

		static engine::ecs::Entity Insert(
			Editor &editor,
			engine::world::WorldId world,
			engine::ecs::ClassId klass,
			engine::ecs::Entity parent = {}
		) {
			return editor.InsertInstance(world, klass, parent);
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
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

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
	};
}

TEST_CASE("inserting a ScreenGui places its visible tree in StarterGui", "[studio][ui-authoring][gui]") {
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	const engine::world::WorldId world = editor.Universe->Create({.Name = engine::core::Name("ui_insert")});
	editor.Universe->Enter(world, [](engine::ecs::Store &store) {
		engine::scene::InstallServices(store);
		engine::gui::InstallGuiServices(store);
	});

	const engine::ecs::Entity screen =
		studio::UiAuthoringProbe::Insert(editor, world, engine::gui::GuiClass("ScreenGui"));
	REQUIRE(screen != engine::ecs::NULL_ENTITY);
	const engine::ecs::Entity label =
		studio::UiAuthoringProbe::Insert(editor, world, engine::gui::GuiClass("TextLabel"), screen);
	REQUIRE(label != engine::ecs::NULL_ENTITY);

	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const engine::ecs::Entity starterGui =
			engine::scene::ServiceOf(store, engine::ecs::Classes::Find(engine::core::Name("StarterGui")));
		REQUIRE(starterGui != engine::ecs::NULL_ENTITY);
		CHECK(store.ParentOf(screen) == starterGui);
		CHECK(store.ParentOf(label) == screen);

		engine::gui::Element element = *store.Get<engine::gui::Element>(label);
		element.Size = engine::core::UDim2{0.0f, 100.0f, 0.0f, 100.0f};
		store.Set(label, element);
		engine::gui::Background background = *store.Get<engine::gui::Background>(label);
		background.Color = {0.125f, 0.125f, 0.125f};
		background.Transparency = 0.0f;
		store.Set(label, background);

		engine::gui::CompileRequest request;
		request.Display = {.Width = 800.0f, .Height = 600.0f};
		request.ScreenGuis = engine::gui::ScreenGuiSource::StarterGui;
		engine::gui::Compiled compiled;
		REQUIRE(compiled.Rebuild(store, request));

		bool foundBackground = false;
		for (const engine::gui::DrawCommand &command : compiled.Commands().Commands) {
			if (command.Source != label || command.Kind != engine::gui::DrawKind::Rectangle) continue;
			foundBackground = command.Tint == background.Color && command.Transparency == 0.0f;
		}
		CHECK(foundBackground);
	});
}

TEST_CASE("UI authoring exposes canonical values and preserves opaque undo", "[studio][ui-authoring]") {
	Context context;
	Jobs jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	const engine::world::WorldId world = editor.Universe->Create({.Name = engine::core::Name("ui")});
	engine::ecs::Entity screen;
	engine::ecs::Entity frame;
	engine::ecs::Entity label;
	engine::ecs::Entity theme;
	engine::ecs::Entity style;
	engine::ecs::Entity animation;
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		screen = store.CreateInstance(engine::gui::GuiClass("ScreenGui"), "ScreenGui");
		frame = store.CreateInstance(engine::gui::GuiClass("Frame"), "Frame");
		store.SetParent(frame, screen);
		label = store.CreateInstance(engine::gui::GuiClass("TextLabel"), "Label");
		store.SetParent(label, frame);
		store.Set(label, engine::gui::LabelLocalizationArguments{});
		theme = store.CreateInstance(engine::gui::GuiClass("UITheme"), "Theme");
		style = store.CreateInstance(engine::gui::GuiClass("UIStyle"), "Style");
		animation = store.CreateInstance(engine::gui::GuiClass("UIAnimation"), "Animation");
		store.SetParent(animation, frame);
		engine::gui::UITheme authoredTheme;
		REQUIRE(authoredTheme.Tokens.Set(
			{engine::core::Name("surface"), engine::gui::StyleValue::FromColor({0.2f, 0.3f, 0.4f})}
		));
		store.Set(theme, authoredTheme);
		engine::gui::ThemeBinding *themeBinding = store.GetMutable<engine::gui::ThemeBinding>(screen);
		REQUIRE(themeBinding != nullptr);
		themeBinding->Theme = theme;
		engine::gui::StyleClass frameClasses;
		REQUIRE(frameClasses.Names.Add(engine::core::Name("hud")));
		store.Set(frame, frameClasses);
		engine::gui::UIStyle authoredStyle;
		authoredStyle.Rule.Class = engine::core::Name("hud");
		REQUIRE(authoredStyle.Rule.Declarations.Set(
			{engine::core::Name("BackgroundTransparency"), engine::gui::StyleValue::FromNumber(0.25f)}
		));
		REQUIRE(authoredStyle.Rule.Declarations.Set(
			{engine::core::Name("BackgroundColor3"), engine::gui::StyleValue::FromColor({0.2f, 0.3f, 0.4f})}
		));
		store.Set(style, authoredStyle);
		store.SetParent(style, frame);
		engine::gui::PresentationTrack track;
		track.Property = engine::gui::PresentationProperty::BackgroundTransparency;
		REQUIRE(track.Add({0.0f, engine::gui::PresentationValue::FromNumber(0.0f)}));
		REQUIRE(track.Add({1.0f, engine::gui::PresentationValue::FromNumber(1.0f)}));
		engine::gui::AnimationPlayback playback;
		REQUIRE(playback.Clip.AddTrack(track));
		REQUIRE(playback.Clip.AddMarker({engine::core::Name("start"), 0.25f}));
		REQUIRE(playback.Clip.AddMarker({engine::core::Name("middle"), 0.5f}));
		store.Set(animation, playback);
	});
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		engine::gui::Compiled compiled;
		engine::gui::CompileRequest request;
		request.Display.Width = 800.0f;
		request.Display.Height = 600.0f;
		REQUIRE(compiled.Rebuild(store, request));
		engine::gui::ResolvedStyle resolved;
		REQUIRE(resolved.Values.Set(
			{engine::core::Name("BackgroundColor3"), engine::gui::StyleValue::FromColor({0.2f, 0.3f, 0.4f})}
		));
		REQUIRE(resolved.Values.Set(
			{engine::core::Name("surface"), engine::gui::StyleValue::FromColor({0.2f, 0.3f, 0.4f})}
		));
		REQUIRE(resolved.Values.Set(
			{engine::core::Name("BackgroundTransparency"), engine::gui::StyleValue::FromNumber(0.25f)}
		));
		store.Set(frame, resolved);
	});
	REQUIRE(screen != engine::ecs::NULL_ENTITY);
	REQUIRE(frame != engine::ecs::NULL_ENTITY);
	REQUIRE(theme != engine::ecs::NULL_ENTITY);
	REQUIRE(style != engine::ecs::NULL_ENTITY);
	REQUIRE(animation != engine::ecs::NULL_ENTITY);

	editor.SelectionWorld = world;
	editor.Selection = {frame};
	editor.ShowUiAuthoring = true;
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
	studio::UiAuthoringProbe::Draw(editor);
	const std::string log = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	CHECK(log.find("Styles") != std::string::npos);
	CHECK(log.find("Resolved style trace") != std::string::npos);
	CHECK(log.find("BackgroundColor3: class rule") != std::string::npos);
	CHECK(log.find("Binding") != std::string::npos);
	CHECK(log.find("Presentation animation") != std::string::npos);
	CHECK(log.find("Accessibility and diagnostics") != std::string::npos);

	editor.Selection = {style};
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	studio::UiAuthoringProbe::Draw(editor);
	const std::string styleLog = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	CHECK(styleLog.find("Style rule") != std::string::npos);
	CHECK(styleLog.find("add color declaration") != std::string::npos);

	editor.Selection = {label};
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	studio::UiAuthoringProbe::Draw(editor);
	const std::string localizationLog = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	CHECK(localizationLog.find("Add string argument") != std::string::npos);

	editor.Selection = {theme};
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	studio::UiAuthoringProbe::Draw(editor);
	const std::string themeLog = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	CHECK(themeLog.find("Theme tokens") != std::string::npos);
	CHECK(themeLog.find("surface") != std::string::npos);

	editor.Selection = {animation};
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	studio::UiAuthoringProbe::Draw(editor);
	const std::string animationLog = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	CHECK(animationLog.find("track 1: 2 keyframes") != std::string::npos);
	CHECK(animationLog.find("markers: 2") != std::string::npos);

	const auto serialise = [](const auto &value) {
		const auto &type =
			engine::ecs::Components::Describe(engine::ecs::Components::Of<std::decay_t<decltype(value)>>());
		engine::core::ByteWriter writer;
		type.Write(writer, &value, 1);
		return std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end());
	};
	const auto edit = editor.Commands->TryBeginRecording("Edit UI theme token");
	REQUIRE(edit.has_value());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto before = *store.Get<engine::gui::UITheme>(theme);
		auto changed = before;
		REQUIRE(changed.Tokens.Set(
			{engine::core::Name("surface"), engine::gui::StyleValue::FromColor({0.9f, 0.8f, 0.7f})}
		));
		store.Set(theme, changed);
		editor.Commands->RecordComponent(
			world,
			theme,
			engine::core::Name("gui.UITheme"),
			serialise(before),
			serialise(changed),
			"Edit UI theme token"
		);
	});
	editor.Commands->FinishRecording(*edit, studio::FinishOperation::Commit);
	REQUIRE(editor.Commands->Undo());
	const engine::ecs::Entity restored = editor.Commands->Resolve(editor.Commands->Redoable().back().Subject);
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *value = store.Get<engine::gui::UITheme>(restored);
		REQUIRE(value != nullptr);
		const auto *surface = value->Tokens.Find(engine::core::Name("surface"));
		REQUIRE(surface != nullptr);
		CHECK(surface->Color.R == 0.2f);
	});
	REQUIRE(editor.Commands->Redo());

	const engine::ecs::Entity redone = editor.Commands->Resolve(editor.Commands->Undoable().back().Subject);
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *value = store.Get<engine::gui::UITheme>(redone);
		REQUIRE(value != nullptr);
		const auto *surface = value->Tokens.Find(engine::core::Name("surface"));
		REQUIRE(surface != nullptr);
		CHECK(surface->Color.R == 0.9f);
	});
	const auto animationEdit = editor.Commands->TryBeginRecording("Edit UI animation keyframe");
	REQUIRE(animationEdit.has_value());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto before = *store.Get<engine::gui::AnimationPlayback>(animation);
		auto changed = before;
		changed.Playing = false;
		store.Set(animation, changed);
		editor.Commands->RecordComponent(
			world,
			animation,
			engine::core::Name("gui.AnimationPlayback"),
			serialise(before),
			serialise(changed),
			"Edit UI animation keyframe"
		);
	});
	editor.Commands->FinishRecording(*animationEdit, studio::FinishOperation::Commit);
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK(store.Get<engine::gui::AnimationPlayback>(animation)->Playing);
	});
	REQUIRE(editor.Commands->Redo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK_FALSE(store.Get<engine::gui::AnimationPlayback>(animation)->Playing);
	});
	const auto markerEdit = editor.Commands->TryBeginRecording("Move UI animation marker");
	REQUIRE(markerEdit.has_value());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto before = *store.Get<engine::gui::AnimationPlayback>(animation);
		auto changed = before;
		engine::gui::UIAnimation rebuilt;
		for (const auto &track : before.Clip.Tracks())
			REQUIRE(rebuilt.AddTrack(track));
		std::vector<engine::gui::AnimationMarker> markers(
			before.Clip.Markers().begin(), before.Clip.Markers().end()
		);
		for (auto &marker : markers) {
			if (marker.Name == engine::core::Name("start")) marker.Time = 0.5f;
			if (marker.Name == engine::core::Name("middle")) marker.Time = 0.25f;
		}
		std::sort(markers.begin(), markers.end(), [](const auto &left, const auto &right) {
			return left.Time < right.Time;
		});
		for (const auto marker : markers) {
			REQUIRE(rebuilt.AddMarker(marker));
		}
		changed.Clip = rebuilt;
		store.Set(animation, changed);
		editor.Commands->RecordComponent(
			world,
			animation,
			engine::core::Name("gui.AnimationPlayback"),
			serialise(before),
			serialise(changed),
			"Move UI animation marker"
		);
	});
	REQUIRE(editor.Commands->FinishRecording(*markerEdit, studio::FinishOperation::Commit));
	const auto markerTime = [](const engine::gui::AnimationPlayback &playback, engine::core::Name name) {
		for (const auto marker : playback.Clip.Markers())
			if (marker.Name == name) return marker.Time;
		return -1.0f;
	};
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *playback = store.Get<engine::gui::AnimationPlayback>(animation);
		CHECK(markerTime(*playback, engine::core::Name("start")) == 0.5f);
		CHECK(markerTime(*playback, engine::core::Name("middle")) == 0.25f);
	});
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *playback = store.Get<engine::gui::AnimationPlayback>(animation);
		CHECK(markerTime(*playback, engine::core::Name("start")) == 0.25f);
		CHECK(markerTime(*playback, engine::core::Name("middle")) == 0.5f);
	});
	REQUIRE(editor.Commands->Redo());

	// A drag spans three ImGui frames. The authoring panel keeps the in-flight
	// color value outside ECS and records only when the item deactivates.
	editor.Selection = {theme};
	const auto drawAuthoring = [&](ImVec2 pointer, bool pressed) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(pointer.x, pointer.y);
		io.AddMouseButtonEvent(0, pressed);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(600.0f, 500.0f), ImGuiCond_Always);
		studio::UiAuthoringProbe::Draw(editor);
		ImGui::Render();
	};
	const size_t beforeDrag = editor.Commands->Undoable().size();
	drawAuthoring({110.0f, 125.0f}, false);
	drawAuthoring({110.0f, 125.0f}, true);
	drawAuthoring({70.0f, 125.0f}, true);
	drawAuthoring({70.0f, 125.0f}, false);
	REQUIRE(editor.Commands->Undoable().size() == beforeDrag + 1);
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *surface =
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("surface"));
		REQUIRE(surface != nullptr);
		CHECK((surface->Color.R != 0.9f || surface->Color.G != 0.8f || surface->Color.B != 0.7f));
	});
	CHECK(editor.Commands->Undoable().size() == beforeDrag + 1);
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *surface =
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("surface"));
		REQUIRE(surface != nullptr);
		CHECK(surface->Color.R == 0.9f);
		CHECK(surface->Color.G == 0.8f);
		CHECK(surface->Color.B == 0.7f);
	});
	REQUIRE(editor.Commands->Redo());

	// A new text label owns an empty argument component through its class set.
	// Clicking the authoring action records a value replacement, so undo restores
	// the empty component and redo restores the first slot.
	editor.Selection = {label};
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		if (store.Get<engine::gui::LabelLocalizationArguments>(label) == nullptr)
			store.Set(label, engine::gui::LabelLocalizationArguments{});
	});
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *arguments = store.Get<engine::gui::LabelLocalizationArguments>(label);
		REQUIRE(arguments != nullptr);
		CHECK(arguments->Count == 0);
	});
	const size_t beforeArgument = editor.Commands->Undoable().size();
	// Localization follows the property sections. Limit probing to that region,
	// where the only action is the typed-argument add button.
	for (float y = 390.0f; y < 500.0f; y += 10.0f) {
		drawAuthoring({90.0f, y}, false);
		drawAuthoring({90.0f, y}, true);
		drawAuthoring({90.0f, y}, false);
		bool added = false;
		editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
			added = store.Get<engine::gui::LabelLocalizationArguments>(label)->Count == 1;
		});
		if (added) break;
	}
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *arguments = store.Get<engine::gui::LabelLocalizationArguments>(label);
		REQUIRE(arguments != nullptr);
		CHECK(arguments->Count == 1);
	});
	REQUIRE(editor.Commands->Undoable().size() == beforeArgument + 1);
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK(store.Get<engine::gui::LabelLocalizationArguments>(label)->Count == 0);
	});
	REQUIRE(editor.Commands->Redo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK(store.Get<engine::gui::LabelLocalizationArguments>(label)->Count == 1);
	});

	// Style declaration widgets keep their draft outside ECS while active. A
	// complete drag becomes one component replacement and one undo step.
	editor.Selection = {style};
	const size_t beforeStyleDrag = editor.Commands->Undoable().size();
	drawAuthoring({105.0f, 174.0f}, false);
	drawAuthoring({105.0f, 174.0f}, true);
	drawAuthoring({160.0f, 174.0f}, true);
	drawAuthoring({160.0f, 174.0f}, false);
	REQUIRE(editor.Commands->Undoable().size() == beforeStyleDrag + 1);
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *value = store.Get<engine::gui::UIStyle>(style);
		REQUIRE(value != nullptr);
		const auto *number = value->Rule.Declarations.Find(engine::core::Name("BackgroundTransparency"));
		REQUIRE(number != nullptr);
		CHECK(number->Number != 0.25f);
	});
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *number = store.Get<engine::gui::UIStyle>(style)->Rule.Declarations.Find(
			engine::core::Name("BackgroundTransparency")
		);
		REQUIRE(number != nullptr);
		CHECK(number->Number == 0.25f);
	});
	REQUIRE(editor.Commands->Redo());
	const size_t beforeStyleColor = editor.Commands->Undoable().size();
	drawAuthoring({105.0f, 198.0f}, false);
	drawAuthoring({105.0f, 198.0f}, true);
	drawAuthoring({70.0f, 198.0f}, true);
	drawAuthoring({70.0f, 198.0f}, false);
	REQUIRE(editor.Commands->Undoable().size() == beforeStyleColor + 1);
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *color = store.Get<engine::gui::UIStyle>(style)->Rule.Declarations.Find(
			engine::core::Name("BackgroundColor3")
		);
		REQUIRE(color != nullptr);
		CHECK((color->Color.R != 0.2f || color->Color.G != 0.3f || color->Color.B != 0.4f));
	});
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *color = store.Get<engine::gui::UIStyle>(style)->Rule.Declarations.Find(
			engine::core::Name("BackgroundColor3")
		);
		REQUIRE(color != nullptr);
		CHECK(color->Color.R == 0.2f);
		CHECK(color->Color.G == 0.3f);
		CHECK(color->Color.B == 0.4f);
	});
	REQUIRE(editor.Commands->Redo());
}

TEST_CASE(
	"UI authoring extracts repeated direct values as one reversible theme edit", "[studio][ui-authoring]"
) {
	Context context;
	Jobs jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();

	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	editor.Commands = std::make_unique<studio::CommandLog>(*editor.Universe);
	const engine::world::WorldId world = editor.Universe->Create({.Name = engine::core::Name("extract")});
	engine::ecs::Entity screen;
	engine::ecs::Entity first;
	engine::ecs::Entity second;
	engine::ecs::Entity theme;
	const engine::core::Color3 colour{0.3f, 0.5f, 0.7f};
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		screen = store.CreateInstance(engine::gui::GuiClass("ScreenGui"), "ScreenGui");
		first = store.CreateInstance(engine::gui::GuiClass("Frame"), "First");
		second = store.CreateInstance(engine::gui::GuiClass("Frame"), "Second");
		store.SetParent(first, screen);
		store.SetParent(second, screen);
		theme = store.CreateInstance(engine::gui::GuiClass("UITheme"), "Theme");
		auto *binding = store.GetMutable<engine::gui::ThemeBinding>(screen);
		REQUIRE(binding != nullptr);
		binding->Theme = theme;
		for (const engine::ecs::Entity frame : {first, second}) {
			auto background = *store.Get<engine::gui::Background>(frame);
			background.Color = colour;
			store.Set(frame, background);
			auto direct = *store.Get<engine::gui::StyleDirect>(frame);
			direct.Set(engine::gui::StyleDirectProperty::BackgroundColor);
			store.Set(frame, direct);
		}
	});
	editor.SelectionWorld = world;
	editor.Selection = {screen};
	editor.ShowUiAuthoring = true;
	const size_t beforeExtraction = editor.Commands->Undoable().size();
	const auto draw = [&](float y, bool down) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(250.0f, y);
		io.AddMouseButtonEvent(0, down);
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
		ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
		studio::UiAuthoringProbe::Draw(editor);
		ImGui::Render();
	};
	// The action shares the Styles row with Add local style. Probe only its
	// horizontal range so no other authoring action can be activated.
	for (float y = 150.0f; y < 260.0f; y += 5.0f) {
		draw(y, false);
		draw(y, true);
		draw(y, false);
		bool extracted = false;
		editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
			extracted =
				store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("BackgroundColor3")) !=
				nullptr;
		});
		if (extracted) break;
	}

	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *token =
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("BackgroundColor3"));
		REQUIRE(token != nullptr);
		CHECK(token->Color == colour);
		for (const engine::ecs::Entity frame : {first, second}) {
			const auto *background = store.Get<engine::gui::Background>(frame);
			const auto *direct = store.Get<engine::gui::StyleDirect>(frame);
			REQUIRE(background != nullptr);
			REQUIRE(direct != nullptr);
			CHECK(background->Color == engine::gui::Background{}.Color);
			CHECK_FALSE(direct->Has(engine::gui::StyleDirectProperty::BackgroundColor));
			engine::gui::StyleSet resolved;
			REQUIRE(
				engine::gui::ResolveStyle(
					store.Get<engine::gui::UITheme>(theme)->Tokens,
					{},
					{},
					{},
					engine::gui::StyleState::None,
					resolved
				)
			);
			CHECK(resolved.Find(engine::core::Name("BackgroundColor3"))->Color == colour);
		}
	});
	REQUIRE(editor.Commands->Undoable().size() > beforeExtraction);
	REQUIRE(editor.Commands->Undo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK(
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("BackgroundColor3")) ==
			nullptr
		);
		for (const engine::ecs::Entity frame : {first, second}) {
			CHECK(store.Get<engine::gui::Background>(frame)->Color == colour);
			CHECK(store.Get<engine::gui::StyleDirect>(frame)->Has(
				engine::gui::StyleDirectProperty::BackgroundColor
			));
		}
	});
	REQUIRE(editor.Commands->Redo());
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK(
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("BackgroundColor3")) !=
			nullptr
		);
		for (const engine::ecs::Entity frame : {first, second})
			CHECK_FALSE(store.Get<engine::gui::StyleDirect>(frame)->Has(
				engine::gui::StyleDirectProperty::BackgroundColor
			));
	});
	REQUIRE(editor.Commands->Undo());
	engine::ecs::Entity other;
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		other = store.CreateInstance(engine::gui::GuiClass("ScreenGui"), "Other");
		auto *otherBinding = store.GetMutable<engine::gui::ThemeBinding>(other);
		REQUIRE(otherBinding != nullptr);
		otherBinding->Theme = theme;
	});
	for (float y = 150.0f; y < 260.0f; y += 5.0f) {
		draw(y, false);
		draw(y, true);
		draw(y, false);
	}
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		CHECK(
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("BackgroundColor3")) ==
			nullptr
		);
		for (const engine::ecs::Entity frame : {first, second})
			CHECK(store.Get<engine::gui::StyleDirect>(frame)->Has(
				engine::gui::StyleDirectProperty::BackgroundColor
			));
	});
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		auto *otherBinding = store.GetMutable<engine::gui::ThemeBinding>(other);
		REQUIRE(otherBinding != nullptr);
		otherBinding->Theme = engine::ecs::NULL_ENTITY;
		const auto *storedValue = store.Get<engine::gui::UITheme>(theme);
		REQUIRE(storedValue != nullptr);
		auto value = *storedValue;
		REQUIRE(value.Tokens.Set(
			{engine::core::Name("BackgroundColor3"), engine::gui::StyleValue::FromColor({0.9f, 0.1f, 0.2f})}
		));
		store.Set(theme, value);
	});
	const size_t beforeConflictingExtraction = editor.Commands->Undoable().size();
	for (float y = 150.0f; y < 260.0f; y += 5.0f) {
		draw(y, false);
		draw(y, true);
		draw(y, false);
	}
	CHECK(editor.Commands->Undoable().size() == beforeConflictingExtraction);
	editor.Universe->Enter(world, [&](engine::ecs::Store &store) {
		const auto *token =
			store.Get<engine::gui::UITheme>(theme)->Tokens.Find(engine::core::Name("BackgroundColor3"));
		REQUIRE(token != nullptr);
		CHECK(token->Color == engine::core::Color3{0.9f, 0.1f, 0.2f});
		for (const engine::ecs::Entity frame : {first, second}) {
			CHECK(store.Get<engine::gui::Background>(frame)->Color == colour);
			CHECK(store.Get<engine::gui::StyleDirect>(frame)->Has(
				engine::gui::StyleDirectProperty::BackgroundColor
			));
		}
	});
}
