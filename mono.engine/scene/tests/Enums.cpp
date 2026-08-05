#include <engine/scene/Enums.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

TEST_SUITE_ID("engine.scene.enums")

using engine::scene::BodyKind;
using engine::scene::Describe;
using engine::scene::NormalId;
using engine::scene::NormalOf;
using engine::scene::ShapeKind;

TEST_CASE("every body kind has a name", "[scene][enums]") {
	// A `?` in a log is a value somebody has to trace back to a switch, which
	// is the one thing they are reading the log to avoid.
	for (const BodyKind kind : {BodyKind::Static, BodyKind::Kinematic, BodyKind::Dynamic}) {
		CHECK(std::string(Describe(kind)) != "?");
	}
}

TEST_CASE("every shape kind has a name", "[scene][enums]") {
	for (const ShapeKind kind : {ShapeKind::Box, ShapeKind::Sphere, ShapeKind::Cylinder}) {
		CHECK(std::string(Describe(kind)) != "?");
	}
}

TEST_CASE("names are distinct", "[scene][enums]") {
	// Two kinds sharing a name makes a log ambiguous in exactly the situation
	// somebody is reading it.
	const std::vector<std::string_view> names{
		Describe(BodyKind::Static),
		Describe(BodyKind::Kinematic),
		Describe(BodyKind::Dynamic),
		Describe(ShapeKind::Box),
		Describe(ShapeKind::Sphere),
		Describe(ShapeKind::Cylinder),
	};

	const std::unordered_set<std::string_view> unique(names.begin(), names.end());
	CHECK(unique.size() == names.size());
}

TEST_CASE("both enums are one byte", "[scene][enums]") {
	// `RigidBody` and `Collider` are laid out around these being a byte each,
	// with named padding filling the rest of the word. Widening one reopens an
	// unnamed hole in a component that a snapshot writes raw, and the symptom
	// is `just determinism` failing a long way from here.
	CHECK(sizeof(BodyKind) == 1);
	CHECK(sizeof(ShapeKind) == 1);
	CHECK(static_cast<uint8_t>(BodyKind::Static) == 0);
	CHECK(static_cast<uint8_t>(ShapeKind::Box) == 0);
}

// --- faces ------------------------------------------------------------------

TEST_CASE("every face has a name, and they are Roblox's", "[scene][enums]") {
	// Not "?" for any of them: the fallthrough is what a missing case produces,
	// and a face named "?" would be registered as an enum member spelled that
	// way rather than failing.
	for (uint8_t index = 0; index < 6; index++) {
		INFO("ordinal " << static_cast<int>(index));
		CHECK(std::string(Describe(static_cast<NormalId>(index))) != "?");
	}

	// **Roblox's spelling, which is the one a ported script uses.** `Top` and
	// `Bottom` rather than Up and Down — a second name for one face is the
	// duplicate `scene/AGENTS.md` calls the most expensive kind of debt.
	CHECK(std::string(Describe(NormalId::Right)) == "Right");
	CHECK(std::string(Describe(NormalId::Top)) == "Top");
	CHECK(std::string(Describe(NormalId::Back)) == "Back");
	CHECK(std::string(Describe(NormalId::Left)) == "Left");
	CHECK(std::string(Describe(NormalId::Bottom)) == "Bottom");
	CHECK(std::string(Describe(NormalId::Front)) == "Front");
}

TEST_CASE("the face ordinals are the format and are pinned", "[scene][enums]") {
	// **This is a format assertion rather than a tidiness one.**
	// `SurfaceCamera::Face` stores the ordinal in a trivially-copied component,
	// so the number reaches a snapshot and a game file — and they are Roblox's
	// numbers so that a `Face` of 1 means `Top` in a file this engine wrote and
	// in one it did not. Reordering the enum is a format change, and this is
	// what says so out loud.
	CHECK(static_cast<uint8_t>(NormalId::Right) == 0);
	CHECK(static_cast<uint8_t>(NormalId::Top) == 1);
	CHECK(static_cast<uint8_t>(NormalId::Back) == 2);
	CHECK(static_cast<uint8_t>(NormalId::Left) == 3);
	CHECK(static_cast<uint8_t>(NormalId::Bottom) == 4);
	CHECK(static_cast<uint8_t>(NormalId::Front) == 5);
}

TEST_CASE("opposite faces have opposite normals", "[scene][enums]") {
	// Cheap, and it catches the one mistake that matters: a sign. A face whose
	// normal points the wrong way puts a mirror's reflection behind the pane,
	// which renders the clear colour and reads as a broken camera rather than as
	// a wrong axis.
	CHECK(NormalOf(NormalId::Right) == -NormalOf(NormalId::Left));
	CHECK(NormalOf(NormalId::Top) == -NormalOf(NormalId::Bottom));
	CHECK(NormalOf(NormalId::Back) == -NormalOf(NormalId::Front));

	// Every one is a unit vector, so the reflection arithmetic can divide by a
	// length it already knows is one.
	for (uint8_t index = 0; index < 6; index++) {
		INFO("ordinal " << static_cast<int>(index));
		CHECK(NormalOf(static_cast<NormalId>(index)).MagnitudeSquared() == 1.0f);
	}
}
