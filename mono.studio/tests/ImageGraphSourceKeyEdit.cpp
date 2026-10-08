#include "../src/ImageGraphSourceKeyEdit.hpp"

#include "../src/KeyframeKindEditor.hpp"
#include "../src/TimelineDopesheet.hpp"
#include "../src/TimelineEaseEditor.hpp"

#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("studio.imagegraph.source_key_edit")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.imagegraph.group_replay")
namespace {
	using namespace engine::imagegraph;
	Document ImportedAliases() {
		using Json = nlohmann::json;
		const auto *entry = FindCatalogueEntry("pc.invert");
		REQUIRE(entry);
		const auto *input = FindCatalogueInput(*entry, "mix");
		REQUIRE(input);
		Json inputs = Json::array();
		for (int32_t i = 0; i <= input->SourceIndex; ++i)
			inputs.push_back(Json{{"r", {{"d", -4}}}});
		inputs[input->SourceIndex] = {
			{"anim", true},
			{"r",
			 Json::array(
				 {Json::array(
					  {Json::array({0, 0}), .25, Json::array({0, 1}), Json::array({0, 0}), 0, 0, 0, 0}
				  ),
				  Json::array(
					  {Json::array({0, 10}), .75, Json::array({0, 1}), Json::array({0, 0}), 0, 0, 0, 0}
				  )}
			 )}
		};
		Json nodes = Json::array();
		for (const char *id : {"owner", "alias", "sibling"}) {
			Json node{{"id", id}, {"type", entry->SourceNode}, {"inputs", inputs}, {"x", 0}, {"y", 0}};
			if (std::string_view(id) != "owner") node["instanceBase"] = "owner";
			nodes.push_back(std::move(node));
		}
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = Json{{"nodes", nodes}}.dump() + '\0';
		std::string failure;
		std::vector<std::byte> bytes;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		engine::imagegraphio::PxcxImport imported;
		REQUIRE(engine::imagegraphio::ImportPxcxImageGraph(archive, imported, failure));
		std::string importDiagnostics;
		for (const auto &diagnostic : imported.Diagnostics) {
			if (!importDiagnostics.empty()) importDiagnostics += '\n';
			importDiagnostics += diagnostic.Message;
		}
		INFO(importDiagnostics);
		Diagnostic migration;
		REQUIRE(Migrate(imported.Graph, migration) == Status::Ok);
		REQUIRE(imported.Graph.Nodes.size() == 3);
		const auto alias =
			std::find_if(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const auto &node) {
				return node.Id == "alias";
			});
		REQUIRE(alias != imported.Graph.Nodes.end());
		REQUIRE(alias->Type == "pc.invert");
		REQUIRE(alias->InstanceBase == "owner");
		REQUIRE(imported.Graph.SourceAnimators);
		CHECK(imported.Graph.SourceAnimators->Bindings.empty());
		REQUIRE(imported.Graph.SourceCommonOwners.size() == 3);
		const auto &commonAnimators = *imported.Graph.SourceAnimators;
		CHECK(commonAnimators.Detached.size() == imported.Graph.SourceCommonOwners.size());
		CHECK(commonAnimators.DetachedValues.size() == imported.Graph.SourceCommonOwners.size());
		for (const auto &owner : imported.Graph.SourceCommonOwners) {
			CHECK(owner.UpdateAnimatorOwnerId == owner.SourceOwnerId);
			const auto metadata = std::find_if(
				commonAnimators.Detached.begin(), commonAnimators.Detached.end(), [&](const auto &row) {
					return row.OwnerId == owner.UpdateAnimatorOwnerId && row.Id == owner.UpdateAnimatorPort;
				}
			);
			REQUIRE(metadata != commonAnimators.Detached.end());
			CHECK(metadata->OriginalPort == "pxcx.update_in_trigger");
			const auto payload = std::find_if(
				commonAnimators.DetachedValues.begin(),
				commonAnimators.DetachedValues.end(),
				[&](const auto &row) {
					return row.NodeId == owner.UpdateAnimatorOwnerId && row.Port == owner.UpdateAnimatorPort;
				}
			);
			REQUIRE(payload != commonAnimators.DetachedValues.end());
		}
		imported.Graph.Outputs = {{"result", "owner", "surface_out"}};
		REQUIRE(
			std::count_if(
				imported.Graph.Keyframes.begin(), imported.Graph.Keyframes.end(), [](const auto &key) {
					return key.Port == "mix";
				}
			) == 6
		);
		return imported.Graph;
	}
	const Keyframe &Key(const Document &document, std::string_view node, uint64_t tick = 0) {
		const auto key =
			std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == node && key.Port == "mix" && key.Tick == tick;
			});
		REQUIRE(key != document.Keyframes.end());
		return *key;
	}
	void RestoreHost(const Document &document, studio::ImageGraphGroupHost &host, uint64_t revision) {
		host.Clear();
		Plan plan;
		Diagnostic error;
		REQUIRE(Compile(document, plan, error) == Status::Ok);
		EvaluationRequest request;
		const bool restored = host.Prepare(document, plan, revision, request, error);
		INFO(error.Message);
		REQUIRE(restored);
	}
	template <class Edit>
	bool EditPin(
		Document &document,
		studio::ImageGraphHistory &history,
		studio::ImageGraphGroupHost &host,
		const Keyframe &pin,
		const Edit &edit,
		Diagnostic &error
	) {
		return studio::ApplyImageGraphSourceKeyEdit(
			document,
			history,
			host,
			1,
			{},
			[&](Document &candidate, uint64_t available) {
				return studio::EditImageGraphPinnedKey(candidate, document, pin, available, edit, error);
			},
			error
		);
	}
}
TEST_CASE(
	"First imported alias edit captures undo writers and shared key gestures", "[studio][source_key_edit]"
) {
	auto document = ImportedAliases();
	for (auto &key : document.Keyframes)
		key.SourceKeyId = key.NodeId + "-" + std::to_string(key.Tick);
	std::reverse(document.Keyframes.begin(), document.Keyframes.end());
	const auto original = document;
	const std::vector<Keyframe> pins{Key(document, "alias")};
	studio::TimelineKeyEditor editor;
	editor.Selection = {{"alias", "mix", {}}};
	Diagnostic error;
	REQUIRE(editor.Begin(document, false, {}, error));
	editor.Destination = {2, .5, false};
	studio::ImageGraphHistory history;
	studio::ImageGraphGroupHost host;
	const bool accepted = studio::ApplyImageGraphSourceKeyEdit(
		document,
		history,
		host,
		1,
		{},
		[&](Document &candidate, uint64_t available) {
			return studio::WithImageGraphProjectedKeyPins(
				document,
				candidate,
				editor.Originals,
				available,
				[&](auto mapped, uint64_t remaining) {
					return editor.PrepareCommit(candidate, error, remaining, mapped);
				},
				error
			);
		},
		error
	);
	INFO(error.Message);
	REQUIRE(accepted);
	editor.PublishCommit();
	REQUIRE(document.SourceAnimators);
	CHECK(host.Revision == 2);
	CHECK(document.SourceAnimators->Bindings.size() >= 2);
	for (const char *id : {"owner", "alias", "sibling"}) {
		const auto key =
			std::find_if(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
				return key.NodeId == id && GetFrameTime(key) == editor.Destination;
			});
		REQUIRE(key != document.Keyframes.end());
		CHECK(key->Data == Value{.25});
	}
	const auto changed = document;
	REQUIRE(history.Undo(document));
	REQUIRE(document.SourceAnimators);
	CHECK(Key(document, "owner").Data == Key(original, "owner").Data);
	CHECK(Key(document, "alias").Data == Key(original, "alias").Data);
	CHECK_FALSE(history.CanUndo());
	RestoreHost(document, host, 3);
	REQUIRE(host.Replay.Binding("alias", "mix"));
	CHECK(host.Replay.Binding("alias", "mix")->OwnerId == "owner");
	REQUIRE(history.Redo(document));
	CHECK(document == changed);
	Document reopened;
	REQUIRE(Read(Write(document), reopened, error) == Status::Ok);
	CHECK(reopened == changed);
	RestoreHost(reopened, host, 4);
	CHECK(host.Replay.Binding("alias", "mix")->OwnerId == "owner");
}
TEST_CASE(
	"Initial alias metadata edits map cleared ids and canonical row ordering", "[studio][source_key_edit]"
) {
	for (int operation = 0; operation < 7; ++operation) {
		INFO(operation);
		auto document = ImportedAliases();
		for (auto &key : document.Keyframes)
			key.SourceKeyId = key.NodeId + "-" + std::to_string(key.Tick);
		std::reverse(document.Keyframes.begin(), document.Keyframes.end());
		const auto pin = Key(document, "alias");
		studio::ImageGraphHistory history;
		studio::ImageGraphGroupHost host;
		Diagnostic error;
		const bool accepted = EditPin(
			document,
			history,
			host,
			pin,
			[&](Document &candidate, size_t index, uint64_t available) {
				switch (operation) {
				case 0:
					return studio::SetImageGraphKeyframeKind(
						candidate, index, KeyframeKind::Adder, error, available
					);
				case 1:
					return studio::SetImageGraphKeyframeInterpolation(
						candidate, index, "linear", error, available
					);
				case 2: {
					KeyframeEase ease;
					ease.OutType = "bezier";
					ease.Out.X = .75;
					return studio::SetImageGraphKeyframeEase(candidate, index, ease, error, available);
				}
				case 3:
					return studio::SetImageGraphKeyframeSourceDriver(
						candidate, index, KeyframeLinearDriver{.125}, error, available
					);
				case 4:
					return studio::SetImageGraphKeyframeSineDriver(
						candidate, index, KeyframeSineDriver{}, error, available
					);
				case 5:
					return studio::SetImageGraphAnimationTrack(
						candidate, "alias", "mix", "loop", 1, error, available
					);
				default:
					return studio::RemoveImageGraphKeyframe(
						candidate, "alias", "mix", 0, error, 0, false, available
					);
				}
			},
			error
		);
		INFO(error.Message);
		REQUIRE(accepted);
		if (operation != 5 && operation != 6) {
			auto owner = Key(document, "owner"), alias = Key(document, "alias"),
				 sibling = Key(document, "sibling");
			owner.NodeId = alias.NodeId = sibling.NodeId = "same";
			owner.SourceKeyId = alias.SourceKeyId = sibling.SourceKeyId = {};
			CHECK(owner == alias);
			CHECK(owner == sibling);
		}
		if (operation == 6)
			CHECK(std::count_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
					  return key.Port == "mix";
				  }) == 3);
		if (operation == 5)
			for (const auto &track : document.Tracks)
				if (track.Port == "mix") CHECK(track.End == "loop");
		REQUIRE(history.Undo(document));
		REQUIRE(document.SourceAnimators);
		RestoreHost(document, host, 3);
		REQUIRE(history.Redo(document));
	}
}
TEST_CASE(
	"Source key edit refusals preserve live host document and both history stacks",
	"[studio][source_key_edit]"
) {
	for (int refusal = 0; refusal < 5; ++refusal) {
		auto document = ImportedAliases();
		studio::ImageGraphHistory history(refusal == 1 ? 0 : 128);
		Document future;
		if (refusal != 1) {
			const auto first = document;
			document.Outputs.front().Id = "second";
			REQUIRE(history.TryRecord(first, document));
			future = document;
			future.Outputs.front().Id = "third";
			REQUIRE(history.TryRecord(document, future));
			document = future;
			REQUIRE(history.Undo(document));
		}
		studio::ImageGraphGroupHost host;
		RestoreHost(document, host, 1);
		const auto original = document;
		const auto *retained = host.Replay.Binding("alias", "mix");
		const auto revision = host.Revision;
		Diagnostic error;
		const bool accepted = studio::ApplyImageGraphSourceKeyEdit(
			document,
			history,
			host,
			1,
			{},
			[&](Document &candidate, uint64_t available) {
				if (refusal == 0) return false;
				if (refusal == 4) available = 1;
				return studio::SetImageGraphAnimationTrack(
					candidate, "alias", "mix", "loop", 1, error, available
				);
			},
			error,
			refusal == 2 ? 1 : Limits::MaximumEvaluationBytes,
			nullptr,
			[&](auto &, auto *, uint64_t) { return refusal != 3; }
		);
		CHECK_FALSE(accepted);
		CHECK(document == original);
		CHECK(host.Revision == revision);
		CHECK(host.Replay.Binding("alias", "mix") == retained);
		CHECK(history.CanUndo() == (refusal != 1));
		CHECK(history.CanRedo() == (refusal != 1));
		if (refusal != 1) {
			REQUIRE(history.Redo(document));
			CHECK(document == future);
			REQUIRE(history.Undo(document));
			CHECK(document == original);
		}
	}
}
TEST_CASE(
	"Captured key pins refuse stale payloads and preserve writer identity on no-op",
	"[studio][source_key_edit]"
) {
	auto document = ImportedAliases();
	const auto original = document;
	studio::ImageGraphHistory history;
	studio::ImageGraphGroupHost host;
	Diagnostic error;
	auto stale = Key(document, "alias");
	stale.Data = .99;
	CHECK_FALSE(EditPin(
		document,
		history,
		host,
		stale,
		[&](Document &candidate, size_t index, uint64_t available) {
			return studio::SetImageGraphKeyframeKind(candidate, index, KeyframeKind::Adder, error, available);
		},
		error
	));
	CHECK(document == original);
	CHECK(host.Revision == 0);
	bool unchanged = false;
	CHECK_FALSE(
		studio::ApplyImageGraphSourceKeyEdit(
			document,
			history,
			host,
			1,
			{},
			[](Document &, uint64_t) { return true; },
			error,
			Limits::MaximumEvaluationBytes,
			&unchanged
		)
	);
	CHECK(unchanged);
	CHECK(document == original);
	CHECK(host.Revision == 0);
	CHECK_FALSE(history.CanUndo());
}
TEST_CASE(
	"Key edits clear projected group overlays before restoring declarations", "[studio][source_key_edit]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"input",
		 "pc.group_input",
		 "group",
		 {},
		 {{"input_type", EnumValue{1}},
		  {"subtype", EnumValue{0}},
		  {"vector_size", EnumValue{0}},
		  {"parent_value", .5}}},
		{"output", "pc.group_output", "group", {}, {}}
	};
	document.Nodes[0].SourceAnimatedInputs = {"parent_value"};
	document.Groups = {
		{"group",
		 "Group",
		 {},
		 {{"input", "input/parent", PortDirection::Input, "input"},
		  {"output", "output/parent", PortDirection::Output, "output"}}}
	};
	document.Junctions = {
		{"input/parent", "group", ValueType::Any, .5},
		{"output/parent", "group", ValueType::Any, std::nullopt}
	};
	document.Links = {{"input", "value", "output", "value"}, {"output", "value", "output/parent", "value"}};
	document.Outputs = {{"result", "output", "value"}};
	studio::ImageGraphGroupHost host;
	RestoreHost(document, host, 1);
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	Value pressed = .9;
	GroupRefreshEvent event;
	event.NodeId = "input";
	event.Reason = GroupRefreshReason::ParentEdit;
	event.EditedPort = "parent_value";
	event.LocalValue = &pressed;
	event.LocalAnimated = true;
	event.At.Tick = 5;
	GroupReplayState edited;
	REQUIRE(ReplayGroupRefresh(document, plan, {&event, 1}, host.Replay, 1, edited, error) == Status::Ok);
	host.Replay = std::move(edited);
	REQUIRE(host.Replay.Find("input"));
	REQUIRE_FALSE(host.Replay.Find("input")->ParentKeys.empty());
	studio::ImageGraphHistory history;
	const bool accepted = studio::ApplyImageGraphSourceKeyEdit(
		document,
		history,
		host,
		1,
		{},
		[&](Document &candidate, uint64_t available) {
			return studio::SetImageGraphKeyframe(
				candidate, "input", "parent_value", 5, "source", error, 0, false, available
			);
		},
		error
	);
	INFO(error.Message);
	REQUIRE(accepted);
	CHECK(host.Replay.Find("input")->ParentKeys.empty());
	CHECK_FALSE(host.Replay.Find("input")->ParentReset);
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = 2;
	request.Tick = 5;
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "result", request, value, error) == Status::Ok);
	CHECK(value.Data == Value{.5});
	REQUIRE(history.Undo(document));
	RestoreHost(document, host, 3);
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	request.GroupReplay = &host.Replay;
	request.GroupAuthoringRevision = 3;
	REQUIRE(EvaluateValue(document, plan, "result", request, value, error) == Status::Ok);
	CHECK(value.Data == Value{.9});
}
TEST_CASE("Initial Dopesheet deletion and selected easing map alias pins", "[studio][source_key_edit]") {
	for (const bool deleting : {false, true}) {
		auto document = ImportedAliases();
		for (auto &key : document.Keyframes)
			key.SourceKeyId = key.NodeId + "-" + std::to_string(key.Tick);
		studio::TimelineKeyEditor editor;
		editor.Selection = {{"alias", "mix", {}}, {"sibling", "mix", {}}};
		studio::TimelineDopesheet sheet;
		studio::TimelineEaseEditor ease;
		Diagnostic error;
		if (deleting)
			REQUIRE(sheet.BeginDeletion(document, editor, error));
		else {
			REQUIRE(ease.Begin(document, editor, error));
			ease.Delta = .125;
		}
		studio::ImageGraphHistory history;
		studio::ImageGraphGroupHost host;
		const auto &originals = deleting ? sheet.Originals : ease.Originals;
		const bool accepted = studio::ApplyImageGraphSourceKeyEdit(
			document,
			history,
			host,
			1,
			{},
			[&](Document &candidate, uint64_t available) {
				return studio::WithImageGraphProjectedKeyPins(
					document,
					candidate,
					originals,
					available,
					[&](auto mapped, uint64_t remaining) {
						return deleting ? sheet.PrepareCommit(candidate, editor, error, remaining, mapped)
										: ease.PrepareCommit(candidate, error, remaining, mapped);
					},
					error
				);
			},
			error
		);
		INFO(error.Message);
		REQUIRE(accepted);
		if (deleting) {
			sheet.PublishCommit(editor);
			CHECK(std::count_if(document.Keyframes.begin(), document.Keyframes.end(), [](const auto &key) {
					  return key.Port == "mix";
				  }) == 3);
			CHECK(editor.Selection.empty());
		} else {
			REQUIRE(Key(document, "owner").Ease);
			CHECK(Key(document, "owner").Ease->Out.X == .125);
			CHECK(Key(document, "alias").Ease == Key(document, "owner").Ease);
		}
		REQUIRE(history.Undo(document));
		RestoreHost(document, host, 3);
		REQUIRE(history.Redo(document));
	}
}
TEST_CASE("Initial key authoring and track removal share captured history", "[studio][source_key_edit]") {
	auto document = ImportedAliases();
	studio::ImageGraphGroupHost host;
	studio::ImageGraphHistory history;
	Diagnostic error;
	REQUIRE(studio::SetImageGraphValue(document, "alias", "mix", .5, error));
	const bool added = studio::ApplyImageGraphSourceKeyEdit(
		document,
		history,
		host,
		1,
		{},
		[&](Document &candidate, uint64_t available) {
			return studio::SetImageGraphKeyframe(
				candidate, "alias", "mix", 7, "linear", error, .25, false, available
			);
		},
		error
	);
	INFO(error.Message);
	REQUIRE(added);
	for (const char *node : {"owner", "alias", "sibling"}) {
		CHECK(Key(document, node, 7).Subframe == .25);
		CHECK(Key(document, node, 7).Data == Value{.5});
		CHECK(Key(document, node, 7).Interpolation == "linear");
	}
	REQUIRE(
		studio::ApplyImageGraphSourceKeyEdit(
			document,
			history,
			host,
			2,
			{},
			[&](Document &candidate, uint64_t available) {
				return studio::RemoveImageGraphAnimationTrack(candidate, "alias", "mix", error, available);
			},
			error
		)
	);
	CHECK(std::none_of(document.Tracks.begin(), document.Tracks.end(), [](const auto &track) {
		return track.Port == "mix";
	}));
	REQUIRE(history.Undo(document));
	CHECK(std::any_of(document.Tracks.begin(), document.Tracks.end(), [](const auto &track) {
		return track.Port == "mix";
	}));
	REQUIRE(history.Undo(document));
	CHECK(bool(document.SourceAnimators));
	REQUIRE(history.Redo(document));
	REQUIRE(history.Redo(document));
	CHECK(std::none_of(document.Tracks.begin(), document.Tracks.end(), [](const auto &track) {
		return track.Port == "mix";
	}));
}

TEST_CASE(
	"Projected pin payload comparisons are admitted before draft allocation", "[studio][source_key_edit]"
) {
	Document authored;
	authored.Nodes = {{"text", "pc.text", {}, {}, {}}};
	Keyframe key{"text", "text", 0, std::string(100 * 1024, 'x'), "step"};
	for (size_t i = 0; i < 256; ++i) {
		key.Subframe = double(i) / 512;
		authored.Keyframes.push_back(key);
	}
	auto projected = authored;
	Diagnostic error;
	bool called = false;
	CHECK_FALSE(
		studio::WithImageGraphProjectedKeyPins(
			authored,
			projected,
			authored.Keyframes,
			Limits::MaximumEvaluationBytes,
			[&](auto, uint64_t) {
				called = true;
				return true;
			},
			error
		)
	);
	CHECK(error.Code == Status::LimitExceeded);
	CHECK_FALSE(called);
	CHECK(projected == authored);
}
