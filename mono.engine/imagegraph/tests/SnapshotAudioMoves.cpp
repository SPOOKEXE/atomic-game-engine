#include "../src/SnapshotAudioMoves.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.snapshot_audio_moves")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;
TEST_CASE(
	"Snapshot audio moves transfer admitted capacities without "
	"reallocating samples",
	"[imagegraph][evaluation_snapshot]"
) {
	EvaluationBudget budget(4096);
	auto source = budget.Reserve(1024);
	auto destination = budget.Reserve(16);
	REQUIRE(source);
	REQUIRE(destination);
	AudioBit a{{1, 2, 3}, 8}, b{{4, 5, 6}, 8}, targetA, targetB;
	const auto *aData = a.Samples.data();
	const auto *bData = b.Samples.data();
	const uint64_t bytesA = RetainedPayloadBytes(a), bytesB = RetainedPayloadBytes(b);
	std::array moves{
		SnapshotAudioMove{&a, &targetA, &*source, bytesA}, SnapshotAudioMove{&b, &targetB, &*source, bytesB}
	};
	const auto before = budget.Used();
	REQUIRE(PrepareSnapshotAudioMoves(moves, *destination));
	CHECK(source->Bytes() == 1024);
	CHECK(destination->Bytes() == 16);
	CommitSnapshotAudioMoves(moves, *destination);
	CHECK(budget.Used() == before);
	CHECK(source->Bytes() == 1024 - bytesA - bytesB);
	CHECK(destination->Bytes() == 16 + bytesA + bytesB);
	CHECK(targetA.Samples.data() == aData);
	CHECK(targetB.Samples.data() == bData);
	CHECK(targetA.Samples == std::vector<double>{1, 2, 3});
	CHECK(targetB.Samples == std::vector<double>{4, 5, 6});
	CHECK(targetA.SampleRate == 8);
	CHECK(targetB.SampleRate == 8);
	CHECK(RetainedPayloadBytes(a) == 0);
	CHECK(RetainedPayloadBytes(b) == 0);
}
TEST_CASE(
	"Snapshot audio preflight refuses distinct-ledger transfer before "
	"changing ownership",
	"[imagegraph][evaluation_snapshot]"
) {
	EvaluationBudget first(4096), second(4096);
	auto source = first.Reserve(1024);
	auto destination = second.Reserve(16);
	REQUIRE(source);
	REQUIRE(destination);
	AudioBit audio{{1, 2, 3}, 8}, target;
	const auto *data = audio.Samples.data();
	const std::array moves{SnapshotAudioMove{&audio, &target, &*source, RetainedPayloadBytes(audio)}};
	CHECK_FALSE(PrepareSnapshotAudioMoves(moves, *destination));
	CHECK(source->Bytes() == 1024);
	CHECK(destination->Bytes() == 16);
	CHECK(first.Used() == 1024);
	CHECK(second.Used() == 16);
	CHECK(audio.Samples.data() == data);
	CHECK(target.Samples.empty());
}
TEST_CASE(
	"Snapshot audio preflight checks the combined charge and rejects "
	"alias plans",
	"[imagegraph][evaluation_snapshot]"
) {
	EvaluationBudget budget(4096);
	auto source = budget.Reserve(24), extraAdmission = budget.Reserve(1024), destination = budget.Reserve(16);
	REQUIRE(source);
	REQUIRE(extraAdmission);
	REQUIRE(destination);
	AudioBit a{{1, 2, 3}, 8}, b{{4, 5, 6}, 8}, targetA, targetB;
	const auto *aData = a.Samples.data(), *bData = b.Samples.data();
	std::array moves{
		SnapshotAudioMove{&a, &targetA, &*source, RetainedPayloadBytes(a)},
		SnapshotAudioMove{&b, &targetB, &*source, RetainedPayloadBytes(b)}
	};
	// Additional shadow admission keeps test payloads admitted; the proposed
	// owning-result charge is hostile.
	const auto used = budget.Used();
	CHECK_FALSE(PrepareSnapshotAudioMoves(moves, *destination));
	CHECK(budget.Used() == used);
	CHECK(source->Bytes() == 24);
	CHECK(destination->Bytes() == 16);
	CHECK(a.Samples.data() == aData);
	CHECK(b.Samples.data() == bData);
	REQUIRE(source->Resize(1024));
	moves[1].Source = &a;
	moves[1].Bytes = RetainedPayloadBytes(a);
	CHECK_FALSE(PrepareSnapshotAudioMoves(moves, *destination));
	moves[1] = {&b, &a, &*source, RetainedPayloadBytes(b)};
	CHECK_FALSE(PrepareSnapshotAudioMoves(moves, *destination));
	CHECK(a.Samples.data() == aData);
	CHECK(b.Samples.data() == bData);
}

TEST_CASE(
	"A later incompatible result charge cannot commit an earlier valid "
	"audio move",
	"[imagegraph][evaluation_snapshot]"
) {
	EvaluationBudget first(4096), second(4096);
	auto sourceA = first.Reserve(1024), sourceB = second.Reserve(1024), destination = first.Reserve(16);
	REQUIRE(sourceA);
	REQUIRE(sourceB);
	REQUIRE(destination);
	AudioBit a{{1, 2, 3}, 8}, b{{4, 5, 6}, 8}, targetA, targetB;
	const auto *aData = a.Samples.data(), *bData = b.Samples.data();
	const std::array moves{
		SnapshotAudioMove{&a, &targetA, &*sourceA, RetainedPayloadBytes(a)},
		SnapshotAudioMove{&b, &targetB, &*sourceB, RetainedPayloadBytes(b)}
	};
	CHECK_FALSE(PrepareSnapshotAudioMoves(moves, *destination));
	CHECK(sourceA->Bytes() == 1024);
	CHECK(sourceB->Bytes() == 1024);
	CHECK(destination->Bytes() == 16);
	CHECK(a.Samples.data() == aData);
	CHECK(b.Samples.data() == bData);
	CHECK(targetA.Samples.empty());
	CHECK(targetB.Samples.empty());
}
