#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.document")

using namespace engine::imagegraph;

namespace {
	Document Basic() {
		return {
			.Nodes = {{"solid", Solid{2, 2, {1, 2, 3, 255}}, {}, {11.5, -8.25}}},
			.Outputs = {{"main", "solid"}}
		};
	}
	void Replace(std::string &text, std::string_view before, std::string_view after) {
		const size_t at = text.find(before);
		REQUIRE(at != std::string::npos);
		text.replace(at, before.size(), after);
	}
}

TEST_CASE(
	"2d image graph project preserves named kinds controls links and layout", "[imagegraph][document]"
) {
	Document document{
		.Nodes =
			{
				{"source", Source{"textures/source.png"}, {}, {-10, 20}},
				{"solid", Solid{2, 2, {12, 34, 56, 78}}, {}, {}},
				{"resize", Resize{4, 4, Sampling::Bilinear}, {"source"}, {}},
				{"crop", Crop{-2, 3, 4, 4}, {"resize"}, {}},
				{"transform", Transform{4, 4, 1.5, -2.5, -1, 2, 90, 2, 2, Sampling::Nearest}, {"crop"}, {}},
				{"flip", Flip{true, false}, {"transform"}, {}},
				{"blend", Blend{0.75}, {"flip", "solid"}, {}},
			},
		.Outputs = {{"composed", "blend"}, {"original", "source"}},
	};
	Diagnostic diagnostic;
	std::string encoded;
	REQUIRE(Write(document, encoded, diagnostic));
	CHECK(encoded.find("image.transform") != std::string::npos);
	CHECK(encoded.find("bilinear") != std::string::npos);
	Document decoded;
	REQUIRE(Read(encoded, decoded, diagnostic));
	std::string rewritten;
	REQUIRE(Write(decoded, rewritten, diagnostic));
	CHECK(rewritten == encoded);
	CHECK(decoded.Nodes[0].Position == document.Nodes[0].Position);
	CHECK(decoded.Outputs[0].Node == "blend");
	CHECK(std::get<Transform>(decoded.Nodes[4].Value).ScaleX == -1);
	CHECK(Kind(decoded.Nodes[6].Value) == "image.blend");
	CHECK(SamplingName(static_cast<Sampling>(255)).empty());
}

TEST_CASE("compile resolves a shared DAG in stable authored order", "[imagegraph][document]") {
	Document document{
		.Nodes =
			{
				{"blend", Blend{}, {"left", "right"}, {}},
				{"left", Flip{true, false}, {"source"}, {}},
				{"right", Flip{false, true}, {"source"}, {}},
				{"source", Source{"source.png"}, {}, {}},
			},
		.Outputs = {{"main", "blend"}}
	};
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Compile(document, plan, diagnostic));
	CHECK(plan.Order == std::vector<size_t>{3, 1, 2, 0});
	CHECK(plan.Inputs[0] == std::vector<size_t>{1, 2});
	CHECK(plan.Outputs == std::vector<size_t>{0});
	Plan repeated;
	REQUIRE(Compile(document, repeated, diagnostic));
	CHECK(repeated == plan);
}

TEST_CASE("compile refuses malformed graphs without replacing an accepted plan", "[imagegraph][document]") {
	const Document accepted = Basic();
	Diagnostic diagnostic;
	Plan prior;
	REQUIRE(Compile(accepted, prior, diagnostic));
	const auto refuse = [&](const Document &document, std::string_view message) {
		Plan output = prior;
		CHECK_FALSE(Compile(document, output, diagnostic));
		CHECK(output == prior);
		CHECK(diagnostic.Message.find(message) != std::string::npos);
	};
	SECTION("empty graph") {
		refuse({}, "count");
	}
	SECTION("duplicate node") {
		auto document = accepted;
		document.Nodes.push_back(document.Nodes[0]);
		refuse(document, "duplicated");
	}
	SECTION("wrong arity") {
		auto document = accepted;
		document.Nodes[0].Inputs = {"solid"};
		refuse(document, "input count");
	}
	SECTION("dangling link") {
		auto document = accepted;
		document.Nodes.push_back({"flip", Flip{}, {"missing"}, {}});
		refuse(document, "missing node");
		CHECK(diagnostic.Node == "flip");
	}
	SECTION("cycle") {
		auto document = accepted;
		document.Nodes.push_back({"cycle", Flip{}, {"cycle"}, {}});
		refuse(document, "cycle");
		CHECK(diagnostic.Node == "cycle");
	}
	SECTION("missing output") {
		auto document = accepted;
		document.Outputs[0].Node = "gone";
		refuse(document, "missing node");
	}
	SECTION("duplicate output") {
		auto document = accepted;
		document.Outputs.push_back(document.Outputs[0]);
		refuse(document, "duplicated");
	}
	SECTION("excess nodes") {
		auto document = accepted;
		document.Nodes.resize(Limits::MaximumNodes + 1);
		refuse(document, "count");
	}
	SECTION("excess outputs") {
		auto document = accepted;
		document.Outputs.resize(Limits::MaximumOutputs + 1);
		refuse(document, "count");
	}
	SECTION("empty identity") {
		auto document = accepted;
		document.Nodes[0].Id.clear();
		refuse(document, "identity");
	}
	SECTION("invalid source path") {
		auto document = accepted;
		document.Nodes.push_back({"bad", Source{"../escape.png"}, {}, {}});
		refuse(document, "relative path");
	}
	SECTION("absolute source path") {
		auto document = accepted;
		document.Nodes.push_back({"bad", Source{"/escape.png"}, {}, {}});
		refuse(document, "relative path");
	}
	SECTION("zero dimension") {
		auto document = accepted;
		std::get<Solid>(document.Nodes[0].Value).Width = 0;
		refuse(document, "dimensions");
	}
	SECTION("oversized image") {
		auto document = accepted;
		std::get<Solid>(document.Nodes[0].Value).Width = Limits::MaximumDimension + 1;
		refuse(document, "dimensions");
	}
	SECTION("disconnected invalid controls") {
		auto document = accepted;
		document.Nodes.push_back({"bad", Transform{.ScaleX = 0}, {"solid"}, {}});
		refuse(document, "nonzero");
	}
	SECTION("nonfinite controls") {
		auto document = accepted;
		document.Nodes.push_back(
			{"bad", Blend{std::numeric_limits<double>::quiet_NaN()}, {"solid", "solid"}, {}}
		);
		refuse(document, "opacity");
	}
	SECTION("opacity range") {
		auto document = accepted;
		document.Nodes.push_back({"bad", Blend{1.01}, {"solid", "solid"}, {}});
		refuse(document, "opacity");
	}
	SECTION("nonfinite layout") {
		auto document = accepted;
		document.Nodes[0].Position[0] = std::numeric_limits<double>::infinity();
		refuse(document, "position");
	}
	SECTION("unknown filter") {
		auto document = accepted;
		document.Nodes.push_back({"bad", Resize{2, 2, static_cast<Sampling>(255)}, {"solid"}, {}});
		refuse(document, "filter");
	}
}

TEST_CASE(
	"project reader refuses unknown malformed and over-limit data atomically", "[imagegraph][document]"
) {
	Document accepted = Basic();
	Diagnostic diagnostic;
	std::string valid;
	REQUIRE(Write(accepted, valid, diagnostic));
	const auto refuse = [&](std::string_view encoded) {
		Document output = accepted;
		CHECK_FALSE(Read(encoded, output, diagnostic));
		std::string after;
		REQUIRE(Write(output, after, diagnostic));
		CHECK(after == valid);
	};
	SECTION("truncated") {
		refuse(std::string_view(valid).substr(0, valid.size() - 1));
	}
	SECTION("malformed") {
		refuse("{this is not json}");
	}
	SECTION("unknown kind") {
		auto encoded = valid;
		Replace(encoded, "image.solid", "image.audio");
		refuse(encoded);
	}
	SECTION("duplicate fields") {
		auto encoded = valid;
		Replace(encoded, "\"version\": 1", "\"version\": 1, \"version\": 1");
		refuse(encoded);
	}
	SECTION("unknown fields") {
		auto encoded = valid;
		Replace(encoded, "\"width\": 2", "\"unknown\": 2");
		refuse(encoded);
	}
	SECTION("unsupported version") {
		auto encoded = valid;
		Replace(encoded, "\"version\": 1", "\"version\": 2");
		refuse(encoded);
	}
	SECTION("negative dimensions") {
		auto encoded = valid;
		Replace(encoded, "\"width\": 2", "\"width\": -2");
		refuse(encoded);
	}
	SECTION("fractional integer") {
		auto encoded = valid;
		Replace(encoded, "\"width\": 2", "\"width\": 2.5");
		refuse(encoded);
	}
	SECTION("huge integer") {
		auto encoded = valid;
		Replace(encoded, "\"width\": 2", "\"width\": 18446744073709551615");
		refuse(encoded);
	}
	SECTION("colour outside byte range") {
		auto encoded = valid;
		Replace(encoded, "255", "256");
		refuse(encoded);
	}
	SECTION("bounded nesting") {
		refuse(
			std::string(Limits::MaximumJsonDepth + 1, '[') + "0" +
			std::string(Limits::MaximumJsonDepth + 1, ']')
		);
	}
	SECTION("bounded bytes") {
		refuse(std::string(Limits::MaximumDocumentBytes + 1, ' '));
	}
	SECTION("quoted brackets do not change depth") {
		auto document = accepted;
		document.Nodes[0].Id = "[[[{}]]]\"quoted";
		document.Outputs[0].Node = document.Nodes[0].Id;
		std::string encoded;
		REQUIRE(Write(document, encoded, diagnostic));
		Document read;
		CHECK(Read(encoded, read, diagnostic));
	}
}

TEST_CASE("project writer preserves prior bytes on refusal", "[imagegraph][document]") {
	Document document = Basic();
	document.Outputs[0].Node = "missing";
	std::string encoded = "accepted project";
	Diagnostic diagnostic;
	CHECK_FALSE(Write(document, encoded, diagnostic));
	CHECK(encoded == "accepted project");
}
