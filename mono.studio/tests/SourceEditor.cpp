// The storage seam behind Studio's shared program and shader editor.
//
// A program is filed through `SourceCache`; a shader is written through
// `SetShaderSource`, whose revision is what makes the renderer see the edit.
// The panel cannot prove either path without a window, so the adapter is tested
// here against a real store.

#include "SourceEditor.hpp"

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("studio.source-editor")
TEST_DEPENDS("engine.scene.shaders")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using studio::ReadSourceDocument;
using studio::SourceDocument;
using studio::SourceDocumentKind;
using studio::SourceDocumentKindOf;
using studio::WriteSourceDocument;

namespace {

	Store Fresh(const char *name) {
		engine::scene::EnsureClassTree();
		engine::script::ScriptClass();
		engine::scene::ShaderScriptClass();
		return Store(name);
	}

}

TEST_CASE("the source editor recognizes programs and shader scripts", "[studio][source-editor]") {
	Store store = Fresh("source-editor.kinds");
	const Entity program = store.CreateInstance(engine::script::ScriptClass(), "Program");
	const Entity shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "Shader");
	const Entity part = store.CreateInstance(engine::scene::PartClass(), "Part");

	CHECK(SourceDocumentKindOf(store, program) == SourceDocumentKind::Program);
	CHECK(SourceDocumentKindOf(store, shader) == SourceDocumentKind::Shader);
	CHECK_FALSE(SourceDocumentKindOf(store, part).has_value());
}

TEST_CASE("an unsaved program receives a path and round-trips through the cache", "[studio][source-editor]") {
	Store store = Fresh("source-editor.program");
	const Entity program = store.CreateInstance(engine::script::ScriptClass(), "Bootstrap");

	const std::optional<SourceDocument> opened = ReadSourceDocument(store, program);
	REQUIRE(opened.has_value());
	CHECK(opened->Kind == SourceDocumentKind::Program);
	CHECK_FALSE(opened->Path.IsValid());
	CHECK(opened->Text.empty());

	Name path;
	REQUIRE(WriteSourceDocument(store, program, SourceDocumentKind::Program, path, "print('ready')"));
	CHECK(path.Text() == "Scripts/Bootstrap.luau");

	const std::optional<SourceDocument> saved = ReadSourceDocument(store, program);
	REQUIRE(saved.has_value());
	CHECK(saved->Path == path);
	CHECK(saved->Text == "print('ready')");
}

TEST_CASE(
	"new programs do not overwrite existing sources and retain their language", "[studio][source-editor]"
) {
	struct AssetsRoot {
		std::filesystem::path Previous = engine::core::Paths::Assets();
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() /
			("studio-source-editor-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		AssetsRoot() {
			std::filesystem::create_directories(Directory / "Scripts");
			engine::core::Paths::SetAssetsOverride(Directory);
		}
		~AssetsRoot() {
			engine::core::Paths::SetAssetsOverride(Previous);
			std::error_code error;
			std::filesystem::remove_all(Directory, error);
		}
	} assets;
	Store store = Fresh("source-editor.unique");
	const Entity first = store.CreateInstance(engine::script::ScriptClass(), "Script");
	const Entity second = store.CreateInstance(engine::script::ScriptClass(), "Script");
	const Entity reserved = store.CreateInstance(engine::script::ScriptClass(), "Other");
	engine::script::SetSourcePath(store, reserved, Name("Scripts/Script_1.luau"));
	std::ofstream(assets.Directory / "Scripts/Script_2.luau") << "existing file";

	Name firstPath;
	Name secondPath;
	REQUIRE(WriteSourceDocument(store, first, SourceDocumentKind::Program, firstPath, "first"));
	REQUIRE(WriteSourceDocument(store, second, SourceDocumentKind::Program, secondPath, "second"));
	CHECK(firstPath.Text() == "Scripts/Script.luau");
	CHECK(secondPath.Text() == "Scripts/Script_3.luau");
	REQUIRE(ReadSourceDocument(store, first).has_value());
	CHECK(ReadSourceDocument(store, first)->Text == "first");
	CHECK(ReadSourceDocument(store, second)->Text == "second");

	const Entity javascript = store.CreateInstance(engine::script::ScriptClass(), "Script");
	store.Set(javascript, engine::script::CodeSourceContainerSelector{engine::script::Language::JavaScript});
	Name javascriptPath;
	REQUIRE(
		WriteSourceDocument(store, javascript, SourceDocumentKind::Program, javascriptPath, "let value = 1;")
	);
	CHECK(javascriptPath.Text() == "Scripts/Script.js");
	CHECK(engine::script::ActiveLanguageOf(store, javascript) == engine::script::Language::JavaScript);
	CHECK(ReadSourceDocument(store, javascript)->Text == "let value = 1;");
	CHECK(ReadSourceDocument(store, first)->Text == "first");
}

TEST_CASE("a shader edit advances the revision and reopens from its row", "[studio][source-editor]") {
	Store store = Fresh("source-editor.shader");
	const Entity shader = store.CreateInstance(engine::scene::ShaderScriptClass(), "Toon");
	REQUIRE(engine::scene::SetShaderSource(store, shader, "#version 450\nvoid main() {}"));

	const uint32_t before = store.Get<engine::scene::ShaderSource>(shader)->Revision;
	const std::optional<SourceDocument> opened = ReadSourceDocument(store, shader);
	REQUIRE(opened.has_value());
	CHECK(opened->Kind == SourceDocumentKind::Shader);
	CHECK_FALSE(opened->Path.IsValid());
	CHECK(opened->Text.find("void main") != std::string::npos);

	Name unusedPath;
	REQUIRE(
		WriteSourceDocument(store, shader, SourceDocumentKind::Shader, unusedPath, "void main() { discard; }")
	);
	const engine::scene::ShaderSource *saved = store.Get<engine::scene::ShaderSource>(shader);
	REQUIRE(saved != nullptr);
	CHECK(saved->Code == "void main() { discard; }");
	CHECK(saved->Revision > before);
	CHECK_FALSE(unusedPath.IsValid());

	CHECK_FALSE(WriteSourceDocument(store, shader, SourceDocumentKind::Program, unusedPath, "wrong"));
}
