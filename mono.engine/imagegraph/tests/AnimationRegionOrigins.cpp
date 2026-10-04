#include "SourceRegionOrigin.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.animation_region_origins")
namespace {
	using namespace engine::imagegraph;
	Document Regions() {
		Document document;
		document.FormatVersion = 9;
		document.Project.emplace();
		document.Project->AnimationRegions = {
			{"", {7, 13, 19, 255}, {2, .5, true}, {3, .25, false}, "pxc:region:0"},
			{"", {7, 13, 19, 255}, {2, .5, true}, {3, .25, false}, "pxc:region:2"},
			{"new", {0, 0, 0, 255}, {}, {}}
		};
		return document;
	}
}
TEST_CASE(
	"Optional region origins preserve duplicate labels and old native records", "[imagegraph][region_origin]"
) {
	const auto document = Regions();
	const auto text = Write(document);
	REQUIRE_FALSE(text.empty());
	CHECK(text.find("source_region_origin 0 \"pxc:region:0\"") != std::string::npos);
	CHECK(text.find("source_region_origin 1 \"pxc:region:2\"") != std::string::npos);
	CHECK(text.find("source_region_origin 2 ") == std::string::npos);
	Diagnostic diagnostic;
	Document parsed;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	auto legacy = document;
	for (auto &region : legacy.Project->AnimationRegions)
		region.SourceRegionId.clear();
	const auto oldText = Write(legacy);
	CHECK(oldText.find("source_region_origin") == std::string::npos);
	REQUIRE(Read(oldText, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == legacy);
	const auto before = DocumentRetainedPayloadBytes(document);
	REQUIRE(before);
	auto reserved = document;
	reserved.Project->AnimationRegions[0].SourceRegionId.reserve(4096);
	const auto after = DocumentRetainedPayloadBytes(reserved);
	REQUIRE(after);
	CHECK(*after >= *before + 4096 - document.Project->AnimationRegions[0].SourceRegionId.capacity());
}
TEST_CASE(
	"Region origins reject duplicates malformed and repeated native stanzas atomically",
	"[imagegraph][region_origin]"
) {
	const auto document = Regions();
	const auto text = Write(document);
	Diagnostic diagnostic;
	Document parsed = document;
	for (const auto suffix :
		 {"source_region_origin 0 \"pxc:region:7\"\n",
		  "source_region_origin 2 \"pxc:region:2\"\n",
		  "source_region_origin 3 \"pxc:region:3\"\n",
		  "source_region_origin 2 \"pxc:region:03\"\n",
		  "source_region_origin 2 \"pxc:region:18446744073709551616\"\n",
		  "source_region_origin 2 \"pxc:region:3\" trailing\n"}) {
		CHECK(Read(text + suffix, parsed, diagnostic) != Status::Ok);
		CHECK(parsed == document);
	}
	for (const auto &origin :
		 {std::string("pxc:region:2"), std::string("pxc:region:00"), std::string(65, 'x')}) {
		auto invalid = document;
		invalid.Project->AnimationRegions[0].SourceRegionId = origin;
		CHECK_FALSE(ValidProjectAnimationRegions(*invalid.Project));
		CHECK(Write(invalid).empty());
	}
}
TEST_CASE(
	"The maximum region set validates unique archive addresses without quadratic scans",
	"[imagegraph][region_origin]"
) {
	ProjectSettings project;
	project.AnimationRegions.reserve(Limits::MaximumAnimationRegions);
	for (size_t index = 0; index < Limits::MaximumAnimationRegions; ++index) {
		AnimationRegion region;
		region.SourceRegionId = "pxc:region:" + std::to_string(index);
		project.AnimationRegions.push_back(std::move(region));
	}
	CHECK(ValidProjectAnimationRegions(project));
	project.AnimationRegions.back().SourceRegionId = project.AnimationRegions.front().SourceRegionId;
	CHECK_FALSE(ValidProjectAnimationRegions(project));
	CHECK(detail::SourceRegionOrdinal("pxc:region:18446744073709551615") == UINT64_MAX);
}
