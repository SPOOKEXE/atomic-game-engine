// Whether the editor is allowed to write its viewport camera's lens.
//
// **The bug: three properties of an instance an author can select could not be
// edited at all.** `EnsureViewerCamera` assigned the whole `scene::Camera`
// component every frame, so typing 35 into `FieldOfView` in the properties panel
// held for one frame and came back as 69.9008 degrees - 1.22 radians in degrees,
// which is the default lens and therefore the fingerprint of the editor rather
// than of a script. `NearPlane` and `FarPlane` went the same way, and it held in
// Edit, Run and Play alike.
//
// It was reported from Run mode because that is where somebody was looking, and
// measured in all three: a headless editor driven over its control port set
// `FieldOfView` to 35 and read 69.9008 back a second later in every mode.
//
// **Deleting the write is not the fix**, which is why this is a decision worth a
// file. The far plane is stretched to the fly speed, so an author flying fast
// across a large world would find it clipping and would have to know to go and
// raise a field they have never opened. The editor keeps the lens until somebody
// takes it, and then leaves it alone.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <studio/ViewerLens.hpp>

TEST_SUITE_ID("studio.viewerlens")

using engine::scene::Camera;
using studio::ViewerLensToWrite;

namespace {
	// What the editor derives: its default, with the far plane stretched to the
	// fly speed. `Editor::PresentWorld` builds exactly this.
	Camera Derived(float farPlane = 2000.0f) {
		Camera lens;
		lens.FarPlane = farPlane;
		return lens;
	}
}

TEST_CASE("a freshly minted camera takes the editor's lens", "[studio][viewerlens]") {
	// Nothing written yet, so the lens on the instance is whatever
	// `CreateInstance` left there and the editor's is better: it carries the far
	// plane the fly speed asks for. A rule that refused here would leave every
	// new panel clipping at 500 metres however fast somebody flew.
	const std::optional<Camera> written = ViewerLensToWrite(Camera{}, std::nullopt, Derived());

	REQUIRE(written.has_value());
	CHECK(written->FarPlane == 2000.0f);
	CHECK(written->FieldOfViewRadians == Camera{}.FieldOfViewRadians);
}

TEST_CASE("the fly speed still moves the far plane while nobody else has", "[studio][viewerlens]") {
	// The editor's own value on the instance, and a new one derived because
	// somebody sped up. That is not a fight, and refusing it would be a far
	// plane frozen at whatever speed the panel opened with.
	const Camera ours = Derived(2000.0f);
	const std::optional<Camera> written = ViewerLensToWrite(ours, ours, Derived(8000.0f));

	REQUIRE(written.has_value());
	CHECK(written->FarPlane == 8000.0f);
}

TEST_CASE("an unchanged lens is not written again", "[studio][viewerlens]") {
	// **A dirty mark per panel per frame on a row nothing has moved**, which the
	// explorer and the edit stream both pay for. The value being right is not a
	// reason to write it.
	const Camera ours = Derived();
	CHECK_FALSE(ViewerLensToWrite(ours, ours, ours).has_value());
}

TEST_CASE("a field somebody edited is left alone", "[studio][viewerlens]") {
	const Camera ours = Derived();

	// 35 degrees, which is the number the report was made with.
	Camera edited = ours;
	edited.FieldOfViewRadians = 0.6109f;

	CHECK_FALSE(ViewerLensToWrite(edited, ours, ours).has_value());

	// **And still refused when the editor has a new value it would like**, which
	// is the case the whole thing turns on: the fly speed changing must not be a
	// back door to overwriting the field. This is what used to happen every
	// frame, because the fly speed is read every frame.
	CHECK_FALSE(ViewerLensToWrite(edited, ours, Derived(8000.0f)).has_value());

	// The other two fields are the lens as much as the first one is.
	Camera nearer = ours;
	nearer.NearPlane = 0.01f;
	CHECK_FALSE(ViewerLensToWrite(nearer, ours, Derived(8000.0f)).has_value());

	Camera shorter = ours;
	shorter.FarPlane = 120.0f;
	CHECK_FALSE(ViewerLensToWrite(shorter, ours, Derived(8000.0f)).has_value());
}

TEST_CASE("a refusal stays refused frame after frame", "[studio][viewerlens]") {
	// **The property that makes this usable rather than merely correct once.**
	// There is no flag saying the author owns it; what says so is that `written`
	// is recorded *only when a write happens*, so an author's value goes on
	// disagreeing with it for as long as it stands. Modelled here as the loop
	// the editor actually runs.
	std::optional<Camera> written;
	Camera onInstance;

	const auto frame = [&](const Camera &derived) {
		if (const auto next = ViewerLensToWrite(onInstance, written, derived)) {
			onInstance = *next;
			written = *next;
			return true;
		}
		return false;
	};

	// The first frame seeds it, the second has nothing to say.
	CHECK(frame(Derived()));
	CHECK_FALSE(frame(Derived()));

	// Somebody types 35 into the panel.
	onInstance.FieldOfViewRadians = 0.6109f;

	// Sixty frames of flying about at changing speeds, and it holds.
	for (int tick = 0; tick < 60; tick++) {
		CHECK_FALSE(frame(Derived(2000.0f + static_cast<float>(tick) * 100.0f)));
	}
	CHECK(onInstance.FieldOfViewRadians == 0.6109f);
}

TEST_CASE("a re-minted camera hands the lens back to the editor", "[studio][viewerlens]") {
	// A snapshot restore replaces every entity and a panel repointed at another
	// world mints a new camera; `Editor::ReleaseViewerCamera` clears the record
	// with it, which is this. The row somebody edited is gone, so there is
	// nobody left to be fighting.
	Camera edited;
	edited.FieldOfViewRadians = 0.6109f;

	const std::optional<Camera> written = ViewerLensToWrite(edited, std::nullopt, Derived());
	REQUIRE(written.has_value());
	CHECK(written->FieldOfViewRadians == Camera{}.FieldOfViewRadians);
}
