// What can be checked about the light path without a GPU, which is less than it
// first looks.
//
// **`MAX_SCENE_LIGHTS` is spelled twice** — once here as a C++ constant and once
// as `MAX_LIGHTS` in `shaders/opaque.frag` — because there is no header a GLSL
// file can include and the staged shaders are SPIR-V by the time a test could
// read them. The first version of this suite tried to read the source out of the
// staged tree and compare, which does not work: what is staged is compiled.
//
// So this is **not** the check `AGENTS.md` rule 6 asks for, and saying so is the
// point of this paragraph. The rule says a constraint the build does not check is
// documentation, and this one is documentation — filed as `DEFERRED.md` D00029,
// where the fix is to inject the count as a `-D` at shader compile time so the
// number has one home.
//
// What *is* checked here is the half that is real: the defaults a light carries,
// which decide whether `Instance.new("PointLight")` lights anything at all.

#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.scenelights")

TEST_CASE("a scene light's defaults are the ones a point light wants", "[render]") {
	const engine::render::SceneLight light;

	// **-1 is the value that means "no cone", and it is a default rather than a
	// sentinel a caller has to know.** The shader tests `cosine > -1.0` to decide
	// whether to clip, so a light left alone is a point light and needs no branch
	// on its kind — which is what lets `scene::Light` be one component behind
	// three classes.
	REQUIRE(light.ConeCosine == -1.0f);

	// A range of zero is a light the shader rejects at its first compare, so the
	// default has to be positive or `Instance.new("PointLight")` would be dark
	// until somebody set one.
	REQUIRE(light.Range > 0.0f);

	// Brightness is folded into the colour by `client::CollectLights`, so the
	// default here is the unmultiplied white a light with no author starts from.
	REQUIRE(light.Colour.R == 1.0f);
}

TEST_CASE("the light cap is a number the renderer can act on", "[render]") {
	// Not a tautology: this is the constant a caller sizes its own list against,
	// and a zero or a wildly large value would each be a different kind of broken
	// — one drops every light, the other overflows a uniform buffer at the
	// driver rather than here.
	REQUIRE(engine::render::MAX_SCENE_LIGHTS > 0);
	REQUIRE(engine::render::MAX_SCENE_LIGHTS <= 64);
}
