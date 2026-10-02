#include "../src/ImageGraphExportTriggers.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.export_triggers")
TEST_DEPENDS("studio.imagegraph")

using namespace engine::imagegraph;

TEST_CASE(
	"Source export callbacks resolve wired flags and exclude sequence or animation modes",
	"[studio][export_triggers]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"surface",
		 "image.solid",
		 {},
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{}}}},
		{"flag", "value.boolean", {}, {}, {{"value", true}}},
		{"export",
		 "pc.export",
		 {},
		 {},
		 {{"export_on_save", true}, {"export_on_update", false}, {"type", EnumValue{0}}}}
	};
	document.Links = {
		{"surface", "image", "export", "surface"}, {"flag", "boolean", "export", "export_on_update"}
	};
	document.Outputs = {{"result", "surface", "image"}};
	Plan plan;
	Diagnostic error;
	const auto compileStatus1 = Compile(document, plan, error);
	INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
	REQUIRE(compileStatus1 == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "export", {}, snapshot, error) == Status::Ok);
	CHECK(
		studio::detail::SourceExportTriggered(snapshot.Values(), studio::detail::ImageGraphExportEvent::Save)
	);
	CHECK(
		studio::detail::SourceExportTriggered(
			snapshot.Values(), studio::detail::ImageGraphExportEvent::Update
		)
	);
	for (int type : {1, 2}) {
		document.Nodes[2].Values.back().Data = EnumValue{type};
		const auto compileStatus2 = Compile(document, plan, error);
		INFO(error.Message << " node=" << error.NodeId << " port=" << error.Port);
		REQUIRE(compileStatus2 == Status::Ok);
		REQUIRE(EvaluateNodeInputs(document, plan, "export", {}, snapshot, error) == Status::Ok);
		CHECK_FALSE(
			studio::detail::SourceExportTriggered(
				snapshot.Values(), studio::detail::ImageGraphExportEvent::Save
			)
		);
		CHECK_FALSE(
			studio::detail::SourceExportTriggered(
				snapshot.Values(), studio::detail::ImageGraphExportEvent::Update
			)
		);
	}
}

TEST_CASE(
	"Export updates run once for a document and captured input revision at the exact frame",
	"[studio][export_triggers]"
) {
	studio::detail::ImageGraphExportUpdate update;
	CHECK(update.Accept(1, 1, FrameTime{3, .25}));
	CHECK_FALSE(update.Accept(1, 1, FrameTime{3, .25}));
	CHECK(update.Accept(1, 1, FrameTime{3, .5}));
	CHECK(update.Accept(2, 1, FrameTime{3, .5}));
	CHECK(update.Accept(2, 2, FrameTime{3, .5}));
	CHECK(update.Accept(2, 2, FrameTime{3, .5, true}));
}
