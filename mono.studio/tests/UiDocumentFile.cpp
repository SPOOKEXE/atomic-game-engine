#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/gui/Animation.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Style.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <studio/Commands.hpp>
#include <studio/UiDocumentFile.hpp>

TEST_SUITE_ID("studio.ui_document_file")

TEST_CASE("Studio reads and writes bounded canonical UI documents", "[studio][ui_document]") {
	const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path =
		std::filesystem::temp_directory_path() / ("atomic-ui-document-" + std::to_string(suffix) + ".aui");
	engine::gui::UiDocument document;
	engine::gui::DocumentNode root;
	root.Id = "root";
	root.Class = "Frame";
	root.Name = "Panel";
	document.Roots.push_back(root);
	engine::gui::DocumentReport report;
	REQUIRE(studio::WriteUiDocumentFile(path, document, report));
	engine::gui::UiDocument read;
	REQUIRE(studio::ReadUiDocumentFile(path, read, report));
	REQUIRE(read.Roots.size() == 1);
	CHECK(read.Roots.front().Name == "Panel");
	std::filesystem::remove(path);
}

TEST_CASE("Studio refuses an oversized UI document before decoding", "[studio][ui_document]") {
	const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path path = std::filesystem::temp_directory_path() /
									   ("atomic-ui-document-limit-" + std::to_string(suffix) + ".aui");
	{
		std::ofstream output(path, std::ios::binary);
		output.seekp(engine::gui::DocumentLimits::HARD_MAXIMUM_BINARY_BYTES);
		output.put('x');
	}
	engine::gui::UiDocument out;
	engine::gui::DocumentNode existing;
	existing.Id = "keep";
	existing.Class = "Frame";
	existing.Name = "Keep";
	out.Roots.push_back(existing);
	engine::gui::DocumentReport report;
	CHECK_FALSE(studio::ReadUiDocumentFile(path, out, report));
	REQUIRE(out.Roots.size() == 1);
	CHECK(out.Roots.front().Name == "Keep");
	std::filesystem::remove(path);
}

TEST_CASE("Studio UI document import attaches to StarterGui as one undo step", "[studio][ui_document]") {
	struct ScopedJobs {
		ScopedJobs() {
			engine::parallel::Jobs::Start(1);
		}
		~ScopedJobs() {
			engine::parallel::Jobs::Stop();
		}
	} jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::world::Universe universe;
	const auto world = universe.Create({.Name = engine::core::Name("ui.document.edit")});
	studio::CommandLog commands(universe);
	universe.Enter(world, [](engine::ecs::Store &store) { engine::scene::InstallServices(store); });
	engine::gui::UiDocument document;
	engine::gui::DocumentNode root;
	root.Id = "root";
	root.Class = "ScreenGui";
	root.Name = "Imported";
	document.Roots.push_back(root);
	const auto recording = commands.TryBeginRecording("Import UI document");
	REQUIRE(recording.has_value());
	std::vector<engine::ecs::Entity> roots;
	engine::gui::DocumentReport report;
	bool imported = false;
	universe.Enter(world, [&](engine::ecs::Store &store) {
		imported = studio::ImportUiDocumentEdit(
			store, world, engine::ecs::NULL_ENTITY, document, commands, roots, report
		);
	});
	REQUIRE(imported);
	REQUIRE(commands.FinishRecording(*recording, studio::FinishOperation::Commit));
	REQUIRE(roots.size() == 1);
	universe.Enter(world, [&](engine::ecs::Store &store) {
		const auto starter =
			engine::scene::ServiceOf(store, engine::ecs::Classes::Find(engine::core::Name("StarterGui")));
		CHECK(store.ParentOf(roots.front()) == starter);
	});
	REQUIRE(commands.CanUndo());
	CHECK(commands.Undo());
	universe.Enter(world, [&](engine::ecs::Store &store) { CHECK_FALSE(store.Alive(roots.front())); });
	document.Roots.front().Class = "Frame";
	engine::gui::DocumentReport rejected;
	universe.Enter(world, [&](engine::ecs::Store &store) {
		CHECK_FALSE(
			studio::ImportUiDocumentEdit(
				store, world, engine::ecs::NULL_ENTITY, document, commands, roots, rejected
			)
		);
	});
	CHECK_FALSE(rejected.Ok());
}

TEST_CASE("Studio theme-only UI document import is undoable", "[studio][ui_document]") {
	struct ScopedJobs {
		ScopedJobs() {
			engine::parallel::Jobs::Start(1);
		}
		~ScopedJobs() {
			engine::parallel::Jobs::Stop();
		}
	} jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::world::Universe universe;
	const auto world = universe.Create({.Name = engine::core::Name("ui.document.theme_only")});
	studio::CommandLog commands(universe);
	universe.Enter(world, [](engine::ecs::Store &store) { engine::scene::InstallServices(store); });

	engine::gui::UiDocument document;
	engine::gui::DocumentTheme theme;
	theme.Id = "palette";
	theme.Name = "Palette";
	REQUIRE(theme.Tokens.Set(
		{engine::core::Name("surface"), engine::gui::StyleValue::FromColor({0.2f, 0.3f, 0.4f})}
	));
	document.Themes.push_back(theme);
	const auto recording = commands.TryBeginRecording("Import theme only");
	REQUIRE(recording.has_value());
	std::vector<engine::ecs::Entity> roots;
	engine::gui::DocumentReport report;
	bool imported = false;
	universe.Enter(world, [&](engine::ecs::Store &store) {
		imported = studio::ImportUiDocumentEdit(
			store, world, engine::ecs::NULL_ENTITY, document, commands, roots, report
		);
	});
	REQUIRE(imported);
	REQUIRE(roots.empty());
	REQUIRE(commands.FinishRecording(*recording, studio::FinishOperation::Commit));
	REQUIRE(commands.Undoable().back().Kind == studio::CommandKind::UiDocumentImport);
	const studio::EditId themeId = commands.Undoable().back().Import.Themes.front();
	const engine::ecs::Entity original = commands.Resolve(themeId);
	REQUIRE(original != engine::ecs::NULL_ENTITY);
	REQUIRE(commands.Undo());
	CHECK(commands.Resolve(themeId) == engine::ecs::NULL_ENTITY);
	REQUIRE(commands.Redo());
	universe.Enter(world, [&](engine::ecs::Store &store) {
		const engine::ecs::Entity restored = commands.Resolve(themeId);
		REQUIRE(store.Alive(restored));
		const auto *value = store.Get<engine::gui::UITheme>(restored);
		REQUIRE(value != nullptr);
		const auto *surface = value->Tokens.Find(engine::core::Name("surface"));
		REQUIRE(surface != nullptr);
		CHECK(surface->Color.G == 0.3f);
	});
}

TEST_CASE("Studio UI import redo rebinds an external theme resource", "[studio][ui_document]") {
	struct ScopedJobs {
		ScopedJobs() {
			engine::parallel::Jobs::Start(1);
		}
		~ScopedJobs() {
			engine::parallel::Jobs::Stop();
		}
	} jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::world::Universe universe;
	const auto world = universe.Create({.Name = engine::core::Name("ui.document.theme_redo")});
	studio::CommandLog commands(universe);
	universe.Enter(world, [](engine::ecs::Store &store) { engine::scene::InstallServices(store); });

	engine::gui::UiDocument document;
	engine::gui::DocumentTheme theme;
	theme.Id = "theme";
	theme.Name = "Palette";
	REQUIRE(theme.Tokens.Set(
		{engine::core::Name("TextColor3"), engine::gui::StyleValue::FromColor({0.2f, 0.4f, 0.6f})}
	));
	document.Themes.push_back(theme);
	engine::gui::DocumentNode root;
	root.Id = "root";
	root.Class = "ScreenGui";
	root.Name = "Themed";
	engine::gui::DocumentProperty reference;
	reference.Name = "Theme";
	reference.Type = engine::ecs::PropertyType::Reference;
	reference.Value.Type = engine::ecs::PropertyType::Reference;
	reference.Reference = "theme";
	root.Properties.push_back(reference);
	engine::gui::UIStyle style;
	style.Rule.Class = engine::core::Name("accent");
	style.Rule.State = engine::gui::StyleState::Hovered;
	REQUIRE(style.Rule.Declarations.Set(
		{engine::core::Name("TextTransparency"), engine::gui::StyleValue::FromNumber(0.25f)}
	));
	engine::core::ByteWriter styleBytes(0, engine::gui::DocumentLimits::HARD_MAXIMUM_COMPONENT_BYTES);
	const engine::ecs::TypeDescriptor &styleDescriptor =
		engine::ecs::Components::Describe(engine::ecs::Components::Find(engine::core::Name("gui.UIStyle")));
	styleDescriptor.Write(styleBytes, &style, 1);
	engine::gui::DocumentNode styleNode;
	styleNode.Id = "style";
	styleNode.Class = "UIStyle";
	styleNode.Name = "HoverStyle";
	styleNode.Components.push_back(
		{"gui.UIStyle", std::vector<std::byte>(styleBytes.Bytes().begin(), styleBytes.Bytes().end())}
	);
	engine::gui::DocumentNode panel;
	panel.Id = "panel";
	panel.Class = "Frame";
	panel.Name = "Panel";
	panel.Children.push_back(std::move(styleNode));

	engine::gui::AnimationPlayback animation;
	animation.Clip.Tween.Time = 0.5f;
	engine::gui::PresentationTrack track;
	track.Property = engine::gui::PresentationProperty::BackgroundTransparency;
	REQUIRE(track.Add({0.0f, engine::gui::PresentationValue::FromNumber(0.0f)}));
	REQUIRE(track.Add({1.0f, engine::gui::PresentationValue::FromNumber(1.0f)}));
	REQUIRE(animation.Clip.AddTrack(track));
	engine::core::ByteWriter animationBytes(0, engine::gui::DocumentLimits::HARD_MAXIMUM_COMPONENT_BYTES);
	const engine::ecs::TypeDescriptor &animationDescriptor = engine::ecs::Components::Describe(
		engine::ecs::Components::Find(engine::core::Name("gui.AnimationPlayback"))
	);
	animationDescriptor.Write(animationBytes, &animation, 1);
	engine::gui::DocumentNode animationNode;
	animationNode.Id = "animation";
	animationNode.Class = "UIAnimation";
	animationNode.Name = "Fade";
	animationNode.Components.push_back(
		{"gui.AnimationPlayback",
		 std::vector<std::byte>(animationBytes.Bytes().begin(), animationBytes.Bytes().end())}
	);
	panel.Children.push_back(std::move(animationNode));
	root.Children.push_back(std::move(panel));
	document.Roots.push_back(root);

	const auto recording = commands.TryBeginRecording("Import themed UI");
	REQUIRE(recording.has_value());
	std::vector<engine::ecs::Entity> roots;
	engine::gui::DocumentReport report;
	bool imported = false;
	universe.Enter(world, [&](engine::ecs::Store &store) {
		imported = studio::ImportUiDocumentEdit(
			store, world, engine::ecs::NULL_ENTITY, document, commands, roots, report
		);
	});
	REQUIRE(imported);
	REQUIRE(commands.FinishRecording(*recording, studio::FinishOperation::Commit));
	engine::game::PropertyValue before;
	engine::game::PropertyValue after;
	universe.Enter(world, [&](engine::ecs::Store &store) {
		const engine::ecs::Entity importedRoot = commands.Resolve(commands.Undoable().back().Subject);
		const engine::ecs::ClassInfo &info = engine::ecs::Classes::Describe(store.ClassOf(importedRoot));
		for (const engine::ecs::PropertyDescriptor &descriptor : info.Properties) {
			if (descriptor.Name != engine::core::Name("Enabled")) continue;
			REQUIRE(engine::game::ReadProperty(store, importedRoot, descriptor, before));
			after = before;
			after.Bool = false;
			REQUIRE(engine::game::WriteProperty(store, importedRoot, descriptor, after));
			commands.RecordProperty(
				world, importedRoot, descriptor.Name, before, after, "Disable imported UI"
			);
			return;
		}
		FAIL("ScreenGui must expose Enabled");
	});
	// A later ordinary command owns the edit itself. Replaying it after the
	// import rebuild verifies the import command does not overwrite that history.
	REQUIRE(commands.Undo());
	const studio::Command &importCommand = commands.Undoable().back();
	REQUIRE(importCommand.Kind == studio::CommandKind::UiDocumentImport);
	const studio::EditId importSubject = importCommand.Subject;
	const studio::EditId importTheme = importCommand.Import.Themes.front();
	universe.Enter(world, [&](engine::ecs::Store &store) {
		store.DestroyInstance(commands.Resolve(importTheme));
	});
	REQUIRE(commands.Undo());
	CHECK(commands.Resolve(importSubject) == engine::ecs::NULL_ENTITY);
	REQUIRE(commands.Redo());
	REQUIRE(commands.Redo());

	universe.Enter(world, [&](engine::ecs::Store &store) {
		const engine::ecs::Entity rebuilt = commands.Resolve(commands.Undoable().back().Subject);
		REQUIRE(store.Alive(rebuilt));
		const engine::ecs::ClassInfo &info = engine::ecs::Classes::Describe(store.ClassOf(rebuilt));
		for (const engine::ecs::PropertyDescriptor &descriptor : info.Properties) {
			if (descriptor.Name != engine::core::Name("Enabled")) continue;
			engine::game::PropertyValue enabled;
			REQUIRE(engine::game::ReadProperty(store, rebuilt, descriptor, enabled));
			CHECK_FALSE(enabled.Bool);
			break;
		}
		const engine::gui::ThemeBinding *binding = store.Get<engine::gui::ThemeBinding>(rebuilt);
		REQUIRE(binding != nullptr);
		REQUIRE(store.Alive(binding->Theme));
		const engine::gui::UITheme *restoredTheme = store.Get<engine::gui::UITheme>(binding->Theme);
		REQUIRE(restoredTheme != nullptr);
		const auto *color = restoredTheme->Tokens.Find(engine::core::Name("TextColor3"));
		REQUIRE(color != nullptr);
		CHECK(color->Color.B == 0.6f);
		engine::ecs::Entity styleInstance = engine::ecs::NULL_ENTITY;
		engine::ecs::Entity animationInstance = engine::ecs::NULL_ENTITY;
		store.EachChild(rebuilt, [&](engine::ecs::Entity panelInstance) {
			store.EachChild(panelInstance, [&](engine::ecs::Entity child) {
				if (store.IsA(child, engine::gui::GuiClass("UIStyle"))) styleInstance = child;
				if (store.IsA(child, engine::gui::GuiClass("UIAnimation"))) animationInstance = child;
			});
		});
		const engine::gui::UIStyle *restoredStyle = store.Get<engine::gui::UIStyle>(styleInstance);
		REQUIRE(restoredStyle != nullptr);
		CHECK(restoredStyle->Rule.Class == engine::core::Name("accent"));
		CHECK(restoredStyle->Rule.Declarations.Find(engine::core::Name("TextTransparency"))->Number == 0.25f);
		const auto *restoredAnimation = store.Get<engine::gui::AnimationPlayback>(animationInstance);
		REQUIRE(restoredAnimation != nullptr);
		REQUIRE(restoredAnimation->Clip.Tracks().size() == 1);
		CHECK(restoredAnimation->Clip.Tracks().front().Keys().size() == 2);
	});

	// A parent id bound in another world is never a valid attachment target,
	// even when the other store happens to have a live entity with that handle.
	REQUIRE(commands.Undo());
	REQUIRE(commands.Undo());
	const studio::Command &undoneImport = commands.Redoable().back();
	const auto other = universe.Create({.Name = engine::core::Name("ui.document.other")});
	engine::ecs::Entity foreignParent;
	universe.Enter(other, [&](engine::ecs::Store &store) {
		foreignParent = store.CreateInstance(engine::gui::GuiClass("Frame"), "Foreign parent");
	});
	commands.Adopt(undoneImport.Import.Parents.front(), other, foreignParent);
	CHECK_FALSE(commands.Redo());
}

TEST_CASE("Studio drops an import whose whole membership already went away", "[studio][ui_document]") {
	struct ScopedJobs {
		ScopedJobs() {
			engine::parallel::Jobs::Start(1);
		}
		~ScopedJobs() {
			engine::parallel::Jobs::Stop();
		}
	} jobs;
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	engine::gui::RegisterGuiClasses();
	engine::world::Universe universe;
	const auto world = universe.Create({.Name = engine::core::Name("ui.document.gone")});
	studio::CommandLog commands(universe);
	universe.Enter(world, [](engine::ecs::Store &store) { engine::scene::InstallServices(store); });
	engine::gui::UiDocument document;
	engine::gui::DocumentNode root;
	root.Id = "root";
	root.Class = "ScreenGui";
	root.Name = "Gone";
	document.Roots.push_back(std::move(root));
	std::vector<engine::ecs::Entity> roots;
	engine::gui::DocumentReport report;
	universe.Enter(world, [&](engine::ecs::Store &store) {
		REQUIRE(
			studio::ImportUiDocumentEdit(
				store, world, engine::ecs::NULL_ENTITY, document, commands, roots, report
			)
		);
		store.DestroyInstance(roots.front());
	});
	CHECK_FALSE(commands.Undo());
	CHECK_FALSE(commands.CanRedo());
}
