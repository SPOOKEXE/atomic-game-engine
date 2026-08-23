// What `Renderer::CaptureSceneTexture` refuses, which is all of it that a
// machine with no GPU can see.
//
// **The success path needs a device and is not faked here.** A capture is a
// texture created on the device and a blit submitted to its queue; standing a
// fake in front of that would test the fake. What *is* checkable without one is
// the half that decides whether a caller is about to be handed a texture it now
// owns - and every one of those refusals has to leave the caller exactly as it
// found them, because `TextureTable::Adopt` transfers ownership and a refusal
// that had already half-transferred would be a double free or a leak.
//
// The drawn-into path is exercised by running the studio: hovering a mesh row
// captures the slot and every other row then shows the picture. See
// `DEFERRED.md` D00033.

#include <engine/core/Name.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.scenecapture")

TEST_CASE("a capture with no device is refused rather than attempted", "[render]") {
	// No `Initialise`, so there is no device - the same arrangement
	// `tests/Passes.cpp` uses to exercise a contract on a build machine with no
	// GPU.
	engine::render::Renderer renderer;

	// **Refused, not crashed.** This is reachable in a real editor: the studio
	// asks for a capture from inside its draw, and a headless run has no device
	// at all.
	CHECK_FALSE(renderer.CaptureSceneTexture(0, engine::core::Name("studio.thumbnail/fox.amesh")));
}

TEST_CASE("a capture under an invalid name is refused", "[render]") {
	engine::render::Renderer renderer;

	// A default `Name` is the "nothing" value, and publishing a texture under it
	// would put an entry in the table that no lookup could ever name again -
	// device memory with no way to reach it and no way to drop it.
	CHECK_FALSE(renderer.CaptureSceneTexture(0, engine::core::Name()));
}

TEST_CASE("a capture from a slot that was never drawn into is refused", "[render]") {
	engine::render::Renderer renderer;

	// **A slot index past the end is the ordinary case rather than an error.**
	// Slots are created as they are drawn into, so asking about one the frame
	// never used is what a caller does when a preview has not had its turn in
	// the rotation yet - and the honest answer is "nothing to copy" rather than
	// a blank texture, which would cache an empty picture for ever.
	CHECK_FALSE(renderer.CaptureSceneTexture(64, engine::core::Name("studio.thumbnail/late.amesh")));
}

TEST_CASE("file capture requests expose and clear their pending state without a device", "[render]") {
	engine::render::Renderer renderer;
	CHECK_FALSE(renderer.CapturePending());

	renderer.RequestSceneCapture("scene.bmp", 3);
	CHECK(renderer.CapturePending());
	renderer.RequestSceneCapture({});
	CHECK_FALSE(renderer.CapturePending());

	renderer.RequestWindowCapture("studio.bmp");
	CHECK(renderer.CapturePending());
	renderer.RequestWindowCapture({});
	CHECK_FALSE(renderer.CapturePending());
}
