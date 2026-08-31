// The storage seam behind Studio's shared program and shader editor.
//
// A program is filed through `SourceCache`; a shader is written through
// `SetShaderSource`, whose revision is what makes the renderer see the edit.
// The panel cannot prove either path without a window, so the adapter is tested
// here against a real store.

#include "SourceEditor.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

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
