#include "ImageGraphCacheEditObservation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.cache_edit_observation")

namespace {
	using namespace engine::imagegraph;
	using namespace studio::detail;
	Document Scene(bool array = false, bool ownerMember = false) {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = 2;
		document.Project->SurfaceHeight = 1;
		document.Nodes = {
			{"input",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}},
			{"cache",
			 array ? "pc.cache_array" : "pc.cache",
			 "",
			 {},
			 array
				 ? std::vector<
					   AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				 : std::vector<AuthoredValue>{{"animated", false}}}
		};
		ArrayValue members{ValueType::Text, {std::string{"input"}}};
		if (ownerMember) members.Elements.push_back(std::string{"cache"});
		document.Nodes[1].SourceProperties = {{"cache_group", members}};
		document.Links = {{"input", "image", "cache", "surface_in"}};
		document.Outputs = {
			{"out", "cache", array ? "cache_array" : "cache_surface"}, {"input-out", "input", "image"}
		};
		return document;
	}
	Plan Compiled(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	EvaluationRequest Request(bool endpoint = false) {
		EvaluationRequest request;
		request.SourceCachePlayback =
			SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
		request.SourceCacheProject =
			SourceFrameCacheProjectObservation{{0, 0, false}, endpoint ? 0.0 : 5.0, false, false};
		return request;
	}
	struct Fixture {
		Document Authored;
		ImageGraphCacheEditObservation Observation;
		CapturedFeedbackHost Host;
		Diagnostic Error;
		Fixture(bool array = false, bool ownerMember = false, bool routed = false, bool globals = false)
			: Authored(Scene(array, ownerMember)) {
			if (routed) {
				Authored.Nodes.push_back({"number", "pc.number", "", {}, {{"value", 2.0}}});
				auto &members = std::get<ArrayValue>(Authored.Nodes[1].SourceProperties[0].Data);
				members.Elements.push_back(std::string{"number"});
				Authored.Junctions = {
					{"junction", "group", ValueType::Boolean, false},
					{"replacement", "group", ValueType::Boolean, false}
				};
				Authored.Groups = {
					{"group",
					 "group",
					 "",
					 {{"integer", "junction", PortDirection::Output},
					  {"alternate", "replacement", PortDirection::Output}}}
				};
				Authored.Links.push_back({"junction", "value", "number", "integer"});
			}
			if (globals) {
				Node global{"globals", "pc.global_scope", "", {}, {}};
				global.DynamicInputs = {{"answer", ValueType::Scalar, Value{4.0}}};
				Authored.Nodes.push_back(std::move(global));
				Authored.ProjectGlobalNodeId = "globals";
				std::get<ArrayValue>(Authored.Nodes[1].SourceProperties[0].Data)
					.Elements.push_back(std::string{"globals"});
			}
			REQUIRE(ObserveImageGraphCacheEdits(
				Authored, Observation, Host, ImageGraphCacheEditKind::FreshDocument, Error
			));
			auto request = Request(true);
			REQUIRE(Host.Prepare(
				Authored, Compiled(Authored), 1, 1, request, Error, Limits::MaximumEvaluationBytes, "out"
			));
			REQUIRE(Host.PreparedData(1, 1));
			CHECK_FALSE(Member("input").RenderActive);
		}
		const CacheGroupReplayNode &Member(std::string_view id) const {
			const auto *data = Host.PreparedData(1, 1);
			REQUIRE(data);
			const auto found = std::find_if(
				data->CacheGroups.Nodes.begin(), data->CacheGroups.Nodes.end(), [&](const auto &node) {
					return node.NodeId == id;
				}
			);
			REQUIRE(found != data->CacheGroups.Nodes.end());
			return *found;
		}
		bool Observe(
			ImageGraphCacheEditKind kind = ImageGraphCacheEditKind::ValueSetter,
			uint64_t maximum = Limits::MaximumEvaluationBytes
		) {
			return ObserveImageGraphCacheEdits(Authored, Observation, Host, kind, Error, maximum);
		}
	};
}

TEST_CASE(
	"Studio cache input observation ignores presentation and source attributes",
	"[studio][imagegraph][cache_group]"
) {
	Fixture fixture;
	const auto original = *fixture.Host.PreparedData(1, 1);
	fixture.Authored.Nodes[0].Position = {30, 50};
	fixture.Authored.Nodes[0].SourceDisplayName = "new label";
	fixture.Authored.Nodes[1].SourceProperties.push_back({"serialize", false});
	fixture.Authored.Nodes[0].SourceInputExpressions.push_back({"width", "2", true});
	fixture.Authored.Nodes[0].InstanceBase = "different-base";
	std::reverse(fixture.Authored.Nodes[0].Values.begin(), fixture.Authored.Nodes[0].Values.end());
	std::reverse(fixture.Authored.Nodes.begin(), fixture.Authored.Nodes.end());
	REQUIRE(fixture.Observe());
	CHECK(*fixture.Host.PreparedData(1, 1) == original);
	CHECK(fixture.Observation.Inputs.Nodes[0].SourceProperties.empty());
	CHECK(fixture.Observation.Inputs.Nodes[0].SourceDisplayName.empty());
	CHECK_FALSE(fixture.Member("input").RenderActive);
}

TEST_CASE(
	"Studio value edits wake source cache members before same-clock revision replay",
	"[studio][imagegraph][cache_group]"
) {
	for (const bool array : {false, true}) {
		Fixture fixture(array);
		fixture.Authored.Nodes[0].Values[2].Data = Colour{70, 20, 30, 255};
		REQUIRE(fixture.Observe());
		CHECK(fixture.Member("input").RenderActive);
		const auto *cleared = fixture.Host.PreparedData(1, 1);
		REQUIRE(cleared);
		REQUIRE(cleared->Entries.size() == 1);
		CHECK(cleared->Entries[0].FrameCacheConstructorCleared);
		CHECK(cleared->Entries[0].Values.size() == 2);
		// Both setters reach cacheCheck even when edits return to the original value before preview.
		fixture.Authored.Nodes[0].Values[2].Data = Colour{10, 20, 30, 255};
		REQUIRE(fixture.Observe());
		CHECK(fixture.Member("input").RenderActive);
		fixture.Authored.Nodes[0].Values[2].Data = Colour{90, 20, 30, 255};
		REQUIRE(fixture.Observe());
		auto request = Request();
		request.SourceCachePlayback->Playing = false;
		REQUIRE(fixture.Host.Prepare(
			fixture.Authored,
			Compiled(fixture.Authored),
			2,
			1,
			request,
			fixture.Error,
			Limits::MaximumEvaluationBytes,
			"input-out"
		));
		REQUIRE(fixture.Host.Output("input-out"));
		CHECK(fixture.Host.Output("input-out")->Pixels[0] == 90);
	}
}

TEST_CASE(
	"Studio timeline changes render without waking but animator undo wakes",
	"[studio][imagegraph][cache_group]"
) {
	for (const bool undo : {false, true}) {
		Fixture fixture;
		fixture.Authored.Keyframes = {{"input", "width", 0, int64_t{2}, "step"}};
		REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
		fixture.Authored.Keyframes[0].Data = int64_t{3};
		REQUIRE(fixture.Observe(
			undo ? ImageGraphCacheEditKind::AnimatorUndo : ImageGraphCacheEditKind::RenderOnly
		));
		CHECK(fixture.Member("input").RenderActive == undo);
	}
	Fixture mode;
	const auto original = *mode.Host.PreparedData(1, 1);
	mode.Authored.Nodes[0].SourceAnimatedInputs = {"width"};
	mode.Authored.Keyframes = {{"input", "width", 0, int64_t{2}, "step"}};
	REQUIRE(mode.Observe(ImageGraphCacheEditKind::RenderOnly));
	mode.Authored.Nodes[0].SourceAnimatedInputs.clear();
	mode.Authored.Keyframes.clear();
	REQUIRE(mode.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(*mode.Host.PreparedData(1, 1) == original);
}

TEST_CASE(
	"Studio connection replacement and disconnection wake destinations", "[studio][imagegraph][cache_group]"
) {
	for (const bool disconnect : {false, true}) {
		Fixture fixture(false, true);
		if (disconnect)
			fixture.Authored.Links.clear();
		else {
			auto alternate = fixture.Authored.Nodes[0];
			alternate.Id = "alternate";
			fixture.Authored.Nodes.push_back(std::move(alternate));
			fixture.Authored.Links[0].FromNode = "alternate";
		}
		REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
		CHECK(fixture.Member("cache").RenderActive);
		CHECK(fixture.Member("input").RenderActive);
	}
}

TEST_CASE(
	"Studio junction defaults and changed group routes reach frozen consumers",
	"[studio][imagegraph][cache_group]"
) {
	for (const bool changeGroup : {false, true}) {
		Fixture fixture(false, false, true);
		if (changeGroup)
			std::swap(
				fixture.Authored.Groups[0].Ports[0].JunctionId, fixture.Authored.Groups[0].Ports[1].JunctionId
			);
		else
			fixture.Authored.Junctions[0].Default = true;
		Compiled(fixture.Authored);
		REQUIRE(fixture.Observe());
		CHECK(fixture.Member("input").RenderActive);
	}
}

TEST_CASE(
	"Studio refused value observation cannot be consumed by a render-only edit",
	"[studio][imagegraph][cache_group]"
) {
	Fixture fixture;
	const auto original = *fixture.Host.PreparedData(1, 1);
	const auto observed = fixture.Observation.Inputs;
	fixture.Authored.Nodes[0].Values[2].Data = Colour{70, 20, 30, 255};
	CHECK_FALSE(fixture.Observe(ImageGraphCacheEditKind::ValueSetter, 1));
	CHECK(fixture.Observation.Inputs == observed);
	CHECK(*fixture.Host.PreparedData(1, 1) == original);
	CHECK(fixture.Observation.PendingValueEdit);
	fixture.Authored.Nodes[0].Position = {12, 13};
	CHECK_FALSE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK(fixture.Error.Code == Status::UnsupportedExecution);
	CHECK(fixture.Observation.Inputs == observed);
	CHECK(*fixture.Host.PreparedData(1, 1) == original);
	REQUIRE(fixture.Observe());
	CHECK_FALSE(fixture.Observation.PendingValueEdit);
	CHECK(fixture.Member("input").RenderActive);
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::FreshDocument));
	CHECK_FALSE(fixture.Host.PreparedData(1, 1));
}

TEST_CASE(
	"Studio pending setter retains its kind through animator undo", "[studio][imagegraph][cache_group]"
) {
	Fixture fixture;
	fixture.Authored.Nodes[0].Values[2].Data = Colour{70, 20, 30, 255};
	CHECK_FALSE(fixture.Observe(ImageGraphCacheEditKind::ValueSetter, 1));
	fixture.Authored.Nodes[0].SourceAnimatedInputs = {"width"};
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(fixture.Member("input").RenderActive);
	CHECK_FALSE(fixture.Observation.PendingValueEdit);
}

TEST_CASE(
	"Studio global value setters and animator undo preserve frozen group activity",
	"[studio][imagegraph][cache_group]"
) {
	for (const auto kind : {ImageGraphCacheEditKind::ValueSetter, ImageGraphCacheEditKind::AnimatorUndo}) {
		Fixture fixture(false, false, false, true);
		const auto original = *fixture.Host.PreparedData(1, 1);
		fixture.Authored.Nodes.back().DynamicInputs[0].Default = Value{8.0};
		fixture.Authored.Keyframes = {{"globals", "answer", 0, 8.0, "step"}};
		Compiled(fixture.Authored);
		REQUIRE(fixture.Observe(kind));
		CHECK(*fixture.Host.PreparedData(1, 1) == original);
		CHECK_FALSE(fixture.Member("input").RenderActive);
		CHECK_FALSE(fixture.Member("globals").RenderActive);
	}
}
