#include "../src/SourceCommonAuthoring.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_common_authoring")
using namespace engine::imagegraph;

namespace {
	Document Authored() {
		Document document;
		document.FormatVersion = 11;
		document.Nodes.push_back({"number", "pc.number", {}, {1.25, 2.5}, {}, {}});
		document.Groups.push_back({"scope", ""});
		document.Groups.back().SourcePosition = {3.125, -4.5};
		document.Groups.back().SourceInternalName = "collection_runtime";
		SourceCommonOwnerRecord collection;
		collection.SourceOwnerId = "collection";
		collection.SourceType = "Node_Collection";
		collection.NativeOwnerKind = SourceCommonNativeOwnerKind::Group;
		collection.NativeOwnerId = "scope";
		collection.OutMeta = true;
		collection.DisplayNamePresent = true;
		collection.UpdateAnimatorOwnerId = "collection";
		collection.UpdateAnimatorPort = "native:animator:0";
		collection.UpdateOverrideInstance = true;
		collection.UpdateExpression = SourceInputExpression{"pxcx.update_in_trigger", "false", false};
		SourceCommonOwnerRecord number;
		number.SourceOwnerId = "number";
		number.SourceType = "Node_Number";
		number.NativeOwnerId = "number";
		number.ShowUpdateTrigger = true;
		number.UpdateGraph = false;
		number.UpdateAnimatorOwnerId = "number";
		number.UpdateAnimatorPort = "native:animator:1";
		document.SourceCommonOwners = {collection, number};
		document.SourceAnimators.emplace();
		for (const auto &owner : document.SourceCommonOwners) {
			DetachedSourceAnimator animator;
			animator.Id = owner.UpdateAnimatorPort;
			animator.OwnerId = owner.SourceOwnerId;
			animator.OriginalPort = "pxcx.update_in_trigger";
			animator.Type = ValueType::Boolean;
			document.SourceAnimators->Detached.push_back(animator);
			GroupSubtypeOverlay payload;
			payload.NodeId = owner.SourceOwnerId;
			payload.Port = owner.UpdateAnimatorPort;
			payload.Fixed = false;
			document.SourceAnimators->DetachedValues.push_back(payload);
		}
		return document;
	}
}

TEST_CASE(
	"Common authoring preserves mixed owner order and sole metadata locations", "[imagegraph][common]"
) {
	const auto document = Authored();
	const auto text = Write(document);
	REQUIRE_FALSE(text.empty());
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK(restored.SourceCommonOwners.front().SourceOwnerId == "collection");
	CHECK(restored.SourceCommonOwners.front().DisplayNamePresent);
	CHECK(restored.Groups.front().Name.empty());
	CHECK_FALSE(restored.SourceCommonOwners.back().DisplayNamePresent);
	CHECK_FALSE(restored.SourceCommonOwners.back().UpdateGraph);
	CHECK(restored.SourceCommonOwners.front().UpdateGraph);
	CHECK(restored.Nodes.front().SourceDisplayName.empty());
	REQUIRE(restored.SourceCommonOwners.front().UpdateExpression);
	CHECK_FALSE(restored.SourceCommonOwners.front().UpdateExpression->Enabled);
	CHECK_FALSE(restored.SourceCommonOwners.back().UpdateExpression);
	CHECK((restored.Groups.front().SourcePosition == Vector2{3.125, -4.5}));
	CHECK(restored.SourceAnimators->DetachedValues.size() == 2);
}

TEST_CASE(
	"Common authoring accepts animated local storage and preserves legacy absence", "[imagegraph][common]"
) {
	auto document = Authored();
	auto &metadata = document.SourceAnimators->Detached.back();
	document.SourceCommonOwners.back().DisplayNamePresent = true;
	metadata.Writer = GroupSubtypeAnimator::Animated;
	metadata.Track = AnimationTrack{metadata.OwnerId, metadata.Id, "hold", -1};
	auto &payload = document.SourceAnimators->DetachedValues.back();
	payload.Fixed.reset();
	Keyframe key;
	key.NodeId = payload.NodeId;
	key.Port = payload.Port;
	key.Tick = 2;
	key.Data = true;
	key.Interpolation = "step";
	payload.Keys.push_back(key);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Document legacy;
	legacy.FormatVersion = 10;
	legacy.Nodes = document.Nodes;
	REQUIRE(Read(Write(legacy), restored, diagnostic) == Status::Ok);
	CHECK(restored.SourceCommonOwners.empty());
	CHECK(restored == legacy);
}

TEST_CASE(
	"Common authored identity and expression failures preserve replacement document", "[imagegraph][common]"
) {
	const auto valid = Authored();
	for (int fault = 0; fault < 9; ++fault) {
		auto document = valid;
		switch (fault) {
		case 0:
			document.SourceCommonOwners.back().SourceOwnerId = "collection";
			break;
		case 1:
			document.SourceCommonOwners.back().NativeOwnerId = "gone";
			break;
		case 2:
			document.SourceCommonOwners.back().InstanceBase = "number";
			break;
		case 3:
			document.SourceCommonOwners.back().UpdateAnimatorPort = "native:animator:99";
			break;
		case 4:
			document.SourceCommonOwners.front().UpdateExpression->Port = "ordinary";
			break;
		case 5:
			document.SourceCommonOwners.front().UpdateExpression->Code = std::string(1, char(0xff));
			break;
		case 6:
			document.SourceCommonOwners.back().UpdateAnimatorOwnerId = "collection";
			break;
		case 7:
			document.FormatVersion = 10;
			break;
		case 8:
			document.Nodes.front().SourceProperties.push_back({"update_graph", false});
			break;
		}
		INFO(fault);
		CHECK(Write(document).empty());
		Diagnostic diagnostic;
		CHECK(detail::ValidateSourceCommonOwners(document, diagnostic) != Status::Ok);
	}
	Document prior = valid;
	Diagnostic diagnostic;
	const auto text = Write(valid);
	const auto marker = text.find("source_common_owner ");
	REQUIRE(marker != std::string::npos);
	const auto end = text.find('\n', marker);
	const auto duplicated = text + text.substr(marker, end - marker + 1);
	CHECK(Read(duplicated, prior, diagnostic) == Status::DuplicateId);
	CHECK(prior == valid);
	const auto groupMarker = text.find("group_source_position ");
	REQUIRE(groupMarker != std::string::npos);
	const auto groupEnd = text.find('\n', groupMarker);
	const auto repeatedPosition = text + text.substr(groupMarker, groupEnd - groupMarker + 1);
	CHECK(Read(repeatedPosition, prior, diagnostic) == Status::Malformed);
	CHECK(prior == valid);
	CHECK(Read(std::string_view(text), prior, diagnostic, 1) == Status::LimitExceeded);
	CHECK(prior == valid);
}

TEST_CASE("Common authored codec refuses malformed owner kinds and flags", "[imagegraph][common]") {
	const auto document = Authored();
	for (const std::string_view fields :
		 {"alien 1 0 0 0 0 1", "node 2 0 0 0 0 1", "node 1 0 0 0 0 1 extra", "node 1 0 0 0 0 2"}) {
		std::string text = "imagegraph 11\nsource_common_owner \"id\" \"Node_Number\" \"id\" \"\" \"id\" "
						   "\"native:animator:0\" ";
		text += fields;
		text += '\n';
		Document prior = document;
		Diagnostic diagnostic;
		CHECK(Read(text, prior, diagnostic) == Status::Malformed);
		CHECK(prior == document);
	}
}

TEST_CASE("Group-only common authoring retains exact source coordinates", "[imagegraph][common]") {
	auto document = Authored();
	document.Nodes.clear();
	document.SourceCommonOwners.resize(1);
	document.SourceAnimators->Detached.resize(1);
	document.SourceAnimators->DetachedValues.resize(1);
	document.Groups.front().SourcePosition = {3.123456789, -7.987654321};
	const auto text = Write(document);
	REQUIRE_FALSE(text.empty());
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK((restored.Groups.front().SourcePosition == Vector2{3.123456789, -7.987654321}));
}

TEST_CASE(
	"Group source metadata roundtrips without a common lifecycle registration", "[imagegraph][common]"
) {
	Document document;
	document.FormatVersion = 11;
	document.Groups.push_back({"group", "editor label"});
	document.Groups.front().SourcePosition = {3.123456789, -8.987654321};
	document.Groups.front().SourceInternalName = "source_iname";
	CHECK(document.SourceCommonOwners.empty());
	Diagnostic diagnostic;
	REQUIRE(detail::ValidateSourceCommonOwners(document, diagnostic) == Status::Ok);
	const auto text = Write(document);
	REQUIRE_FALSE(text.empty());
	Document restored;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	CHECK(restored.SourceCommonOwners.empty());
}

TEST_CASE(
	"Group source metadata requires its codec version even without lifecycle owners", "[imagegraph][common]"
) {
	for (int fields = 1; fields <= 3; ++fields) {
		Document document;
		document.FormatVersion = 10;
		document.Groups.push_back({"group", "editor label"});
		if (fields & 1) document.Groups.front().SourcePosition = {1.125, -2.25};
		if (fields & 2) document.Groups.front().SourceInternalName = "source_iname";
		INFO(fields);
		Diagnostic diagnostic;
		CHECK(detail::ValidateSourceCommonOwners(document, diagnostic) == Status::UnsupportedVersion);
		CHECK(Write(document).empty());
	}
	Document legacy;
	legacy.FormatVersion = 10;
	legacy.Groups.push_back({"group", "editor label"});
	Diagnostic diagnostic;
	REQUIRE(detail::ValidateSourceCommonOwners(legacy, diagnostic) == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(legacy), restored, diagnostic) == Status::Ok);
	CHECK(restored == legacy);
}
