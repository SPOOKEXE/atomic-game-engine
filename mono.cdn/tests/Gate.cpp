#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cdn/Gate.hpp>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

TEST_SUITE_ID("cdn.gate")
TEST_DEPENDS("engine.assets.grant")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::assets::ContentHash;
using engine::assets::Grant;
using engine::assets::GrantKey;
using engine::assets::GrantScope;
using engine::assets::Hasher;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	constexpr uint64_t NOW = 1'000'000;
	constexpr uint64_t EXPIRY = NOW + 300;

	GrantKey Key(uint8_t fill = 1) {
		std::array<std::byte, GrantKey::BYTES> secret{};
		for (size_t index = 0; index < secret.size(); ++index) {
			secret[index] = static_cast<std::byte>(fill + index);
		}
		auto key = GrantKey::FromSecret(secret);
		REQUIRE(key.has_value());
		return std::move(*key);
	}

	ContentHash Bundle(std::string_view text) {
		return Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size())
		);
	}

	std::vector<std::byte> Token(const GrantKey &key) {
		GrantScope scope;
		scope.Session = 7;
		scope.Bundles = {Bundle("terrain"), Bundle("characters")};
		scope.ExpiresAtSeconds = EXPIRY;
		scope.ByteBudget = 1024 * 1024;

		const auto grant = Grant::Issue(scope, key);
		REQUIRE(grant.has_value());
		return grant->Encode();
	}
}

TEST_CASE("the gate admits a bundle the grant names", "[cdn][gate]") {
	const cdn::Gate gate{Key()};
	const auto token = Token(Key());

	CHECK(gate.Admits(token, Bundle("terrain"), NOW));
	CHECK(gate.Admits(token, Bundle("characters"), NOW));
}

TEST_CASE("the gate refuses a bundle outside the grant", "[cdn][gate]") {
	const cdn::Gate gate{Key()};
	const auto token = Token(Key());

	// A valid grant asking for content it was not given. The origin has no way
	// to widen it and no reason to want one — CDN.md §4.
	CHECK_FALSE(gate.Admits(token, Bundle("someone else's content"), NOW));
	CHECK_FALSE(gate.Admits(token, ContentHash{}, NOW));
}

TEST_CASE("the gate refuses a token from another key", "[cdn][gate]") {
	const cdn::Gate gate{Key(1)};
	const auto foreign = Token(Key(200));

	CHECK_FALSE(gate.Admits(foreign, Bundle("terrain"), NOW));
}

TEST_CASE("the gate refuses an expired token", "[cdn][gate]") {
	const cdn::Gate gate{Key()};
	const auto token = Token(Key());

	CHECK(gate.Admits(token, Bundle("terrain"), EXPIRY - 1));
	CHECK_FALSE(gate.Admits(token, Bundle("terrain"), EXPIRY));
	CHECK_FALSE(gate.Admits(token, Bundle("terrain"), EXPIRY + 100'000));
}

TEST_CASE("the gate refuses a tampered or empty token", "[cdn][gate]") {
	const cdn::Gate gate{Key()};
	const auto token = Token(Key());

	CHECK_FALSE(gate.Admits({}, Bundle("terrain"), NOW));

	auto edited = token;
	edited[edited.size() / 2] =
		static_cast<std::byte>(static_cast<uint8_t>(edited[edited.size() / 2]) ^ 0x01);
	CHECK_FALSE(gate.Admits(edited, Bundle("terrain"), NOW));
}

TEST_CASE("the gate reports itself to the frame graph and the metrics sink", "[cdn][gate][framegraph]") {
	const cdn::Gate gate{Key()};
	const auto token = Token(Key());
	const auto foreign = Token(Key(200));

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	CHECK(gate.Admits(token, Bundle("terrain"), NOW));
	CHECK_FALSE(gate.Admits(token, Bundle("elsewhere"), NOW));
	CHECK_FALSE(gate.Admits(foreign, Bundle("terrain"), NOW));
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
		return span.Name == "Gate::Admits";
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

	// Three outcomes, three counters. A bad token is an alarm; a valid grant
	// reaching outside its scope is a client bug or a probe; an admission is
	// neither. One counter for all three would bury the alarm.
	CHECK(total("cdn.gate.admitted") == 1.0);
	CHECK(total("cdn.gate.outofscope") == 1.0);
	CHECK(total("cdn.gate.refused") == 1.0);
}
