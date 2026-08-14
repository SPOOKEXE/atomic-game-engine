// The half of a shader a world can hold, and the half it cannot.
//
// **What is pinned here is the path `render/ShaderCompiler.hpp` was written
// against and that did not exist.** That header names a `ShaderScript` whose
// revision changed as the reason a runtime compiler ships; `docs/DEFERRED.md`
// D00110 records that there was no class, no property and no route from a world
// to a compile. So the four things a compile needs before it can happen are the
// four things here: an instance that holds GLSL, a counter that moves when it is
// edited, a name a material selects it by, and a resolve that carries the
// selection onto the part the draw path reads.
//
// Nothing here compiles anything. Compilation needs `libshaderc` and lives at
// L12 - `mono.engine/render/tests/ShaderLibrary.cpp` is the other end.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.scene.shaders")

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::PropertyDescriptor;
using engine::ecs::Store;
using engine::scene::DemandedShaders;
using engine::scene::MaterialClass;
using engine::scene::MaterialRef;
using engine::scene::ResolveMaterials;
using engine::scene::SetShaderSource;
using engine::scene::ShaderScriptClass;
using engine::scene::ShaderScriptNamed;
using engine::scene::ShaderSource;
using engine::scene::ShaderText;
using engine::scene::ShaderTextOf;
using engine::scene::SurfaceAppearance;

namespace {
	// Registered before the store exists - `tests/Materials.cpp` carries the
	// reason: a resource keyed by a component id minted before the explicit
	// registration lands takes the compiler's spelling of the type.
	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		engine::scene::RegisterSceneClasses();
		ShaderScriptClass();
		return Store(name);
	}

	// One property of a class, by name, or null.
	const PropertyDescriptor *PropertyOf(engine::ecs::ClassId owner, const char *name) {
		for (const PropertyDescriptor &property : Classes::Describe(owner).Properties) {
			if (property.Spelling == name) {
				return &property;
			}
		}
		return nullptr;
	}

	// A shader script called `name` holding `code`.
	Entity Author(Store &store, const char *name, const char *code) {
		const Entity script = store.CreateInstance(ShaderScriptClass(), name);
		REQUIRE(script != NULL_ENTITY);
		REQUIRE(SetShaderSource(store, script, code));
		return script;
	}
}

TEST_CASE("a world can hold a shader script and find it by name", "[scene][shaders]") {
	Store store = Fresh("shaders.hold");

	// The class is an `Instance`, not a `PVInstance`: it has no place in the
	// world, so nothing gives it one.
	REQUIRE(Classes::IsA(ShaderScriptClass(), Classes::Find(Name("Instance"))));
	REQUIRE_FALSE(Classes::IsA(ShaderScriptClass(), Classes::Find(Name("PVInstance"))));

	const Entity toon = Author(store, "Toon", "void main() {}");

	REQUIRE(ShaderScriptNamed(store, Name("Toon")) == toon);
	REQUIRE(ShaderScriptNamed(store, Name("Unlit")) == NULL_ENTITY);
	REQUIRE(ShaderScriptNamed(store, Name{}) == NULL_ENTITY);

	// **`Found` and an empty `Code` are different answers.** A script somebody
	// created and has not typed into is a shader to report an error about; no
	// script at all is a name to go looking for a built-in under.
	const ShaderText held = ShaderTextOf(store, Name("Toon"));
	REQUIRE(held.Found);
	REQUIRE(held.Code == "void main() {}");
	REQUIRE_FALSE(ShaderTextOf(store, Name("Unlit")).Found);
}

TEST_CASE("editing a shader script moves its revision", "[scene][shaders]") {
	Store store = Fresh("shaders.revision");
	const Entity toon = Author(store, "Toon", "one");

	const uint32_t first = ShaderTextOf(store, Name("Toon")).Revision;
	REQUIRE(first > 0);

	REQUIRE(SetShaderSource(store, toon, "two"));
	const ShaderText second = ShaderTextOf(store, Name("Toon"));
	REQUIRE(second.Code == "two");

	// **The whole reason the counter exists.** A library holding the revision
	// it last compiled decides whether to recompile with an integer compare; a
	// counter that did not move on an edit is an edit that never reaches a
	// frame, which is a shader somebody debugs by restarting the engine.
	REQUIRE(second.Revision > first);

	// It counts writes rather than differences, so it moves for identical text
	// too. One writer and one meaning - `SetShaderSource` carries the argument.
	REQUIRE(SetShaderSource(store, toon, "two"));
	REQUIRE(ShaderTextOf(store, Name("Toon")).Revision > second.Revision);

	// An entity with no `ShaderSource` is refused rather than given one.
	REQUIRE_FALSE(SetShaderSource(store, store.CreateInstance(MaterialClass(), "Material"), "x"));
}

TEST_CASE("the source property is the only thing that moves the revision", "[scene][shaders]") {
	Store store = Fresh("shaders.property");
	const Entity toon = Author(store, "Toon", "one");

	const PropertyDescriptor *source = PropertyOf(ShaderScriptClass(), "Source");
	REQUIRE(source != nullptr);
	REQUIRE(source->Type == engine::ecs::PropertyType::String);

	// **Not scriptable**, which is `LuaSourceContainer::Source`'s rule: a script
	// that could write another instance's shader could put arbitrary GLSL in
	// front of the driver. An author still can, through this descriptor.
	REQUIRE_FALSE(source->Scriptable);

	const uint32_t before = ShaderTextOf(store, Name("Toon")).Revision;

	std::string written = "edited";
	REQUIRE(source->Set(store, toon, &written));

	std::string read;
	REQUIRE(source->Get(store, toon, &read));
	REQUIRE(read == "edited");
	REQUIRE(ShaderTextOf(store, Name("Toon")).Revision > before);

	// Readable and not writable: it is what a consumer acts on, so hiding it
	// would make a shader that will not recompile undiagnosable, and writing it
	// would be a way to make the library skip a real edit.
	const PropertyDescriptor *revision = PropertyOf(ShaderScriptClass(), "Revision");
	REQUIRE(revision != nullptr);
	REQUIRE_FALSE(revision->Writable);
	REQUIRE(revision->Set == nullptr);
}

TEST_CASE("a material selects a shader by name and the part reads it", "[scene][shaders]") {
	Store store = Fresh("shaders.select");

	// The property an author uses. `Shader` rather than `ShaderId`, because it
	// is not a name a publisher owns - it may be a script in this very world.
	const PropertyDescriptor *shader = PropertyOf(MaterialClass(), "Shader");
	REQUIRE(shader != nullptr);
	REQUIRE(shader->Type == engine::ecs::PropertyType::Name);

	const Entity part = engine::scene::MakePart(store, {});
	REQUIRE(part != NULL_ENTITY);

	const Entity material = store.CreateInstance(MaterialClass(), "Material");
	REQUIRE(material != NULL_ENTITY);
	REQUIRE(store.SetParent(material, part));

	Name selected("Toon");
	REQUIRE(shader->Set(store, material, &selected));

	// Nothing has resolved yet, so the part still says nothing.
	REQUIRE_FALSE(store.Get<SurfaceAppearance>(part)->Shader.IsValid());

	REQUIRE(ResolveMaterials(store) == 1);
	REQUIRE(store.Get<SurfaceAppearance>(part)->Shader == Name("Toon"));

	// **And it is cleared when the material goes**, exactly as the maps are: a
	// part still drawn by a deleted material's shader is the one thing about it
	// that would remain visible after its textures had gone.
	store.DestroyInstance(material);
	REQUIRE(ResolveMaterials(store) == 0);
	REQUIRE_FALSE(store.Get<SurfaceAppearance>(part)->Shader.IsValid());
}

TEST_CASE("a world reports the shaders its materials ask for", "[scene][shaders]") {
	Store store = Fresh("shaders.demand");

	// **A script nobody selected is not demanded.** It is text somebody is
	// still writing, and compiling it every frame would charge the frame for an
	// editor's open buffer.
	Author(store, "Unused", "void main() {}");

	std::vector<Name> demanded;
	REQUIRE(DemandedShaders(store, demanded) == 0);

	const auto select = [&store](const char *name) {
		const Entity material = store.CreateInstance(MaterialClass(), "Material");
		REQUIRE(material != NULL_ENTITY);
		store.GetMutable<MaterialRef>(material)->Shader = Name(name);
	};

	select("Toon");
	select("Unlit");

	// Two materials naming one shader is one compile, which is why this
	// deduplicates rather than handing back a row per material.
	select("Toon");

	REQUIRE(DemandedShaders(store, demanded) == 2);
	REQUIRE(std::find(demanded.begin(), demanded.end(), Name("Toon")) != demanded.end());
	REQUIRE(std::find(demanded.begin(), demanded.end(), Name("Unlit")) != demanded.end());

	// A name a world holds no script for is still demanded: it may be a shader
	// the engine ships, and only a client-tier library can tell.
	REQUIRE(ShaderScriptNamed(store, Name("Toon")) == NULL_ENTITY);
}
