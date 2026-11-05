#include "../src/PresentationFrameMetadata.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("client.presentation_frame_metadata")

TEST_CASE("frame metadata owns its displayed semantic tree and identities", "[client][gui][input]") {
	engine::gui::SemanticSnapshot source;
	engine::gui::SemanticNode node;
	node.Instance = engine::ecs::Entity(7);
	node.Collector = engine::ecs::Entity(8);
	node.Name = "before";
	source.Nodes.push_back(node);
	auto encoded = client::EncodePresentationFrameMetadata(11, 12, &source, 4096);
	REQUIRE(encoded);
	source.Nodes[0].Name = "after";
	auto decoded = client::DecodePresentationFrameMetadata(*encoded, 4096);
	REQUIRE(decoded);
	CHECK(decoded->Runtime == 11);
	CHECK(decoded->Store == 12);
	REQUIRE(decoded->Semantics);
	REQUIRE(decoded->Semantics->Nodes.size() == 1);
	CHECK(decoded->Semantics->Nodes[0].Name == "before");
	CHECK(decoded->Semantics->Nodes[0].Collector == engine::ecs::Entity(8));
}

TEST_CASE("frame metadata rejects incomplete or oversized envelopes", "[client][gui][input]") {
	auto encoded = client::EncodePresentationFrameMetadata(1, 2, nullptr, 4096);
	REQUIRE(encoded);
	for (size_t size = 0; size < encoded->size(); ++size)
		CHECK_FALSE(client::DecodePresentationFrameMetadata(std::span(*encoded).first(size), 4096));
	auto extra = *encoded;
	extra.push_back(std::byte{});
	CHECK_FALSE(client::DecodePresentationFrameMetadata(extra, 4096));
	auto invalid = *encoded;
	invalid[16] = std::byte{2};
	CHECK_FALSE(client::DecodePresentationFrameMetadata(invalid, 4096));
	CHECK_FALSE(client::EncodePresentationFrameMetadata(1, 2, nullptr, 16));
}
