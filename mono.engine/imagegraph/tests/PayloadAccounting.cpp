#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.payload_accounting")
using namespace engine::imagegraph;
TEST_CASE(
	"key copy footprint includes fixed storage names easing and source curve allocation",
	"[imagegraph][payload_accounting]"
) {
	Keyframe key{"node", "value", 0, 1.0, "source"};
	const uint64_t base = sizeof(Keyframe) + 4 + 5 + 6;
	REQUIRE(KeyframePayloadBytes(key));
	CHECK(*KeyframePayloadBytes(key) == base);
	key.Ease = KeyframeEase{"bezier", "cut"};
	CHECK(*KeyframePayloadBytes(key) == base + 6 + 3);
	key.SourceDriver = KeyframeCurveDriver{};
	CHECK(*KeyframePayloadBytes(key) == base + 6 + 3 + 2 * sizeof(std::array<double, 6>));
	key.SourceDriver = KeyframeLinearDriver{};
	CHECK(*KeyframePayloadBytes(key) == base + 6 + 3);
	key.Data = ArrayValue{ValueType::Text, {std::string{"left"}, std::string{"right"}}};
	CHECK(*KeyframePayloadBytes(key) == base + 6 + 3 + 2 * sizeof(ElementValue) + 4 + 5);
}
TEST_CASE(
	"key footprint rejects unbounded data or driver before a host clone", "[imagegraph][payload_accounting]"
) {
	Keyframe key{"node", "value", 0, 1.0, "step"};
	const Keyframe original = key;
	key.Data = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(KeyframePayloadBytes(key));
	key = original;
	KeyframeCurveDriver curve;
	curve.Data.Anchors.resize(Limits::MaximumCurveAnchors + 1);
	key.SourceDriver = std::move(curve);
	CHECK_FALSE(KeyframePayloadBytes(key));
	key = original;
	key.Data = std::string(Limits::MaximumTextBytes + 1, 'x');
	CHECK_FALSE(KeyframePayloadBytes(key));
	CHECK(original.Data == Value{1.0});
}

TEST_CASE(
	"same native key domain preserves metadata and compiles after retargeting", "[imagegraph][key_transfer]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.number_simple", "", {}, {{"value", 5.0}}},
		{"target", "pc.math", "", {}, {{"a", 0.0}, {"b", 1.0}}}
	};
	document.Outputs = {{"answer", "target", "result"}};
	Keyframe original{"source", "value", 2, 5.0, "source"};
	original.Ease = KeyframeEase{};
	original.SourceDriver = KeyframeLinearDriver{2};
	original.Subframe = .25;
	original.NegativeFrame = true;
	original.Kind = KeyframeKind::Adder;
	Keyframe transferred;
	Diagnostic diagnostic;
	REQUIRE(
		PrepareKeyframeCloneForProperty(document, original, "target", "a", transferred, diagnostic) ==
		Status::Ok
	);
	Keyframe expected = original;
	expected.NodeId = "target";
	expected.Port = "a";
	CHECK(transferred == expected);
	CHECK(original.NodeId == "source");
	document.Keyframes = {transferred};
	document.Tracks = {{"target", "a"}};
	Plan plan;
	const Status compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}

TEST_CASE(
	"unrepresented key crossings refuse atomically without changing raw data", "[imagegraph][key_transfer]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "pc.number_simple", "", {}, {{"value", .75}}},
		{"target", "image.solid", "", {}, {{"width", int64_t{1}}, {"height", int64_t{1}}}}
	};
	const Keyframe original{"source", "value", 0, .75, "step"};
	Keyframe transferred{"retained", "property", 7, std::string{"unchanged"}, "step"};
	const Keyframe retained = transferred;
	Diagnostic diagnostic;
	CHECK(
		PrepareKeyframeCloneForProperty(document, original, "target", "width", transferred, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(transferred == retained);
	CHECK(original.Data == Value{.75});
	CHECK(diagnostic.NodeId == "target");
	CHECK(diagnostic.Port == "width");
	CHECK(
		PrepareKeyframeCloneForProperty(document, original, "target", "absent", transferred, diagnostic) ==
		Status::UnknownPort
	);
	CHECK(transferred == retained);
}

TEST_CASE("key transfer uses source declaration and display domains", "[imagegraph][key_transfer]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"number", "pc.number_simple", "", {}, {{"value", 1.0}}},
		{"colour", "pc.color", "", {}, {{"color", Colour{}}}},
		{"text", "pc.text", "", {}, {{"text", std::string{"value"}}}},
		{"flat", "pc.array_composite", "", {}, {{"array", ArrayValue{ValueType::Scalar, {1.0}}}}},
		{"range", "pc.3_d_particle", "", {}, {{"scale", ArrayValue{ValueType::Scalar, {1.0}}}}}
	};
	Keyframe retained{"old", "port", 0, 2.0, "step"};
	const Keyframe original = retained;
	Diagnostic diagnostic;
	const Keyframe number{"number", "value", 0, 1.0, "source"};
	CHECK(
		PrepareKeyframeCloneForProperty(document, number, "colour", "color", retained, diagnostic) ==
		Status::TypeMismatch
	);
	CHECK(retained == original);
	CHECK(
		PrepareKeyframeCloneForProperty(document, number, "text", "text", retained, diagnostic) ==
		Status::TypeMismatch
	);
	CHECK(retained == original);
	const Keyframe array{"flat", "array", 0, ArrayValue{ValueType::Scalar, {1.0}}, "source"};
	// Identical native type and payload shape cannot substitute the source widget classification.
	CHECK(
		PrepareKeyframeCloneForProperty(document, array, "range", "scale", retained, diagnostic) ==
		Status::TypeMismatch
	);
	CHECK(retained == original);
}

TEST_CASE(
	"source key provenance is charged and property cloning creates a fresh identity",
	"[imagegraph][key_provenance]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"source", "value.number", "", {}, {{"value", 1.0}}},
		{"target", "value.number", "", {}, {{"value", 2.0}}}
	};
	Keyframe key{"source", "value", 2, 3.0, "linear"};
	const auto originalBytes = KeyframePayloadBytes(key);
	REQUIRE(originalBytes);
	key.SourceKeyId.assign(Limits::MaximumSourceKeyIdBytes, 'x');
	REQUIRE(KeyframePayloadBytes(key));
	CHECK(*KeyframePayloadBytes(key) == *originalBytes + Limits::MaximumSourceKeyIdBytes);
	Keyframe copied;
	Diagnostic error;
	REQUIRE(PrepareKeyframeCloneForProperty(document, key, "target", "value", copied, error) == Status::Ok);
	CHECK(copied.SourceKeyId.empty());
	CHECK(key.SourceKeyId.size() == Limits::MaximumSourceKeyIdBytes);
	CHECK(copied.Data == key.Data);
	key.SourceKeyId.push_back('x');
	CHECK_FALSE(KeyframePayloadBytes(key));
	const auto retained = copied;
	CHECK(PrepareKeyframeCloneForProperty(document, key, "target", "value", copied, error) != Status::Ok);
	CHECK(copied == retained);
}
