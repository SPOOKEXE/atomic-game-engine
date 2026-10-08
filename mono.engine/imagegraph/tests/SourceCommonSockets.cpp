#include <engine/imagegraph/SourceCommonSockets.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_common_sockets")
using namespace engine::imagegraph;
namespace {
	SourceCommonSocketSession Cold(std::span<const SourceCommonOwner> owners) {
		SourceCommonSocketSession session;
		Diagnostic diagnostic;
		REQUIRE(InitializeSourceCommonSockets(owners, session, diagnostic) == Status::Ok);
		return session;
	}
	void Full(SourceCommonSocketSession &session, std::string_view id, std::string_view type) {
		const std::array captures{SourceCommonFullUpdateCapture{id, type, false, true}};
		Diagnostic diagnostic;
		REQUIRE(CompleteSourceCommonFullUpdates(captures, session, diagnostic) == Status::Ok);
	}
} // namespace
TEST_CASE(
	"Common source outputs start cold and explicit construction retires "
	"warm values",
	"[source_common]"
) {
	const std::array owners{
		SourceCommonOwner{"one", "Node_Number"}, SourceCommonOwner{"group", "Node_Group", false}
	};
	auto session = Cold(owners);
	REQUIRE(session.Owners.size() == 2);
	CHECK_FALSE(session.Owners[0].Updated);
	CHECK(session.Owners[0].Name.empty());
	CHECK(session.Owners[0].Position == Vector2{});
	CHECK(session.Owners[1].OwnerType == "Node_Group");
	Full(session, "one", "Node_Number");
	CHECK(session.Owners[0].Updated);
	Diagnostic diagnostic;
	REQUIRE(InitializeSourceCommonSockets(owners, session, diagnostic) == Status::Ok);
	CHECK_FALSE(session.Owners[0].Updated);
}
TEST_CASE("Common step guards retain held metadata and hidden update pulses", "[source_common]") {
	std::array owners{SourceCommonOwner{"one", "Node_Number", true, false, true}};
	auto session = Cold(owners);
	Full(session, "one", "Node_Number");
	std::array captures{SourceCommonStepCapture{
		"one", "Node_Number", {}, {}, SourceCommonMetadataCapture{"actual name", {12, 34}}
	}};
	std::vector<SourceCommonStepReceipt> receipts;
	Diagnostic diagnostic;
	REQUIRE(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::Ok);
	CHECK(session.Owners[0].Updated);
	CHECK(session.Owners[0].Name == "actual name");
	CHECK(session.Owners[0].Position == Vector2{12, 34});
	CHECK(receipts.empty());
	owners[0].OutMeta = false;
	captures[0].Metadata = SourceCommonMetadataCapture{"changed", {56, 78}};
	REQUIRE(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::Ok);
	CHECK(session.Owners[0].Name == "actual name");
	CHECK(session.Owners[0].Position == Vector2{12, 34});
	owners[0].ShowUpdateTrigger = true;
	captures[0].UpdateRequested = false;
	REQUIRE(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::Ok);
	CHECK_FALSE(session.Owners[0].Updated);
	Full(session, "one", "Node_Number");
	CHECK(session.Owners[0].Updated);
}
TEST_CASE(
	"Ordered active step captures report direct input resets without "
	"executing callbacks",
	"[source_common]"
) {
	const std::array owners{
		SourceCommonOwner{"first", "Node_Number", true, true},
		SourceCommonOwner{"inactive", "Node_Group", false, true, true},
		SourceCommonOwner{"last", "Node_Number", true, true}
	};
	auto session = Cold(owners);
	Full(session, "first", "Node_Number");
	Full(session, "inactive", "Node_Group");
	Full(session, "last", "Node_Number");
	const std::array captures{
		SourceCommonStepCapture{"first", "Node_Number", true, true},
		SourceCommonStepCapture{"last", "Node_Number", false}
	};
	std::vector<SourceCommonStepReceipt> receipts{{"prior", true}};
	Diagnostic diagnostic;
	REQUIRE(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::Ok);
	REQUIRE(receipts.size() == 1);
	CHECK(receipts[0] == SourceCommonStepReceipt{"first", true});
	CHECK_FALSE(session.Owners[0].Updated);
	CHECK(session.Owners[1].Updated);
	CHECK_FALSE(session.Owners[2].Updated);
}
TEST_CASE(
	"Incomplete or reordered source step observations preserve the "
	"entire publication",
	"[source_common]"
) {
	const std::array owners{
		SourceCommonOwner{"first", "Node_Number", true, true},
		SourceCommonOwner{"last", "Node_Number", true, true, true}
	};
	auto session = Cold(owners);
	Full(session, "first", "Node_Number");
	const auto prior = session;
	std::vector<SourceCommonStepReceipt> receipts{{"prior", true}};
	const auto priorReceipts = receipts;
	std::array captures{
		SourceCommonStepCapture{"first", "Node_Number", true, true},
		SourceCommonStepCapture{"last", "Node_Number", false}
	};
	Diagnostic diagnostic;
	CHECK(
		BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(session == prior);
	CHECK(receipts == priorReceipts);
	captures[1].Metadata = SourceCommonMetadataCapture{"valid", {1, 2}};
	captures[0].DirectUpdateCompleted = false;
	CHECK(
		BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(session == prior);
	CHECK(receipts == priorReceipts);
	captures[0].DirectUpdateCompleted = true;
	std::swap(captures[0], captures[1]);
	CHECK(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::InvalidValue);
	CHECK(session == prior);
	CHECK(receipts == priorReceipts);
}
TEST_CASE(
	"Full wrapper completion is independent of trigger visibility and "
	"safe mode",
	"[source_common]"
) {
	const std::array owners{SourceCommonOwner{"one", "Node_Number"}, SourceCommonOwner{"two", "Node_Group"}};
	auto session = Cold(owners);
	const std::array captures{
		SourceCommonFullUpdateCapture{"one", "Node_Number", true},
		SourceCommonFullUpdateCapture{"two", "Node_Group", false, true}
	};
	Diagnostic diagnostic;
	REQUIRE(CompleteSourceCommonFullUpdates(captures, session, diagnostic) == Status::Ok);
	CHECK_FALSE(session.Owners[0].Updated);
	CHECK(session.Owners[1].Updated);
	const auto prior = session;
	const std::array incomplete{
		SourceCommonFullUpdateCapture{"one", "Node_Number", false, true},
		SourceCommonFullUpdateCapture{"two", "Node_Group", false, false}
	};
	CHECK(CompleteSourceCommonFullUpdates(incomplete, session, diagnostic) == Status::UnsupportedExecution);
	CHECK(session == prior);
}
TEST_CASE(
	"Changed owner types require explicit constructor reset instead of "
	"inherited lifecycle state",
	"[source_common]"
) {
	std::array owners{SourceCommonOwner{"one", "Node_Number"}};
	auto session = Cold(owners);
	Full(session, "one", "Node_Number");
	const auto prior = session;
	owners[0].OwnerType = "Node_Group";
	const std::array captures{SourceCommonStepCapture{"one", "Node_Group"}};
	std::vector<SourceCommonStepReceipt> receipts;
	Diagnostic diagnostic;
	CHECK(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::InvalidValue);
	CHECK(session == prior);
	const std::array completed{SourceCommonFullUpdateCapture{"one", "Node_Group", false, true}};
	CHECK(CompleteSourceCommonFullUpdates(completed, session, diagnostic) == Status::InvalidValue);
	CHECK(session == prior);
	REQUIRE(InitializeSourceCommonSockets(owners, session, diagnostic) == Status::Ok);
	CHECK(session.Owners[0].OwnerType == "Node_Group");
	CHECK_FALSE(session.Owners[0].Updated);
}
TEST_CASE(
	"Common metadata and residency limits refuse before replacing state "
	"or reset receipts",
	"[source_common]"
) {
	const std::array owners{SourceCommonOwner{"one", "Node_Number", true, true, true}};
	auto session = Cold(owners);
	Full(session, "one", "Node_Number");
	const auto prior = session;
	std::vector<SourceCommonStepReceipt> receipts{{"retained", true}};
	const auto priorReceipts = receipts;
	std::array captures{SourceCommonStepCapture{
		"one", "Node_Number", true, true, SourceCommonMetadataCapture{"valid", {3, 4}}
	}};
	Diagnostic diagnostic;
	const auto onlyPublished =
		RetainedSourceCommonSocketBytes(session) + RetainedSourceCommonReceiptBytes(receipts);
	CHECK(
		BeginSourceCommonStep(owners, captures, session, receipts, diagnostic, onlyPublished) ==
		Status::LimitExceeded
	);
	CHECK(session == prior);
	CHECK(receipts == priorReceipts);
	std::string huge(Limits::MaximumTextBytes + 1, 'x');
	captures[0].Metadata->Name = huge;
	CHECK(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::LimitExceeded);
	CHECK(session == prior);
	CHECK(receipts == priorReceipts);
	captures[0].Metadata->Name = "valid";
	captures[0].Metadata->Position.X = std::numeric_limits<double>::infinity();
	CHECK(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::InvalidValue);
	CHECK(session == prior);
	CHECK(receipts == priorReceipts);
}
TEST_CASE("Duplicate lifecycle identities and surplus inactive captures are rejected", "[source_common]") {
	const std::array duplicate{
		SourceCommonOwner{"one", "Node_Number"}, SourceCommonOwner{"one", "Node_Group"}
	};
	SourceCommonSocketSession session;
	Diagnostic diagnostic;
	CHECK(InitializeSourceCommonSockets(duplicate, session, diagnostic) == Status::DuplicateId);
	CHECK(session.Owners.empty());
	const std::array owners{SourceCommonOwner{"one", "Node_Number", false}};
	session = Cold(owners);
	const auto prior = session;
	const std::array captures{SourceCommonStepCapture{"one", "Node_Number"}};
	std::vector<SourceCommonStepReceipt> receipts;
	CHECK(BeginSourceCommonStep(owners, captures, session, receipts, diagnostic) == Status::InvalidValue);
	CHECK(session == prior);
	const std::array completed{
		SourceCommonFullUpdateCapture{"one", "Node_Number", false, true},
		SourceCommonFullUpdateCapture{"one", "Node_Number", true}
	};
	CHECK(CompleteSourceCommonFullUpdates(completed, session, diagnostic) == Status::DuplicateId);
	CHECK(session == prior);
}

TEST_CASE("Common metadata text is scanned linearly across many owner completions", "[source_common]") {
	std::vector<std::string> ids;
	ids.reserve(64);
	for (size_t index = 0; index < 64; ++index)
		ids.push_back("owner-" + std::to_string(index));
	const std::string name(4096, 'n');
	std::vector<SourceCommonOwner> owners;
	std::vector<SourceCommonStepCapture> steps;
	std::vector<SourceCommonFullUpdateCapture> full;
	for (const auto &id : ids) {
		owners.push_back({id, "Node_Number", true, false, true});
		steps.push_back({id, "Node_Number", {}, {}, SourceCommonMetadataCapture{name, {7, 8}}});
		full.push_back({id, "Node_Number", false, true});
	}
	auto session = Cold(owners);
	Diagnostic diagnostic;
	std::vector<SourceCommonStepReceipt> receipts;
	REQUIRE(BeginSourceCommonStep(owners, steps, session, receipts, diagnostic) == Status::Ok);
	REQUIRE(BeginSourceCommonStep(owners, steps, session, receipts, diagnostic) == Status::Ok);
	REQUIRE(CompleteSourceCommonFullUpdates(full, session, diagnostic) == Status::Ok);
	CHECK(receipts.empty());
	for (const auto &state : session.Owners) {
		CHECK(state.Name == name);
		CHECK(state.Position == Vector2{7, 8});
		CHECK(state.Updated);
	}
}
