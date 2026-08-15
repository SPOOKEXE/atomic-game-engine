#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cdn/Grouper.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("cdn.grouper")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using cdn::Assembly;
using cdn::Group;
using cdn::GroupCandidate;
using cdn::Grouper;
using cdn::GroupPolicy;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	ContentHash Asset(std::string_view name) {
		return Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(name.data()), name.size())
		);
	}

	GroupCandidate
	Candidate(std::string_view name, uint64_t bytes, uint32_t affinity = 0, uint32_t priority = 0) {
		GroupCandidate candidate;
		candidate.Root = Asset(name);
		candidate.Bytes = bytes;
		candidate.Affinity = affinity;
		candidate.Priority = priority;
		return candidate;
	}

	// Small bounds, so a test makes several groups out of a few kilobytes.
	GroupPolicy Small() {
		return GroupPolicy{1000, 2000};
	}

	size_t CountAssets(const Assembly &assembly) {
		size_t total = 0;
		for (const Group &group : assembly.Groups) {
			total += group.Assets.size();
		}
		return total;
	}

	bool Contains(const Group &group, const ContentHash &asset) {
		return std::find(group.Assets.begin(), group.Assets.end(), asset) != group.Assets.end();
	}

	const Group *GroupHolding(const Assembly &assembly, const ContentHash &asset) {
		for (const Group &group : assembly.Groups) {
			if (Contains(group, asset)) {
				return &group;
			}
		}
		return nullptr;
	}
}

TEST_CASE("every asset lands in exactly one group", "[cdn][grouper]") {
	const Grouper grouper(Small());
	const std::vector<GroupCandidate> candidates{
		Candidate("a", 300),
		Candidate("b", 400),
		Candidate("c", 500),
		Candidate("d", 600),
		Candidate("e", 700),
	};

	const Assembly assembly = grouper.Assemble(candidates);

	REQUIRE_FALSE(assembly.Groups.empty());
	CHECK(CountAssets(assembly) == candidates.size());

	for (const GroupCandidate &candidate : candidates) {
		INFO("asset missing from every group");
		CHECK(GroupHolding(assembly, candidate.Root) != nullptr);
	}
}

TEST_CASE("an affinity is never split across groups", "[cdn][grouper]") {
	const Grouper grouper(Small());

	// A mesh, its textures, its material and its collider. Rule 1: a group that
	// lands has to make something appear, and half of these makes nothing.
	const std::vector<GroupCandidate> candidates{
		Candidate("rock.mesh", 400, 1),
		Candidate("rock.albedo", 400, 1),
		Candidate("rock.normal", 400, 1),
		Candidate("rock.material", 100, 1),
		Candidate("filler-a", 500),
		Candidate("filler-b", 500),
		Candidate("filler-c", 500),
	};

	const Assembly assembly = grouper.Assemble(candidates);

	const Group *home = GroupHolding(assembly, Asset("rock.mesh"));
	REQUIRE(home != nullptr);
	CHECK(Contains(*home, Asset("rock.albedo")));
	CHECK(Contains(*home, Asset("rock.normal")));
	CHECK(Contains(*home, Asset("rock.material")));
}

TEST_CASE("affinity zero does not bind unrelated assets", "[cdn][grouper]") {
	const Grouper grouper(GroupPolicy{500, 1000});

	// Zero means "belongs with nothing in particular". Treating it as an
	// affinity like any other would bind every unrelated asset in the game into
	// one lump, which is the opposite of what the value means.
	const std::vector<GroupCandidate> candidates{
		Candidate("a", 400),
		Candidate("b", 400),
		Candidate("c", 400),
		Candidate("d", 400),
	};

	const Assembly assembly = grouper.Assemble(candidates);
	CHECK(assembly.Groups.size() > 1);
}

TEST_CASE("groups come out in priority order", "[cdn][grouper]") {
	const Grouper grouper(Small());
	const std::vector<GroupCandidate> candidates{
		Candidate("late", 900, 0, 9),
		Candidate("early", 900, 0, 1),
		Candidate("middle", 900, 0, 5),
	};

	const Assembly assembly = grouper.Assemble(candidates);
	REQUIRE(assembly.Groups.size() >= 2);

	CHECK(
		std::is_sorted(
			assembly.Groups.begin(), assembly.Groups.end(), [](const Group &left, const Group &right) {
				return left.Priority < right.Priority;
			}
		)
	);
	CHECK(Contains(assembly.Groups.front(), Asset("early")));
}

TEST_CASE("a group takes its most urgent member's priority", "[cdn][grouper]") {
	const Grouper grouper(GroupPolicy{10'000, 20'000});

	// A group is wanted as soon as its most urgent member is, so an affinity
	// holding one urgent asset is an urgent group.
	const std::vector<GroupCandidate> candidates{
		Candidate("patient", 100, 1, 8),
		Candidate("urgent", 100, 1, 2),
	};

	const Assembly assembly = grouper.Assemble(candidates);
	REQUIRE(assembly.Groups.size() == 1);
	CHECK(assembly.Groups[0].Priority == 2);
}

TEST_CASE("groups respect the target where an affinity allows", "[cdn][grouper]") {
	const GroupPolicy policy = Small();
	const Grouper grouper(policy);

	std::vector<GroupCandidate> candidates;
	for (int index = 0; index < 20; ++index) {
		candidates.push_back(Candidate("asset-" + std::to_string(index), 300));
	}

	const Assembly assembly = grouper.Assemble(candidates);

	REQUIRE(assembly.Groups.size() > 1);
	CHECK(assembly.Oversized == 0);
	for (const Group &group : assembly.Groups) {
		CHECK(group.TotalBytes <= policy.MaximumBytes);
	}
}

TEST_CASE("a group mixes large assets with small ones", "[cdn][grouper]") {
	const Grouper grouper(GroupPolicy{1000, 2000});

	// One large asset and many small ones. Packing largest-first leaves the
	// small ones to fill what the large one did not use - the mix rule 2 asks
	// for. Packing smallest-first would fill whole groups with tiny assets and
	// strand the large one alone.
	std::vector<GroupCandidate> candidates{Candidate("big", 700)};
	for (int index = 0; index < 6; ++index) {
		candidates.push_back(Candidate("small-" + std::to_string(index), 100));
	}

	const Assembly assembly = grouper.Assemble(candidates);

	const Group *home = GroupHolding(assembly, Asset("big"));
	REQUIRE(home != nullptr);
	CHECK(home->Assets.size() > 1);
}

TEST_CASE("an affinity heavier than the ceiling becomes one oversized group", "[cdn][grouper]") {
	const GroupPolicy policy{1000, 2000};
	const Grouper grouper(policy);

	// Rule 1 outranks the bound. Splitting this would produce two groups
	// neither of which makes anything appear, which is the one outcome the
	// class exists to avoid - so it is allowed and *counted*.
	const std::vector<GroupCandidate> candidates{
		Candidate("huge-a", 1500, 1),
		Candidate("huge-b", 1500, 1),
		Candidate("ordinary", 200),
	};

	const Assembly assembly = grouper.Assemble(candidates);

	CHECK(assembly.Oversized == 1);

	const Group *home = GroupHolding(assembly, Asset("huge-a"));
	REQUIRE(home != nullptr);
	CHECK(Contains(*home, Asset("huge-b")));
	CHECK(home->TotalBytes == 3000);

	// And it did not swallow the unrelated asset on its way past.
	CHECK_FALSE(Contains(*home, Asset("ordinary")));
	CHECK(GroupHolding(assembly, Asset("ordinary")) != nullptr);
}

TEST_CASE("grouping is deterministic", "[cdn][grouper]") {
	const Grouper grouper(Small());

	std::vector<GroupCandidate> forward{
		Candidate("a", 300, 1),
		Candidate("b", 300, 1),
		Candidate("c", 400),
		Candidate("d", 500),
		Candidate("e", 200),
	};
	std::vector<GroupCandidate> backward(forward.rbegin(), forward.rend());

	const Assembly first = grouper.Assemble(forward);
	const Assembly second = grouper.Assemble(backward);

	// Two origins that group the same content differently prepare and cache
	// different bundles for it, and nothing anywhere reports that they have
	// stopped sharing.
	REQUIRE(first.Groups.size() == second.Groups.size());
	for (size_t index = 0; index < first.Groups.size(); ++index) {
		INFO("group " << index);
		CHECK(first.Groups[index].Assets == second.Groups[index].Assets);
		CHECK(first.Groups[index].TotalBytes == second.Groups[index].TotalBytes);
	}
}

TEST_CASE("a group's assets are sorted", "[cdn][grouper]") {
	const Grouper grouper(GroupPolicy{10'000, 20'000});
	const std::vector<GroupCandidate> candidates{
		Candidate("z", 100, 1),
		Candidate("a", 100, 1),
		Candidate("m", 100, 1),
	};

	const Assembly assembly = grouper.Assemble(candidates);
	REQUIRE(assembly.Groups.size() == 1);

	// The arrangement Manifest::AddBundle will impose anyway, done here so the
	// two cannot disagree about what bundle a group is.
	CHECK(std::is_sorted(assembly.Groups[0].Assets.begin(), assembly.Groups[0].Assets.end()));
}

TEST_CASE("no candidates produce no groups", "[cdn][grouper]") {
	const Grouper grouper(Small());

	// Not one empty group, for the reason an empty chunk is not a chunk.
	CHECK(grouper.Assemble({}).Groups.empty());
}

TEST_CASE("invalid policy falls back to the defaults", "[cdn][grouper]") {
	const Grouper inverted(GroupPolicy{2000, 1000});
	CHECK(inverted.Policy().TargetBytes == GroupPolicy{}.TargetBytes);

	const Grouper zeroed(GroupPolicy{0, 0});
	CHECK(zeroed.Policy().TargetBytes == GroupPolicy{}.TargetBytes);

	CHECK(GroupPolicy{}.IsValid());
	CHECK_FALSE(GroupPolicy{2000, 1000}.IsValid());
}

TEST_CASE("assembling reports itself to the frame graph and the metrics sink", "[cdn][grouper][framegraph]") {
	const Grouper grouper(GroupPolicy{1000, 2000});
	const std::vector<GroupCandidate> candidates{
		Candidate("a", 900),
		Candidate("b", 900),
		Candidate("huge", 5000, 1),
	};

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	const Assembly assembly = grouper.Assemble(candidates);
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
		return span.Name == "Grouper::Assemble";
	}));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	CHECK(total("cdn.groups.assembled") == static_cast<double>(assembly.Groups.size()));

	// A bound that is quietly broken reads as a bound that held, and the first
	// anyone hears of it is a client stalling on a group it cannot stream.
	CHECK(total("cdn.groups.oversized") == 1.0);
}
