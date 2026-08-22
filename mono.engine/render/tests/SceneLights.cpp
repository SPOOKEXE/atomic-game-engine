// What can be checked about the light path without a GPU, which is less than it
// first looks.
//
// **`MAX_SCENE_LIGHTS` is no longer spelled twice, and that is why there is no
// case here comparing it to anything.** It used to be a C++ constant and a
// `MAX_LIGHTS` in `shaders/opaque.frag` that agreed only because somebody wrote
// both. The first version of this suite tried to read the staged shader back and
// compare, which cannot work: what is staged is SPIR-V and the constant is folded
// away by then.
//
// Closed at v0.10 by removing the second spelling rather than by checking it -
// `mono.engine/render/CMakeLists.txt` reads the value out of `Renderer.hpp` and
// passes `-DMAX_LIGHTS` to glslc, so the shader has no literal to disagree with.
// **A constraint made impossible beats one that is tested**, which is the shape
// `AGENTS.md` rule 6 is really asking for; a test would only have told us
// afterwards, and only if it could read the number, which it could not.
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
	// on its kind - which is what lets `scene::Light` be one component behind
	// three classes.
	REQUIRE(light.ConeCosine == -1.0f);

	// A range of zero is a light the shader rejects at its first compare, so the
	// default has to be positive or `Instance.new("PointLight")` would be dark
	// until somebody set one.
	REQUIRE(light.Range > 0.0f);

	// Brightness is folded into the colour by `render::CollectLights`, so the
	// default here is the unmultiplied white a light with no author starts from.
	REQUIRE(light.Colour.R == 1.0f);
}

TEST_CASE("the light cap is a number the renderer can act on", "[render]") {
	// Not a tautology: this is the constant a caller sizes its own list against,
	// and a zero or a wildly large value would each be a different kind of broken
	// - one drops every light, the other overflows a uniform buffer at the
	// driver rather than here.
	REQUIRE(engine::render::MAX_SCENE_LIGHTS > 0);
	REQUIRE(engine::render::MAX_SCENE_LIGHTS <= 64);
}
