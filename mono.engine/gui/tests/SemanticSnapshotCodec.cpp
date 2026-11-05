#include <engine/gui/SemanticSnapshotCodec.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.gui.semanticsnapshotcodec")

using namespace engine::gui;

TEST_CASE("semantic snapshots retain their displayed values", "[gui][accessibility]") {
	SemanticSnapshot original;
	auto &node = original.Nodes.emplace_back();
	node.Instance.Id = 42;
	node.Name = "Name €";
	node.Value = "Value";
	node.Description = "Description";
	node.Language = "en";
	node.Bounds = {{1, 2}, {10, 20}};
	node.Selected = true;
	node.Editable = true;
	std::vector<std::byte> bytes;
	REQUIRE(EncodeSemanticSnapshot(original, 4096, bytes));
	SemanticSnapshot decoded;
	REQUIRE(DecodeSemanticSnapshot(bytes, 4096, decoded));
	REQUIRE(decoded.Nodes.size() == 1);
	CHECK(decoded.Nodes[0].Instance == node.Instance);
	CHECK(decoded.Nodes[0].Name == node.Name);
	CHECK(decoded.Nodes[0].Value == node.Value);
	CHECK(decoded.Nodes[0].Description == node.Description);
	CHECK(decoded.Nodes[0].Language == node.Language);
	CHECK(decoded.Nodes[0].Bounds.Min == node.Bounds.Min);
	CHECK(decoded.Nodes[0].Bounds.Max == node.Bounds.Max);
	CHECK(decoded.Nodes[0].Selected);
	CHECK(decoded.Nodes[0].Editable);
	CHECK_FALSE(decoded.Truncated);
}

TEST_CASE("semantic snapshot limits retain a valid prefix", "[gui][accessibility]") {
	SemanticSnapshot original;
	original.Nodes.emplace_back().Name = "first";
	original.Nodes.emplace_back().Name = "second";
	SemanticSnapshot first;
	first.Nodes.push_back(original.Nodes.front());
	std::vector<std::byte> bytes;
	REQUIRE(EncodeSemanticSnapshot(first, 4096, bytes));
	REQUIRE(EncodeSemanticSnapshot(original, bytes.size(), bytes));
	SemanticSnapshot decoded;
	REQUIRE(DecodeSemanticSnapshot(bytes, 4096, decoded));
	REQUIRE(decoded.Nodes.size() == 1);
	CHECK(decoded.Nodes.front().Name == "first");
	CHECK(decoded.Truncated);
}

TEST_CASE("invalid semantic snapshots leave output unchanged", "[gui][accessibility]") {
	SemanticSnapshot original;
	original.Nodes.emplace_back().Name = "saved";
	std::vector<std::byte> bytes;
	REQUIRE(EncodeSemanticSnapshot(original, 4096, bytes));
	SemanticSnapshot decoded = original;
	CHECK_FALSE(DecodeSemanticSnapshot(std::span(bytes).first(bytes.size() - 1), 4096, decoded));
	REQUIRE(decoded.Nodes.size() == 1);
	CHECK(decoded.Nodes.front().Name == "saved");
	CHECK_FALSE(DecodeSemanticSnapshot(bytes, 1, decoded));
	CHECK_FALSE(DecodeSemanticSnapshot(bytes, sizeof(SemanticNode) + 128, decoded));
	original.Nodes.front().Bounds.Min.X = std::numeric_limits<float>::infinity();
	const auto previous = bytes;
	CHECK_FALSE(EncodeSemanticSnapshot(original, 4096, bytes));
	CHECK(bytes == previous);
}
