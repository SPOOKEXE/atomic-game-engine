#include "ImageGraphCacheEditObservation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/ImageGraph.hpp>

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
	"Studio cache input observation ignores presentation and noncache source attributes",
	"[studio][imagegraph][cache_group]"
) {
	Fixture fixture;
	const auto original = *fixture.Host.PreparedData(1, 1);
	fixture.Authored.Nodes[0].Position = {30, 50};
	fixture.Authored.Nodes[0].SourceDisplayName = "new label";
	fixture.Authored.Nodes[1].SourceProperties.push_back({"opaque", false});
	fixture.Authored.Nodes[0].SourceInputExpressions.push_back({"width", "2", true});
	fixture.Authored.Nodes[0].InstanceBase = "different-base";
	std::reverse(fixture.Authored.Nodes[0].Values.begin(), fixture.Authored.Nodes[0].Values.end());
	std::reverse(fixture.Authored.Nodes.begin(), fixture.Authored.Nodes.end());
	REQUIRE(fixture.Observe());
	CHECK(*fixture.Host.PreparedData(1, 1) == original);
	CHECK(fixture.Observation.Inputs.Nodes[0].SourceProperties.size() == 1);
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

TEST_CASE(
	"Studio fresh load stages membership before replacing the live host",
	"[studio][imagegraph][cache_group][load]"
) {
	Fixture fixture;
	const auto original = *fixture.Host.PreparedData(1, 1);
	const auto observed = fixture.Observation.Inputs;
	fixture.Authored.Nodes[1].SourceProperties[0].Data = int64_t{4};
	CHECK_FALSE(fixture.Observe(ImageGraphCacheEditKind::FreshDocument));
	CHECK(fixture.Error.Code == Status::InvalidValue);
	CHECK(*fixture.Host.PreparedData(1, 1) == original);
	CHECK(fixture.Observation.Inputs == observed);
	fixture.Authored.Nodes[1].SourceProperties[0].Data = ArrayValue{ValueType::Text, {std::string{"input"}}};
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::FreshDocument));
	CHECK_FALSE(fixture.Host.PreparedData(1, 1));
	auto request = Request();
	REQUIRE(fixture.Host.Prepare(
		fixture.Authored,
		Compiled(fixture.Authored),
		2,
		1,
		request,
		fixture.Error,
		Limits::MaximumEvaluationBytes,
		"out"
	));
	const auto *data = fixture.Host.PreparedData(2, 1);
	REQUIRE(data);
	const auto input =
		std::find_if(data->CacheGroups.Nodes.begin(), data->CacheGroups.Nodes.end(), [](const auto &node) {
			return node.NodeId == "input";
		});
	REQUIRE(input != data->CacheGroups.Nodes.end());
	CHECK(input->RenderActive);
	CHECK(input->OwnerId == "cache");
}

TEST_CASE(
	"Studio membership removal preserves captured rows through render-only revision",
	"[studio][imagegraph][cache_group][membership]"
) {
	Fixture fixture;
	const auto prior = *fixture.Host.PreparedData(1, 1);
	const auto pixels = *fixture.Host.Output("out");
	REQUIRE(fixture.Host.ToggleSourceCacheGroupMember(fixture.Authored, "cache", "input", fixture.Error));
	CHECK(fixture.Member("input").RenderActive);
	CHECK(fixture.Member("input").OwnerId.empty());
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == prior.Entries);
	CHECK(*fixture.Host.Output("out") == pixels);
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == prior.Entries);
	auto request = Request();
	REQUIRE(fixture.Host.Prepare(
		fixture.Authored,
		Compiled(fixture.Authored),
		2,
		1,
		request,
		fixture.Error,
		Limits::MaximumEvaluationBytes,
		"out"
	));
	CHECK(*fixture.Host.Output("out") == pixels);
	CHECK(fixture.Host.PreparedData(2, 1)->Entries == prior.Entries);
	CHECK(std::get<ArrayValue>(fixture.Authored.Nodes[1].SourceProperties[0].Data).Elements.empty());
	CHECK(fixture.Host.SourceCacheGroups().Owners.front().Members.empty());
}

TEST_CASE(
	"Studio membership history admission preserves replay and existing redo on refusal",
	"[studio][imagegraph][cache_group][membership][history]"
) {
	Fixture fixture;
	const auto before = fixture.Authored;
	const auto replay = *fixture.Host.PreparedData(1, 1);
	const auto pixels = *fixture.Host.Output("out");
	Document small;
	small.Nodes = {{"number", "pc.number", "", {}, {{"value", 1.0}}}};
	auto changed = small;
	changed.Nodes.front().Position.X = 30;
	studio::ImageGraphHistory history(128, Write(changed).size() + 1);
	REQUIRE(history.TryRecord(small, changed));
	REQUIRE(history.Undo(changed));
	REQUIRE(history.CanRedo());
	CHECK_FALSE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, "cache", "input", fixture.Error
	));
	CHECK(fixture.Error.Code == Status::LimitExceeded);
	CHECK(fixture.Authored == before);
	CHECK(*fixture.Host.PreparedData(1, 1) == replay);
	CHECK(*fixture.Host.Output("out") == pixels);
	CHECK_FALSE(history.CanUndo());
	CHECK(history.CanRedo());
	REQUIRE(history.Redo(changed));
	CHECK(changed.Nodes.front().Position.X == 30);
	// grug same-clock replay uses the preframe journal too, after rejected admission.
	auto request = Request(true);
	REQUIRE(fixture.Host.Prepare(
		fixture.Authored,
		Compiled(fixture.Authored),
		1,
		1,
		request,
		fixture.Error,
		Limits::MaximumEvaluationBytes,
		"out"
	));
	CHECK(*fixture.Host.PreparedData(1, 1) == replay);
}

TEST_CASE(
	"Studio membership records authored history before publishing replay without clearing frames",
	"[studio][imagegraph][cache_group][membership][history]"
) {
	Fixture fixture;
	studio::ImageGraphHistory history;
	const auto before = fixture.Authored;
	const auto replay = *fixture.Host.PreparedData(1, 1);
	const auto pixels = *fixture.Host.Output("out");
	REQUIRE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, "cache", "input", fixture.Error
	));
	const auto edited = fixture.Authored;
	CHECK(fixture.Member("input").OwnerId.empty());
	CHECK(fixture.Member("input").RenderActive);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == replay.Entries);
	CHECK(*fixture.Host.Output("out") == pixels);
	CHECK(history.CanUndo());
	// grug check history text here. runtime undo notification belongs to the caller.
	auto restored = edited;
	REQUIRE(history.Undo(restored));
	CHECK(restored == before);
	REQUIRE(history.Redo(restored));
	CHECK(restored == edited);
}

TEST_CASE(
	"Studio membership runtime refusal leaves history unused",
	"[studio][imagegraph][cache_group][membership][history]"
) {
	Fixture fixture;
	studio::ImageGraphHistory history;
	const auto before = fixture.Authored;
	const auto replay = *fixture.Host.PreparedData(1, 1);
	CHECK_FALSE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, "cache", "input", fixture.Error, 1
	));
	CHECK(fixture.Authored == before);
	CHECK(*fixture.Host.PreparedData(1, 1) == replay);
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(history.CanRedo());
	CHECK_FALSE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, "cache", "absent", fixture.Error
	));
	CHECK(fixture.Error.Code == Status::UnknownNode);
	CHECK_FALSE(history.CanUndo());
	CHECK(fixture.Authored == before);
	CHECK(*fixture.Host.PreparedData(1, 1) == replay);
}

TEST_CASE(
	"Studio Serialize restoration preserves overlapping pointers and frozen frames",
	"[studio][imagegraph][cache_group][metadata][history]"
) {
	Fixture fixture;
	Node other{"other", "pc.cache", "", {}, {{"animated", false}}};
	other.SourceProperties = {{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}};
	fixture.Authored.Nodes.push_back(other);
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK(fixture.Member("input").OwnerId == "other");
	const auto rows = fixture.Host.PreparedData(1, 1)->Entries;
	const auto getters = fixture.Member("input").Outputs;
	fixture.Authored.Nodes[1].SourceProperties.push_back({"serialize", false});
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(fixture.Host.SourceCacheGroups().Owners.front().Serialize == false);
	CHECK(fixture.Member("input").OwnerId == "other");
	CHECK_FALSE(fixture.Member("input").RenderActive);
	CHECK(fixture.Member("input").Outputs == getters);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == rows);
	fixture.Authored.Nodes[1].SourceProperties.pop_back();
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
	CHECK(fixture.Member("input").OwnerId == "other");
	CHECK_FALSE(fixture.Member("input").RenderActive);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == rows);
}

TEST_CASE(
	"Studio membership history restoration detaches omitted pointers without clearing frames",
	"[studio][imagegraph][cache_group][metadata][history]"
) {
	Fixture fixture;
	const auto rows = fixture.Host.PreparedData(1, 1)->Entries;
	REQUIRE(fixture.Host.ToggleSourceCacheGroupMember(fixture.Authored, "cache", "input", fixture.Error));
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	studio::ImageGraphHistory history;
	REQUIRE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, "cache", "input", fixture.Error
	));
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK(fixture.Member("input").OwnerId == "cache");
	REQUIRE(history.Undo(fixture.Authored));
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(fixture.Member("input").OwnerId.empty());
	CHECK(fixture.Member("input").RenderActive);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == rows);
	REQUIRE(history.Redo(fixture.Authored));
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(fixture.Member("input").OwnerId == "cache");
	CHECK(fixture.Member("input").RenderActive);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == rows);
}

TEST_CASE(
	"Studio restored Serialize gates admitted input edits in the same transaction",
	"[studio][imagegraph][cache_group][metadata][history]"
) {
	Fixture fixture;
	const auto rows = fixture.Host.PreparedData(1, 1)->Entries;
	fixture.Authored.Nodes[1].SourceProperties.push_back({"serialize", false});
	fixture.Authored.Nodes[0].Values[2].Data = Colour{80, 20, 30, 255};
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK_FALSE(fixture.Member("input").RenderActive);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == rows);
	CHECK_FALSE(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
	fixture.Authored.Nodes[1].SourceProperties.pop_back();
	fixture.Authored.Nodes[0].Values[2].Data = Colour{90, 20, 30, 255};
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::AnimatorUndo));
	CHECK(fixture.Member("input").RenderActive);
	CHECK(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
	REQUIRE(fixture.Host.PreparedData(1, 1)->Entries.size() == 1);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries.front().Values.size() == 2);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries.front().FrameCacheConstructorCleared);
}

TEST_CASE(
	"Studio malformed metadata refusal preserves replay observation and retries",
	"[studio][imagegraph][cache_group][metadata][bounds]"
) {
	Fixture fixture;
	const auto replay = *fixture.Host.PreparedData(1, 1);
	const auto observation = fixture.Observation.Inputs;
	fixture.Authored.Nodes[1].SourceProperties.push_back({"serialize", std::string{"false"}});
	CHECK_FALSE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK(fixture.Error.Code == Status::InvalidValue);
	CHECK(*fixture.Host.PreparedData(1, 1) == replay);
	CHECK(fixture.Observation.Inputs == observation);
	fixture.Authored.Nodes[1].SourceProperties.back().Data = false;
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK_FALSE(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == replay.Entries);
	CHECK_FALSE(fixture.Member("input").RenderActive);
}

TEST_CASE(
	"Studio cold metadata and new membership edits synchronize before first preparation",
	"[studio][imagegraph][cache_group][metadata][load]"
) {
	auto document = Scene();
	ImageGraphCacheEditObservation observation;
	CapturedFeedbackHost host;
	Diagnostic error;
	REQUIRE(ObserveImageGraphCacheEdits(
		document, observation, host, ImageGraphCacheEditKind::FreshDocument, error
	));
	document.Nodes[1].SourceProperties.push_back({"serialize", false});
	REQUIRE(
		ObserveImageGraphCacheEdits(document, observation, host, ImageGraphCacheEditKind::AnimatorUndo, error)
	);
	CHECK_FALSE(host.SourceCacheGroups().Owners.front().Serialize);
	Node other{"other", "pc.cache", "", {}, {{"animated", false}}};
	other.SourceProperties = {{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}};
	document.Nodes.push_back(other);
	REQUIRE(
		ObserveImageGraphCacheEdits(document, observation, host, ImageGraphCacheEditKind::RenderOnly, error)
	);
	REQUIRE(host.SourceCacheGroups().Owners.size() == 2);
	const auto member = std::find_if(
		host.SourceCacheGroups().Nodes.begin(), host.SourceCacheGroups().Nodes.end(), [](const auto &node) {
			return node.NodeId == "input";
		}
	);
	REQUIRE(member != host.SourceCacheGroups().Nodes.end());
	CHECK(member->OwnerId == "other");
}

TEST_CASE(
	"Studio undo of an added then frozen member wakes it for later value edits",
	"[studio][imagegraph][cache_group][metadata][history]"
) {
	Fixture fixture;
	REQUIRE(fixture.Host.ToggleSourceCacheGroupMember(fixture.Authored, "cache", "input", fixture.Error));
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	studio::ImageGraphHistory history;
	REQUIRE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, "cache", "input", fixture.Error
	));
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	auto request = Request();
	request.Tick = 1;
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 1, false, false};
	REQUIRE(fixture.Host.Prepare(
		fixture.Authored,
		Compiled(fixture.Authored),
		2,
		1,
		request,
		fixture.Error,
		Limits::MaximumEvaluationBytes,
		"out"
	));
	const auto currentMember = [&]() -> const CacheGroupReplayNode & {
		const auto &groups = fixture.Host.SourceCacheGroups();
		const auto member = std::find_if(groups.Nodes.begin(), groups.Nodes.end(), [](const auto &node) {
			return node.NodeId == "input";
		});
		REQUIRE(member != groups.Nodes.end());
		return *member;
	};
	REQUIRE_FALSE(currentMember().RenderActive);
	const auto rows = fixture.Host.PreparedData(2, 1)->Entries;
	const auto getters = currentMember().Outputs;
	studio::ImageGraphPlayback playback;
	REQUIRE(ApplyImageGraphCacheHistory(
		fixture.Authored, history, fixture.Host, fixture.Observation, playback, false, fixture.Error
	));
	CHECK(currentMember().OwnerId.empty());
	CHECK(currentMember().RenderActive);
	CHECK(currentMember().Outputs == getters);
	CHECK(fixture.Host.PreparedData(2, 1)->Entries == rows);
	fixture.Authored.Nodes[0].Values[2].Data = Colour{90, 20, 30, 255};
	REQUIRE(fixture.Observe());
	request = Request();
	request.Tick = 1;
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 1, false, false};
	REQUIRE(fixture.Host.Prepare(
		fixture.Authored,
		Compiled(fixture.Authored),
		3,
		1,
		request,
		fixture.Error,
		Limits::MaximumEvaluationBytes,
		"input-out"
	));
	REQUIRE(fixture.Host.Output("input-out"));
	CHECK(fixture.Host.Output("input-out")->Pixels[0] == 90);
	CHECK(fixture.Host.PreparedData(3, 1)->Entries == rows);
}

TEST_CASE(
	"Studio refused cache history keeps authored state and history direction",
	"[studio][imagegraph][cache_group][history]"
) {
	for (const bool redo : {false, true}) {
		Fixture fixture;
		studio::ImageGraphHistory history;
		const auto initial = fixture.Authored;
		auto edited = initial;
		edited.Nodes[1].SourceProperties.push_back({"serialize", false});
		REQUIRE(history.TryRecord(initial, edited));
		if (redo)
			REQUIRE(history.Undo(edited));
		else
			fixture.Authored = edited;
		REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
		const auto document = fixture.Authored;
		const auto data = *fixture.Host.PreparedData(1, 1);
		const auto inputs = fixture.Observation.Inputs;
		const bool undoBefore = history.CanUndo(), redoBefore = history.CanRedo();
		studio::ImageGraphPlayback playback;
		playback.Playing = true;
		playback.Subframe = 0.25;
		playback.Accumulator = 0.1;
		playback.SelectedRegion = std::pair{FrameTime{1, 0, false}, FrameTime{3, 0, false}};
		fixture.Observation.PendingValueKind = ImageGraphCacheEditKind::RenderOnly;
		const auto bytes = DocumentRetainedPayloadBytes(document);
		REQUIRE(bytes);
		for (const uint64_t cap : {uint64_t{1}, *bytes + 1})
			CHECK_FALSE(ApplyImageGraphCacheHistory(
				fixture.Authored,
				history,
				fixture.Host,
				fixture.Observation,
				playback,
				redo,
				fixture.Error,
				cap
			));
		CHECK(fixture.Authored == document);
		CHECK(history.CanUndo() == undoBefore);
		CHECK(history.CanRedo() == redoBefore);
		CHECK(*fixture.Host.PreparedData(1, 1) == data);
		CHECK(fixture.Observation.Inputs == inputs);
		CHECK_FALSE(fixture.Observation.PendingValueEdit);
		CHECK(fixture.Observation.PendingValueKind == ImageGraphCacheEditKind::RenderOnly);
		CHECK(playback.Playing);
		CHECK(playback.Subframe == 0.25);
		CHECK(playback.Accumulator == 0.1);
		CHECK(playback.SelectedRegion.has_value());
		REQUIRE(ApplyImageGraphCacheHistory(
			fixture.Authored, history, fixture.Host, fixture.Observation, playback, redo, fixture.Error
		));
		CHECK(fixture.Authored.Nodes[1].SourceProperties.size() == (redo ? 2 : 1));
		CHECK(history.CanUndo() == redo);
		CHECK(history.CanRedo() == !redo);
		CHECK_FALSE(playback.Playing);
		CHECK(playback.Subframe == 0.25);
		CHECK_FALSE(playback.SelectedRegion.has_value());
		const auto &restored = *fixture.Host.PreparedData(1, 1);
		CHECK(restored.Entries == data.Entries);
		CHECK(restored.CacheGroups.Nodes == data.CacheGroups.Nodes);
		REQUIRE(restored.CacheGroups.Owners.size() == 1);
		CHECK(restored.CacheGroups.Owners[0].Serialize == !redo);
		CHECK(restored.CacheGroups.Owners[0].Members == data.CacheGroups.Owners[0].Members);
	}
}

TEST_CASE(
	"Studio admitted value history wakes current and preframe cache journals",
	"[studio][imagegraph][cache_group][history]"
) {
	for (const bool array : {false, true}) {
		Fixture fixture(array);
		const auto original = fixture.Authored;
		auto prior = original;
		prior.Nodes[0].Values[2].Data = Colour{70, 20, 30, 255};
		studio::ImageGraphHistory history;
		REQUIRE(history.TryRecord(prior, original));
		studio::ImageGraphPlayback playback;
		REQUIRE(ApplyImageGraphCacheHistory(
			fixture.Authored, history, fixture.Host, fixture.Observation, playback, false, fixture.Error
		));
		CHECK(fixture.Authored == prior);
		CHECK(fixture.Member("input").RenderActive);
		CHECK(fixture.Host.PreparedData(1, 1)->Entries[0].Values.size() == 2);
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
		CHECK(fixture.Host.Output("input-out")->Pixels[0] == 70);
		REQUIRE(ApplyImageGraphCacheHistory(
			fixture.Authored, history, fixture.Host, fixture.Observation, playback, true, fixture.Error
		));
		CHECK(fixture.Authored == original);
		request = Request();
		request.SourceCachePlayback->Playing = false;
		REQUIRE(fixture.Host.Prepare(
			fixture.Authored,
			Compiled(fixture.Authored),
			3,
			1,
			request,
			fixture.Error,
			Limits::MaximumEvaluationBytes,
			"input-out"
		));
		REQUIRE(fixture.Host.Output("input-out"));
		CHECK(fixture.Host.Output("input-out")->Pixels[0] == 10);
	}
}
TEST_CASE(
	"Studio history waits for an already pending authored value edit",
	"[studio][imagegraph][cache_group][history]"
) {
	Fixture fixture;
	auto prior = fixture.Authored;
	prior.Nodes[0].Values[2].Data = Colour{70, 20, 30, 255};
	studio::ImageGraphHistory history;
	REQUIRE(history.TryRecord(prior, fixture.Authored));
	fixture.Observation.PendingValueEdit = true;
	fixture.Observation.PendingValueKind = ImageGraphCacheEditKind::ValueSetter;
	studio::ImageGraphPlayback playback;
	const auto document = fixture.Authored;
	const auto data = *fixture.Host.PreparedData(1, 1);
	CHECK_FALSE(ApplyImageGraphCacheHistory(
		fixture.Authored, history, fixture.Host, fixture.Observation, playback, false, fixture.Error
	));
	CHECK(fixture.Error.Code == Status::UnsupportedExecution);
	CHECK(fixture.Authored == document);
	CHECK(*fixture.Host.PreparedData(1, 1) == data);
	CHECK(history.CanUndo());
	CHECK_FALSE(history.CanRedo());
	CHECK(fixture.Observation.PendingValueEdit);
	CHECK(fixture.Observation.PendingValueKind == ImageGraphCacheEditKind::ValueSetter);
}

TEST_CASE(
	"Studio cache controls admit Serialize without changing ownership or captured data",
	"[studio][imagegraph][cache_group][controls]"
) {
	for (const bool array : {false, true}) {
		Fixture fixture(array);
		Node other{"other", "pc.cache", "", {}, {{"animated", false}}};
		other.SourceProperties = {{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}};
		fixture.Authored.Nodes.push_back(other);
		REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
		const auto before = fixture.Authored;
		const auto data = *fixture.Host.PreparedData(1, 1);
		REQUIRE(fixture.Host.Value("out"));
		const auto output = *fixture.Host.Value("out");
		studio::ImageGraphHistory history;
		REQUIRE(ApplyImageGraphCacheSerialize(
			fixture.Authored, history, fixture.Host, fixture.Observation, "cache", false, fixture.Error
		));
		CHECK_FALSE(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
		CHECK(fixture.Host.PreparedData(1, 1)->Entries == data.Entries);
		CHECK(fixture.Host.PreparedData(1, 1)->CacheGroups.Nodes == data.CacheGroups.Nodes);
		REQUIRE(fixture.Host.Value("out"));
		const auto &current = fixture.Host.Value("out")->Output;
		CHECK(current.index() == output.Output.index());

		std::visit(
			[&](const auto &value) {
				using T = std::decay_t<decltype(value)>;
				const auto &prior = std::get<T>(output.Output);
				if constexpr (std::is_same_v<T, ImageArray>) {
					CHECK(value.Images == prior.Images);
					CHECK(value.Items == prior.Items);
				} else
					CHECK(value == prior);
			},
			current
		);
		CHECK(fixture.Member("input").OwnerId == "other");
		CHECK_FALSE(fixture.Member("input").RenderActive);
		const auto disabled = fixture.Authored;
		CHECK_FALSE(ApplyImageGraphCacheSerialize(
			fixture.Authored, history, fixture.Host, fixture.Observation, "cache", false, fixture.Error
		));
		CHECK(fixture.Authored == disabled);
		studio::ImageGraphPlayback playback;
		REQUIRE(ApplyImageGraphCacheHistory(
			fixture.Authored, history, fixture.Host, fixture.Observation, playback, false, fixture.Error
		));
		CHECK(fixture.Authored == before);
		CHECK(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
		CHECK(fixture.Host.PreparedData(1, 1)->CacheGroups.Nodes == data.CacheGroups.Nodes);
		REQUIRE(ApplyImageGraphCacheHistory(
			fixture.Authored, history, fixture.Host, fixture.Observation, playback, true, fixture.Error
		));
		CHECK(fixture.Authored == disabled);
		REQUIRE(ApplyImageGraphCacheSerialize(
			fixture.Authored, history, fixture.Host, fixture.Observation, "cache", true, fixture.Error
		));
		CHECK(fixture.Host.SourceCacheGroups().Owners.front().Serialize);
		CHECK(fixture.Host.PreparedData(1, 1)->Entries == data.Entries);
		CHECK(fixture.Host.PreparedData(1, 1)->CacheGroups.Nodes == data.CacheGroups.Nodes);
	}
}

TEST_CASE(
	"Studio observed membership click preserves selected refresh policy for loaded overlap",
	"[studio][imagegraph][cache_group][controls]"
) {
	Fixture fixture;
	Node other{"other", "pc.cache", "", {}, {{"animated", false}}};
	other.SourceProperties = {{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}};
	fixture.Authored.Nodes.push_back(other);
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	REQUIRE(fixture.Member("input").OwnerId == "other");
	const auto document = fixture.Authored;
	const auto data = *fixture.Host.PreparedData(1, 1);
	const auto getters = fixture.Member("input").Outputs;
	studio::ImageGraphHistory history;
	REQUIRE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, fixture.Observation, "cache", "input", fixture.Error
	));
	CHECK(fixture.Authored == document);
	CHECK_FALSE(history.CanUndo());
	CHECK(fixture.Member("input").OwnerId == "cache");
	CHECK_FALSE(fixture.Member("input").RenderActive);
	CHECK(fixture.Member("input").Outputs == getters);
	CHECK(fixture.Host.PreparedData(1, 1)->Entries == data.Entries);
	REQUIRE(fixture.Observe(ImageGraphCacheEditKind::RenderOnly));
	CHECK(fixture.Member("input").OwnerId == "cache");
	REQUIRE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, fixture.Observation, "cache", "input", fixture.Error
	));
	CHECK(fixture.Member("input").OwnerId.empty());
	CHECK(fixture.Member("input").RenderActive);
	CHECK(history.CanUndo());
	REQUIRE(ApplyImageGraphCacheGroupMember(
		fixture.Authored, history, fixture.Host, fixture.Observation, "cache", "input", fixture.Error
	));
	CHECK(fixture.Member("input").OwnerId == "cache");
	fixture.Authored.Nodes[0].Values[2].Data = Colour{90, 20, 30, 255};
	REQUIRE(fixture.Observe());
	CHECK(fixture.Host.PreparedData(1, 1)->Entries.front().FrameCacheConstructorCleared);
}

TEST_CASE(
	"Studio cache control refusals preserve document observation replay and redo",
	"[studio][imagegraph][cache_group][controls][bounds]"
) {
	for (const bool membership : {false, true}) {
		for (const int refusal : {0, 1, 2, 3, 4}) {
			Fixture fixture;
			studio::ImageGraphHistory history(refusal == 0 ? 0 : 128);
			if (refusal != 0) {
				auto prior = fixture.Authored;
				prior.Nodes[0].Position.X = 20;
				REQUIRE(history.TryRecord(fixture.Authored, prior));
				REQUIRE(history.Undo(prior));
				REQUIRE(history.CanRedo());
			}
			if (refusal == 2) fixture.Observation.PendingValueEdit = true;
			if (refusal == 3) fixture.Authored.Nodes[0].Values[2].Data = Colour{99, 20, 30, 255};
			if (refusal == 4) fixture.Observation.Ready = false;
			const auto document = fixture.Authored;
			const auto inputs = fixture.Observation.Inputs;
			const auto data = *fixture.Host.PreparedData(1, 1);
			const uint64_t cap = refusal == 1 ? 1 : Limits::MaximumEvaluationBytes;
			const bool applied = membership ? ApplyImageGraphCacheGroupMember(
												  fixture.Authored,
												  history,
												  fixture.Host,
												  fixture.Observation,
												  "cache",
												  "input",
												  fixture.Error,
												  cap
											  )
											: ApplyImageGraphCacheSerialize(
												  fixture.Authored,
												  history,
												  fixture.Host,
												  fixture.Observation,
												  "cache",
												  false,
												  fixture.Error,
												  cap
											  );
			CHECK_FALSE(applied);
			CHECK(fixture.Error.Code != Status::Ok);
			CHECK(fixture.Authored == document);
			CHECK(fixture.Observation.Inputs == inputs);
			CHECK(*fixture.Host.PreparedData(1, 1) == data);
			CHECK_FALSE(history.CanUndo());
			CHECK(history.CanRedo() == (refusal != 0));
			CHECK(fixture.Observation.PendingValueEdit == (refusal == 2));
			CHECK(fixture.Observation.Ready == (refusal != 4));
		}
	}
	for (const bool duplicate : {false, true}) {
		Fixture fixture;
		fixture.Authored.Nodes[1].SourceProperties.push_back({"serialize", std::string{"bad"}});
		if (duplicate) {
			fixture.Authored.Nodes[1].SourceProperties.back().Data = true;
			fixture.Authored.Nodes[1].SourceProperties.push_back({"serialize", true});
		}
		const auto before = fixture.Authored;
		const auto data = *fixture.Host.PreparedData(1, 1);
		studio::ImageGraphHistory history;
		CHECK_FALSE(ApplyImageGraphCacheSerialize(
			fixture.Authored, history, fixture.Host, fixture.Observation, "cache", false, fixture.Error
		));
		CHECK(fixture.Error.Code == Status::InvalidValue);
		CHECK(fixture.Authored == before);
		CHECK(*fixture.Host.PreparedData(1, 1) == data);
		CHECK_FALSE(history.CanUndo());
	}
}
