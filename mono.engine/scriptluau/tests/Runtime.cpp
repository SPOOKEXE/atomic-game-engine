// The Luau adapter, used without the factory.
//
// **What this suite is for is the module boundary itself.** Every other
// scripting case reaches a VM through `scripthost::MakeRuntime`, which links
// both adapters - so a Luau adapter that had quietly come to need something out
// of `scriptjs` would still pass all of them. Here nothing but `scriptluau` and
// what it sits on is linked, so an edge that crossed to the other VM is a link
// error rather than a coupling nobody notices.
//
// The behaviour of a Luau runtime is asserted in `engine.scripthost.*`, where it
// is asserted against JavaScript's at the same time. This is deliberately thin.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/scriptluau/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scriptluau.runtime")
// A script's vocabulary is the class tree, so a change to it has to re-run this.
TEST_DEPENDS("engine.scene.part")
TEST_DEPENDS("engine.scene.shaders")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeLuauRuntime;

namespace {
	// Registers the class tree, which is what `Instance.new` resolves against.
	// A `Store` is not movable, so this is a call rather than a factory.
	void RegisterClasses() {
		engine::scene::EnsureClassTree();
	}
}

TEST_CASE("the luau adapter opens a runtime on its own", "[scriptluau]") {
	RegisterClasses();
	Store store("scriptluau_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime != nullptr);
	CHECK(runtime->Which() == Language::Luau);
	CHECK(runtime->Run("local x = 1 + 1 assert(x == 2, 'arithmetic')"));
}

TEST_CASE("the luau adapter builds into the world it was handed", "[scriptluau]") {
	RegisterClasses();
	Store store("scriptluau_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime->Run("Instance.new('Part').Name = 'FromLuau'"));

	// Found by walking the world rather than by being handed a handle, because
	// what is being checked is that the adapter reached `script`'s object model
	// with no factory and no second VM in the process.
	engine::ecs::Entity part = engine::ecs::NULL_ENTITY;
	store.Each<const engine::scene::Transform, const engine::scene::Bounds>(
		[&](engine::ecs::Entity entity, const engine::scene::Transform &, const engine::scene::Bounds &) {
			part = entity;
		}
	);

	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.InstanceNameOf(part).Text() == "FromLuau");
}

TEST_CASE("luau creates shader scripts and writes their source", "[scriptluau][shaders]") {
	RegisterClasses();
	engine::scene::ShaderScriptClass();
	Store store("scriptluau_shader_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime->Run(R"(
		local shader = Instance.new("ShaderScript")
		shader.Name = "Generated"
		local before = shader.Revision
		shader.Source = "#version 450\nvoid main() {}"
		assert(shader.Source == "#version 450\nvoid main() {}", "source did not round-trip")
		assert(shader.Revision > before, "source write did not move the revision")
	)"));

	const engine::scene::ShaderText shader =
		engine::scene::ShaderTextOf(store, engine::core::Name("Generated"));
	REQUIRE(shader.Found);
	CHECK(shader.Code == "#version 450\nvoid main() {}");
	CHECK(shader.Revision > 0);
}
