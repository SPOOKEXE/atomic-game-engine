#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.source_argument_colour")
using namespace engine::imagegraph;
namespace {
	Document Graph(Colour colour, bool linked = false, int64_t mode = 1) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"argument",
			 "pc.argument",
			 "",
			 {},
			 {{"tag", std::string{"colour"}},
			  {"type", EnumValue{mode}},
			  {"default_value", linked ? Value{0.} : Value{colour}}}}
		};
		if (linked) {
			document.Nodes.push_back({"colour", "pc.color", "", {}, {{"color", colour}}});
			document.Links.push_back({"colour", "color", "argument", "default_value"});
		}
		document.Outputs = {{"out", "argument", "value"}};
		return document;
	}
	SourceArgumentHost Provider(std::vector<AuthoredValue> arguments) {
		SourceArgumentHost host;
		Diagnostic diagnostic;
		REQUIRE(host.Prepare(arguments, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		return host;
	}
	Value Evaluated(const Document &document, SourceArgumentHost &host) {
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		Plan plan;
		auto status = Compile(restored, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		request.HostProvider = &host;
		EvaluatedValue output;
		status = EvaluateValue(restored, plan, "out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output.Data;
	}
	// Source cola and make_color_rgba return Long values with ascending RGBA bytes.
	constexpr std::array<std::pair<Colour, int64_t>, 6> FIXTURES{
		{{{0, 0, 0, 0}, INT64_C(0)},
		 {{17, 34, 51, 0}, INT64_C(0x00332211)},
		 {{17, 34, 51, 127}, INT64_C(0x7f332211)},
		 {{17, 34, 51, 128}, INT64_C(0x80332211)},
		 {{17, 34, 51, 255}, INT64_C(0xff332211)},
		 {{255, 255, 255, 255}, INT64_C(4294967295)}}
	};
}

TEST_CASE(
	"Argument Number packs linked Color RGBA after native reload", "[source_argument][argument_colour]"
) {
	auto host = Provider({});
	for (const auto &[colour, packed] : FIXTURES) {
		CAPTURE(packed);
		CHECK(Evaluated(Graph(colour, true), host) == Value{packed});
		CHECK(Evaluated(Graph(colour), host) == Value{packed});
	}
}

TEST_CASE(
	"Argument Number packs observed Color while String mode preserves its carrier",
	"[source_argument][argument_colour]"
) {
	for (const auto &[colour, packed] : FIXTURES) {
		CAPTURE(packed);
		auto host = Provider({{"colour", colour}});
		CHECK(Evaluated(Graph(Colour{1, 2, 3, 4}), host) == Value{packed});
		CHECK(Evaluated(Graph(Colour{1, 2, 3, 4}, false, 0), host) == Value{colour});
	}
}
