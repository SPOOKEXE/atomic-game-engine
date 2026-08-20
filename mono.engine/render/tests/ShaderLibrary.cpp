// The route from a name in a world to SPIR-V, and the order it resolves in.
//
// **No device anywhere in this file, which is what makes the route testable at
// all.** `render/AGENTS.md` is explicit that a header needing a GPU has no unit
// suite, and this one deliberately does not need one: `libshaderc` compiles
// without a driver, a staged `.spv` is a file, and everything in between is a
// name lookup. What a device is needed for is turning the words into a pipeline,
// and that is the one thing `ShaderLibrary` does not do.
//
// The half of D00110 that lives here is the resolution order - a `ShaderScript`
// in the world first, an engine built-in second, a diagnostic third - because
// that is what makes a default shader file reachable by name rather than a file
// nothing loads.

#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.shaderlibrary")

namespace fs = std::filesystem;

using engine::core::Name;
using engine::core::Paths;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;
using engine::render::BuiltInShaderNames;
using engine::render::ShaderLibrary;
using engine::render::ShaderModule;
using engine::scene::MaterialClass;
using engine::scene::MaterialRef;
using engine::scene::SetShaderSource;
using engine::scene::ShaderScriptClass;

namespace {
	// The first word of a SPIR-V module - what separates "some bytes" from
	// "SPIR-V", the same check `tests/ShaderCompiler.cpp` makes.
	constexpr uint32_t SPIRV_MAGIC = 0x07230203u;

	// A fragment shader that compiles. Deliberately trivial: what is being
	// tested here is the route, and `tests/ShaderCompiler.cpp` owns the
	// compiler's own behaviour.
	constexpr const char *VALID = R"(#version 450
layout(location = 0) out vec4 outColour;
void main() { outColour = vec4(1.0); }
)";

	// The assets override is process-wide state, so a case that leaves it set
	// breaks every later one - `engine.resources.shaders` carries the guard.
	struct Staged {
		fs::path Root;

		explicit Staged(const char *leaf) {
			Root = fs::temp_directory_path() / leaf;
			fs::remove_all(Root);
			fs::create_directories(Root / "shaders" / "resources");
			Paths::SetAssetsOverride(Root);
		}

		~Staged() {
			Paths::SetAssetsOverride({});
			std::error_code ignored;
			fs::remove_all(Root, ignored);
		}

		// Writes a stand-in for what `glslc` stages, so the built-in half of the
		// resolution can be exercised without a build having run.
		void Write(const char *file, uint32_t first) const {
			const std::vector<uint32_t> words{first, 0x00010000u, 0u, 1u};
			std::ofstream out(
				engine::resources::Shader(file, engine::resources::ShaderForm::SpirV), std::ios::binary
			);
			out.write(reinterpret_cast<const char *>(words.data()), 16);
		}
	};

	Store Fresh(const char *name) {
		engine::scene::RegisterSceneComponents();
		engine::scene::RegisterSceneClasses();
		engine::gui::RegisterGuiClasses();
		ShaderScriptClass();
		return Store(name);
	}

	// A material naming `shader`, so the world asks for it.
	void Select(Store &store, const char *shader) {
		const Entity material = store.CreateInstance(MaterialClass(), "Material");
		REQUIRE(material != NULL_ENTITY);
		store.GetMutable<MaterialRef>(material)->Shader = Name(shader);
	}

	// An `ImageLabel` naming `shader` directly, so the world asks for it the
	// other way `gui::DemandedShaders` covers.
	void SelectImage(Store &store, const char *shader) {
		const Entity label = store.CreateInstance(engine::gui::GuiClass("ImageLabel"), "Picture");
		REQUIRE(label != NULL_ENTITY);
		store.GetMutable<engine::gui::Picture>(label)->Shader = Name(shader);
	}

	// A shader script called `name` holding `code`.
	Entity Author(Store &store, const char *name, const char *code) {
		const Entity script = store.CreateInstance(ShaderScriptClass(), name);
		REQUIRE(script != NULL_ENTITY);
		REQUIRE(SetShaderSource(store, script, code));
		return script;
	}
}

TEST_CASE("a shader script in the world is compiled by name", "[render][shaders]") {
	Store store = Fresh("library.script");
	Author(store, "Toon", VALID);
	Select(store, "Toon");

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);

	const ShaderModule *module = library.Find(Name("Toon"));
	REQUIRE(module != nullptr);
	REQUIRE(module->Error.empty());
	REQUIRE_FALSE(module->SpirV.empty());
	REQUIRE(module->SpirV.front() == SPIRV_MAGIC);
	REQUIRE_FALSE(module->BuiltIn);
}

TEST_CASE("a world's postprocess shader is compiled the same door a material uses", "[render][shaders]") {
	Store store = Fresh("library.postprocess");
	Author(store, "Sepia", VALID);
	engine::scene::SetPostProcessShader(store, Name("Sepia"));

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);

	const ShaderModule *module = library.Find(Name("Sepia"));
	REQUIRE(module != nullptr);
	REQUIRE(module->Error.empty());
	REQUIRE_FALSE(module->SpirV.empty());
	REQUIRE_FALSE(module->BuiltIn);

	// Switching away stops asking for it, exactly as dropping a material's
	// selection does.
	engine::scene::SetPostProcessShader(store, Name{});
	REQUIRE(library.Refresh(store) == 1);
	CHECK(library.Find(Name("Sepia")) == nullptr);
}

TEST_CASE(
	"an ImageLabel's own shader is compiled by name, the same door a material uses", "[render][shaders]"
) {
	Store store = Fresh("library.picture");
	Author(store, "Toon", VALID);
	SelectImage(store, "Toon");

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);

	const ShaderModule *module = library.Find(Name("Toon"));
	REQUIRE(module != nullptr);
	REQUIRE(module->Error.empty());
	REQUIRE_FALSE(module->SpirV.empty());
	REQUIRE_FALSE(module->BuiltIn);
}

TEST_CASE("a material and an ImageLabel naming the same shader share one module", "[render][shaders]") {
	Store store = Fresh("library.shared");
	Author(store, "Toon", VALID);
	Select(store, "Toon");
	SelectImage(store, "Toon");

	ShaderLibrary library;

	// **One, not two** - the whole point of resolving both through one name
	// space is that a scene and its interface asking for the same shader
	// compile it once.
	REQUIRE(library.Refresh(store) == 1);
	REQUIRE(library.Find(Name("Toon")) != nullptr);
}

TEST_CASE("an unchanged script is not compiled twice", "[render][shaders]") {
	Store store = Fresh("library.revision");
	const Entity toon = Author(store, "Toon", VALID);
	Select(store, "Toon");

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);

	// **The whole reason `ShaderSource::Revision` exists.** A refresh that
	// recompiled every named shader every frame would put a GLSL front end in
	// the frame loop, which is what the revision compare is there to avoid.
	REQUIRE(library.Refresh(store) == 0);

	REQUIRE(SetShaderSource(store, toon, VALID));
	REQUIRE(library.Refresh(store) == 1);
}

TEST_CASE("a broken script keeps its diagnostic and does not stop the rest", "[render][shaders]") {
	Store store = Fresh("library.broken");
	Author(store, "Broken", "#version 450\nvoid main() { this is not glsl }\n");
	Author(store, "Fine", VALID);
	Select(store, "Broken");
	Select(store, "Fine");

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 2);

	// **A user shader failing is a diagnostic string, not a fatal**, which is
	// `render/AGENTS.md`'s split between the two compilers stated as a test. The
	// assertion that matters is that the error is non-empty: a compiler that
	// reported success unconditionally would pass every other case here.
	const ShaderModule *broken = library.Find(Name("Broken"));
	REQUIRE(broken != nullptr);
	REQUIRE_FALSE(broken->Error.empty());
	REQUIRE(broken->SpirV.empty());

	const ShaderModule *fine = library.Find(Name("Fine"));
	REQUIRE(fine != nullptr);
	REQUIRE(fine->Error.empty());
	REQUIRE_FALSE(fine->SpirV.empty());
}

TEST_CASE("a built-in is what a name resolves to when no script holds it", "[render][shaders]") {
	const Staged staged("atomic-shaderlibrary-builtin");

	// Every name the engine ships has a file, and the list is what makes those
	// files reachable rather than staged and unloaded. A default nothing can
	// name is the trap `resources/AGENTS.md` refuses.
	REQUIRE_FALSE(BuiltInShaderNames().empty());
	for (const std::string_view name : BuiltInShaderNames()) {
		staged.Write((std::string(name) + ".frag").c_str(), SPIRV_MAGIC);
	}

	Store store = Fresh("library.builtin");
	const std::string first(BuiltInShaderNames().front());
	Select(store, first.c_str());

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);

	const ShaderModule *module = library.Find(Name(first));
	REQUIRE(module != nullptr);
	REQUIRE(module->BuiltIn);
	REQUIRE(module->Error.empty());
	REQUIRE(module->SpirV.front() == SPIRV_MAGIC);

	// A built-in cannot change while the engine runs - `glslc` compiled it
	// during the build - so a second refresh reloads nothing.
	REQUIRE(library.Refresh(store) == 0);
}

TEST_CASE("a script overrides the built-in of the same name", "[render][shaders]") {
	const Staged staged("atomic-shaderlibrary-override");

	const std::string first(BuiltInShaderNames().front());
	staged.Write((first + ".frag").c_str(), SPIRV_MAGIC);

	Store store = Fresh("library.override");
	Select(store, first.c_str());

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);
	REQUIRE(library.Find(Name(first))->BuiltIn);

	// **The order is the design.** An author's script replaces the engine's
	// shader of that name and nothing else changes - which is what makes a
	// built-in a default rather than a separate mechanism.
	Author(store, first.c_str(), VALID);
	REQUIRE(library.Refresh(store) == 1);

	const ShaderModule *module = library.Find(Name(first));
	REQUIRE(module != nullptr);
	REQUIRE_FALSE(module->BuiltIn);
	REQUIRE(module->SpirV.front() == SPIRV_MAGIC);
}

TEST_CASE("a name nothing holds is reported rather than ignored", "[render][shaders]") {
	const Staged staged("atomic-shaderlibrary-missing");

	Store store = Fresh("library.missing");
	Select(store, "Nonexistent");

	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 1);

	// **A misspelled shader is a diagnostic, not silence.** The same argument
	// `render::MissingTexture` makes for geometry: a typo and a shader somebody
	// deliberately left as the engine's default look identical from the frame,
	// so the one that is a mistake has to say so somewhere.
	const ShaderModule *module = library.Find(Name("Nonexistent"));
	REQUIRE(module != nullptr);
	REQUIRE(module->SpirV.empty());
	REQUIRE_FALSE(module->Error.empty());

	// Reported once. A refresh per frame that re-reported it would be a log
	// line per frame for one typo.
	REQUIRE(library.Refresh(store) == 0);
}

TEST_CASE("the library holds what the world asks for and nothing else", "[render][shaders]") {
	Store store = Fresh("library.demand");
	Author(store, "Toon", VALID);

	// A script nobody selected is text somebody is still writing.
	ShaderLibrary library;
	REQUIRE(library.Refresh(store) == 0);
	REQUIRE(library.Size() == 0);

	Select(store, "Toon");
	REQUIRE(library.Refresh(store) == 1);
	REQUIRE(library.Size() == 1);

	// **Dropped when nothing names it any more**, so the library is a picture of
	// what the world wants rather than of everything it has ever wanted. The
	// alternative grows for the life of a session on a world somebody is
	// editing.
	store.Each<const MaterialRef>([&store](Entity entity, const MaterialRef &) {
		store.GetMutable<MaterialRef>(entity)->Shader = Name{};
	});
	REQUIRE(library.Refresh(store) == 1);
	REQUIRE(library.Size() == 0);
	REQUIRE(library.Find(Name("Toon")) == nullptr);
}
