#include <engine/ecs/Store.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/content/MaterialGraphs.hpp>
#include <engine/scene/content/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.materialgraphs")

using namespace engine;
using namespace engine::scene;

namespace {
	// The dissolve the header shows.
	constexpr const char *DISSOLVE = R"(# A dissolve
parameter float amount = 0.3
parameter vec3 edge = 1, 0.45, 0.1
texture pattern
keyword GLOW
base default
node n = noise uv 12
node cut = step amount n
node rim = sub 1 (smoothstep amount (add amount 0.06) n)
output Albedo = mix base.Albedo edge rim
output@GLOW Emissive = mul edge (mul rim 4)
discard cut
)";

	std::string ErrorOf(const char *document) {
		return CompileMaterialGraph(document, "g").Error;
	}
}

TEST_CASE("a dissolve graph compiles to a surface with its declarations", "[scene][material]") {
	const auto compiled = CompileMaterialGraph(DISSOLVE, "dissolve");
	INFO(compiled.Error);
	REQUIRE(compiled.Error.empty());
	const std::string &source = compiled.Source;
	CHECK(source.find("#pragma parameter float amount = 0.3") != std::string::npos);
	CHECK(source.find("#pragma texture pattern") != std::string::npos);
	CHECK(source.find("#pragma keyword GLOW") != std::string::npos);
	CHECK(source.find("void Surface(SurfaceInput i, inout SurfaceOutput o)") != std::string::npos);
	CHECK(source.find("DefaultSurface(i, o)") != std::string::npos);
	CHECK(source.find("#if GLOW") != std::string::npos);
	CHECK(source.find("discard") != std::string::npos);
	// Each statement names its graph line, for compile errors.
	CHECK(source.find("#line 7 \"dissolve\"") != std::string::npos);
}

TEST_CASE("a graph names the line of what is wrong with it", "[scene][material]") {
	CHECK(ErrorOf("node a = add uv normal\n").starts_with("1: "));
	CHECK(ErrorOf("node a = add uv normal\n").find("vec3") != std::string::npos);
	CHECK(ErrorOf("\nnode a = mul missing 2\n").starts_with("2: nothing called 'missing'"));
	CHECK_FALSE(ErrorOf("node a = uv.xyz\n").empty());
	CHECK_FALSE(ErrorOf("output Albedo = tint.xy\n").empty());
	CHECK_FALSE(ErrorOf("output Shine = 1\n").empty());
	CHECK_FALSE(ErrorOf("discard uv\n").empty());
	CHECK_FALSE(ErrorOf("node a = sample nothing uv\n").empty());
	CHECK_FALSE(ErrorOf("node a = 1\nparameter float late = 1\n").empty());
	CHECK_FALSE(ErrorOf("keyword A\nkeyword B\nkeyword C\nkeyword D\nkeyword E\n").empty());
	CHECK_FALSE(ErrorOf("node a = 1\nnode a = 2\n").empty());
	CHECK(ErrorOf("node a = vec3 uv 1\noutput Albedo = a\n").empty());
	CHECK(ErrorOf("output Roughness = 0.4\n").empty());
}

TEST_CASE("a Material names a graph as it names a script", "[scene][material]") {
	RegisterSceneComponents();
	RegisterSceneClasses();
	ecs::Store store("graphs");
	const auto graph = store.CreateInstance(MaterialGraphClass(), "Dissolve");
	REQUIRE(SetShaderSource(store, graph, DISSOLVE));
	const ShaderText text = ShaderTextOf(store, core::Name("Dissolve"));
	REQUIRE(text.Found);
	CHECK(text.Source == graph);
	CHECK(text.Code.find("void Surface(") != std::string::npos);
	// An edit that does not compile becomes a surface that says why.
	REQUIRE(SetShaderSource(store, graph, "node a = add uv normal\n"));
	const ShaderText broken = ShaderTextOf(store, core::Name("Dissolve"));
	CHECK(broken.Code.starts_with("#error 1: "));
	CHECK(broken.Revision > text.Revision);
}
