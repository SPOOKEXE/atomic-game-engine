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
