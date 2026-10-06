#include "../src/SourceVec2Defaults.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/GroupReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_vec2_defaults")
using namespace engine::imagegraph;
namespace {
	constexpr std::string_view Point = "point_i_0";

	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		Node base{
			"base",
			"pc.gradient_points_n",
			"",
			{},
			{{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}, {"blend_mode", EnumValue{0}}}
		};
		base.DynamicInputs = {
			{std::string(Point), ValueType::Vector2, Value{Vector2{99, 88}}},
			{"point_i_1", ValueType::Vector2, Value{Vector2{30, 3}}}
		};
		base.SourceVec2Defaults.emplace().Inputs.push_back({std::string(Point), Vector2{7, 8}});
		Node copy = base;
		copy.Id = "copy";
		copy.InstanceBase = "base";
		copy.InstanceOverrides = {std::string(Point)};
		document.Nodes = {std::move(base), std::move(copy)};
		return document;
	}
}

TEST_CASE(
	"Saved constructor Vec2 defaults roundtrip apart from mutable socket defaults", "[source_vec2_defaults]"
) {
	auto document = Graph();
	const auto &base = document.Nodes.front();
	CHECK(base.DynamicInputs.front().Default == Value{Vector2{99, 88}});
	CHECK(detail::SourceVec2ConstructorDefault(base, Point) == Vector2{7, 8});
	auto unresolved = base;
	unresolved.SourceVec2Defaults = {};
	CHECK(detail::SourceVec2ConstructorDefault(unresolved, Point) == Vector2{0, 0});

	const auto encoded = Write(document);
	REQUIRE_FALSE(encoded.empty());
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(encoded, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK(restored.Nodes.front().DynamicInputs.front().Default == Value{Vector2{99, 88}});
	CHECK(detail::SourceVec2ConstructorDefault(restored.Nodes.front(), Point) == Vector2{7, 8});
}

TEST_CASE("Constructor Vec2 default validation and byte accounting are bounded", "[source_vec2_defaults]") {
	auto node = Graph().Nodes.front();
	Diagnostic diagnostic;
	REQUIRE(detail::ValidateSourceVec2Defaults(node, diagnostic) == Status::Ok);
	auto retained = detail::SourceVec2DefaultsBytes(node, true);
	auto clone = detail::SourceVec2DefaultsBytes(node, false);
	REQUIRE(retained);
	REQUIRE(clone);
	node.SourceVec2Defaults->Inputs.reserve(4);
	retained = detail::SourceVec2DefaultsBytes(node, true);
	clone = detail::SourceVec2DefaultsBytes(node, false);
	REQUIRE(retained);
	REQUIRE(clone);
	CHECK(*retained >= *clone + 3 * sizeof(SourceVec2Default));

	auto duplicate = Graph().Nodes.front();
	duplicate.SourceVec2Defaults->Inputs.push_back({std::string(Point), Vector2{1, 2}});
	CHECK(detail::ValidateSourceVec2Defaults(duplicate, diagnostic) == Status::DuplicateId);
	CHECK_FALSE(detail::SourceVec2DefaultsBytes(duplicate, false));

	auto unsupported = Graph().Nodes.front();
	unsupported.SourceVec2Defaults->Inputs.front().Port = "missing";
	CHECK(detail::ValidateSourceVec2Defaults(unsupported, diagnostic) == Status::UnknownPort);
	CHECK_FALSE(detail::SourceVec2DefaultsBytes(unsupported, true));

	auto nonfinite = Graph().Nodes.front();
	nonfinite.SourceVec2Defaults->Inputs.front().Data.X = std::numeric_limits<double>::infinity();
	CHECK(detail::ValidateSourceVec2Defaults(nonfinite, diagnostic) == Status::InvalidValue);
	CHECK_FALSE(detail::SourceVec2DefaultsBytes(nonfinite, false));
}

TEST_CASE("Saved constructor pairs do not warm an aliased scalar axis array", "[source_vec2_defaults]") {
	auto document = Graph();
	GroupSubtypeBinding binding{
		"copy", "base", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, std::string(Point)
	};
	Diagnostic diagnostic;
	GroupReplayState empty, initial, bound;
	REQUIRE(RebindGroupReplay(document, empty, 1, initial, diagnostic) == Status::Ok);
	const std::array bindings{binding};
	REQUIRE(BindGroupReplay(document, bindings, initial, 1, bound, diagnostic) == Status::Ok);
	const auto *captured = bound.Binding("copy", Point);
	REQUIRE(captured);
	CHECK(captured->Axes.Storage == GroupAxisStorage::Uninitialized);
	CHECK(captured->Axes.OwnerId == "copy");
}
