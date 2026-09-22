#include <engine/assets/ContentHash.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <server/PortalJournal.hpp>

TEST_SUITE_ID("server.portal-journal")

namespace {
	server::PortalJournalRecord Decision(uint64_t sequence = 7) {
		server::PortalJournalRecord record;
		auto &decision = record.Decision;
		decision.Receipt.Id = {"source", 11, sequence};
		decision.Receipt.DestinationWorld = "destination";
		decision.Receipt.Stage = engine::script::PortalTransferStage::Prepared;
		decision.Receipt.Through.Frame = engine::core::CFrame{};
		decision.Receipt.Through.Scale = 1.0f;
		decision.Receipt.Fence.TopologyRevision = 3;
		decision.Receipt.Fence.AuthorityEpoch = 11;
		decision.Receipt.Fence.PrepareRevision = 5;
		decision.Receipt.Fence.BaselineId = 5;
		decision.Receipt.Fence.H = {"source", 42, 43};
		decision.Body.Key = {17, 19};
		decision.Body.Generation = 2;
		decision.Baseline = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
		decision.Receipt.Fence.BaselineHash = engine::assets::Hasher::Of(decision.Baseline);
		return record;
	}

	struct JournalFile {
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() / "atomic-server-portal-journal-tests";
		std::filesystem::path Path = Root / "live" / "portal-transfers.journal";

		JournalFile() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
		~JournalFile() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
	};
}

TEST_CASE(
	"portal journal recovers one sealed decision without duplicating its acknowledgement", "[server][portal]"
) {
	JournalFile journal;
	const server::PortalJournalRecord decision = Decision();
	REQUIRE(server::AppendPortalJournal(journal.Path, std::span{&decision, size_t{1}}));

	std::vector<server::PortalJournalRecord> restored;
	REQUIRE(server::LoadPortalJournal(journal.Path, restored));
	REQUIRE(restored.size() == 1);
	CHECK(server::PortalJournalContains(restored, decision.Decision));
	CHECK(restored.front().Decision.Receipt.Fence == decision.Decision.Receipt.Fence);
	CHECK(restored.front().Decision.Body.Key == decision.Decision.Body.Key);
	CHECK(restored.front().Decision.Baseline == decision.Decision.Baseline);
	CHECK_FALSE(server::PortalJournalContains({}, decision.Decision));
}

TEST_CASE("portal journal preserves separate receipt generations and sequences", "[server][portal]") {
	JournalFile journal;
	const server::PortalJournalRecord first = Decision(7);
	const server::PortalJournalRecord second = Decision(8);
	const std::array records{first, second};
	REQUIRE(server::AppendPortalJournal(journal.Path, records));

	std::vector<server::PortalJournalRecord> restored;
	REQUIRE(server::LoadPortalJournal(journal.Path, restored));
	CHECK(restored.size() == 2);
	CHECK(server::PortalJournalContains(restored, first.Decision));
	CHECK(server::PortalJournalContains(restored, second.Decision));
}

TEST_CASE("portal journal completion closes a recovered decision", "[server][portal]") {
	JournalFile journal;
	const server::PortalJournalRecord commit = Decision();
	auto completed = commit;
	completed.Outcome = server::PortalJournalOutcome::Completed;
	REQUIRE(server::AppendPortalJournal(journal.Path, std::span{&commit, size_t{1}}));
	REQUIRE(server::AppendPortalJournal(journal.Path, std::span{&completed, size_t{1}}));

	std::vector<server::PortalJournalRecord> restored;
	REQUIRE(server::LoadPortalJournal(journal.Path, restored));
	REQUIRE(restored.size() == 1);
	CHECK(restored.front().Outcome == server::PortalJournalOutcome::Completed);
	CHECK(server::PortalJournalContains(restored, commit.Decision));
	CHECK_FALSE(server::PortalJournalPending(restored, commit.Decision));
}

TEST_CASE("portal journal ignores a torn final append and retains prior decisions", "[server][portal]") {
	JournalFile journal;
	const server::PortalJournalRecord decision = Decision();
	REQUIRE(server::AppendPortalJournal(journal.Path, std::span{&decision, size_t{1}}));
	{
		std::ofstream tail(journal.Path, std::ios::binary | std::ios::app);
		const char bytes[] = {'P', 'J', 'N'};
		tail.write(bytes, sizeof(bytes));
	}

	std::vector<server::PortalJournalRecord> restored;
	REQUIRE(server::LoadPortalJournal(journal.Path, restored));
	REQUIRE(restored.size() == 1);
	CHECK(server::PortalJournalContains(restored, decision.Decision));
}

TEST_CASE("portal journal refuses a corrupt record before later durable history", "[server][portal]") {
	JournalFile journal;
	const server::PortalJournalRecord first = Decision(7);
	const server::PortalJournalRecord second = Decision(8);
	REQUIRE(server::AppendPortalJournal(journal.Path, std::span{&first, size_t{1}}));
	{
		std::fstream file(journal.Path, std::ios::binary | std::ios::in | std::ios::out);
		file.seekp(10);
		const char corrupt = '\0';
		file.write(&corrupt, 1);
	}
	REQUIRE(server::AppendPortalJournal(journal.Path, std::span{&second, size_t{1}}));

	std::vector<server::PortalJournalRecord> restored;
	CHECK_FALSE(server::LoadPortalJournal(journal.Path, restored));
}
