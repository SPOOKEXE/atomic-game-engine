#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.color_depth")
TEST_DEPENDS("engine.imagegraph.document")
using namespace engine::imagegraph;
namespace {
	Document DepthDocument(uint32_t version = 9) {
		Document document;
		document.FormatVersion = version;
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = 8;
		document.Project->SurfaceHeight = 3;
		document.Nodes = {{"solid", "pc.solid", "inner", {}, {{"attribute_color_depth", EnumValue{1}}}}};
		document.Groups = {{"outer", "Outer"}, {"inner", "Inner", "outer"}};
		document.Outputs = {{"out", "solid", "surface_out"}};
		return document;
	}
}

TEST_CASE("Color depth persists durable names and keeps legacy defaults", "[imagegraph][color_depth]") {
	Diagnostic diagnostic;
	for (int64_t depth = 0; depth <= 6; ++depth) {
		auto document = DepthDocument();
		document.Project->ColorDepth = depth;
		document.Groups.front().ColorDepth = depth + 2;
		const auto text = Write(document);
		Document parsed;
		REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
		CHECK(parsed == document);
		Plan plan;
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	}
	for (int64_t groupDepth : {0, 1}) {
		auto document = DepthDocument();
		document.Groups.front().ColorDepth = groupDepth;
		Document parsed;
		REQUIRE(Read(Write(document), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == document);
	}
	auto legacy = DepthDocument(8);
	const auto text = Write(legacy);
	CHECK(text.find("project_depth") == std::string::npos);
	CHECK(text.find("group_depth") == std::string::npos);
	REQUIRE(Migrate(legacy, diagnostic) == Status::Ok);
	CHECK(legacy.Project->ColorDepth == 1);
	CHECK(legacy.Groups.front().ColorDepth == 1);
}

TEST_CASE("Invalid depth and lossy downgrade fail before plan replacement", "[imagegraph][color_depth]") {
	const auto original = DepthDocument();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(original, plan, diagnostic) == Status::Ok);
	const auto previousPlan = plan;
	for (bool group : {false, true}) {
		for (int64_t depth : {-1, 9}) {
			auto invalid = original;
			if (group)
				invalid.Groups.front().ColorDepth = depth;
			else
				invalid.Project->ColorDepth = depth;
			CHECK(
				Compile(invalid, plan, diagnostic) == (group ? Status::InvalidGroup : Status::InvalidValue)
			);
			CHECK(plan == previousPlan);
		}
		auto downgrade = original;
		downgrade.FormatVersion = 8;
		if (group)
			downgrade.Groups.front().ColorDepth = 5;
		else
			downgrade.Project->ColorDepth = 3;
		CHECK(Compile(downgrade, plan, diagnostic) == Status::UnsupportedVersion);
		CHECK(plan == previousPlan);
	}
}

TEST_CASE("Malformed named depth records preserve the previous document", "[imagegraph][color_depth]") {
	const auto original = DepthDocument();
	Diagnostic diagnostic;
	for (std::string_view records :
		 {"project_depth input\n",
		  "project_depth mystery\n",
		  "project_depth rgba32f\nproject_depth rgba16f\n",
		  "group_depth \"missing\" rgba8\n",
		  "group_depth \"inner\" mystery\n",
		  "group_depth \"inner\" rgba32f\ngroup_depth \"inner\" rgba16f\n"}) {
		auto document = original;
		CHECK(Read(Write(original) + std::string(records), document, diagnostic) == Status::Malformed);
		CHECK(document == original);
	}
	auto document = original;
	CHECK(Read("imagegraph 9\nproject_depth rgba32f\n", document, diagnostic) == Status::Malformed);
	CHECK(document == original);
}

TEST_CASE(
	"Solid depth inherits through native group parents and refuses unrepresented input depth",
	"[imagegraph][color_depth]"
) {
	auto document = DepthDocument();
	Diagnostic diagnostic;
	Image image;
	const std::array<SurfaceFormat, 7> formats{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (int64_t depth = 0; depth <= 6; ++depth) {
		document.Project->ColorDepth = depth;
		Plan plan;
		REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
		REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
		CHECK(image.Format == formats[depth]);
		CHECK(image.Width == 8);
		CHECK(image.Height == 3);
	}
	document.Groups.front().ColorDepth = 4;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Format == SurfaceFormat::RGBA16Float);
	document.Groups.back().ColorDepth = 5;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", image, diagnostic) == Status::Ok);
	CHECK(image.Format == SurfaceFormat::RGBA32Float);
	const auto previous = image;
	document.Groups.back().ColorDepth = 0;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", image, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "solid");
	CHECK(image == previous);
}
