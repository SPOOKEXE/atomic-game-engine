#include <engine/imagegraph/SourceAnimatorCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.source_animator_capture")
using namespace engine::imagegraph;

namespace {
	Keyframe Raw(Value value, uint64_t tick = 0) {
		Keyframe key;
		key.NodeId = "source";
		key.Port = "value";
		key.Tick = tick;
		key.Data = std::move(value);
		key.Interpolation = "source";
		key.SourceDriver = KeyframeLinearDriver{.5};
		return key;
	}
	SourceAnimatorCapture
	Captured(std::span<const Keyframe> keys, SourceAnimatorCaptureOptions options = {{3}}) {
		SourceAnimatorCapture result;
		Diagnostic diagnostic;
		const auto status = CaptureSourceDisabledAnimatorValue(keys, options, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
}

TEST_CASE(
	"Disabled lone source animator captures raw numeric driver before getters",
	"[imagegraph][source_animator_capture]"
) {
	// Pinned node_value.setAnim disables first, then calls animator.getValue directly.
	// node_keyframe.getValue's lone-key driver branch precedes the disabled multi-key branch.
	for (Value value : {Value{int64_t{5}}, Value{5.}, Value{EnumValue{5}}}) {
		const std::array keys{Raw(value)};
		CHECK(std::get<double>(Captured(keys).Data) == 6.5);
		CHECK(keys.front().Data == value);
	}
	const std::array colour{Raw(Colour{5, 0, 0, 0})};
	CHECK(std::get<double>(Captured(colour).Data) == 6.5);
	const std::array boolean{Raw(true)};
	CHECK(std::get<double>(Captured(boolean).Data) == 2.5);
	const std::array future{Raw(int64_t{5}, 4)};
	CHECK(std::get<int64_t>(Captured(future).Data) == 5);
}

TEST_CASE(
	"Disabled source multi-key and trigger snapshots avoid interpolation and drivers",
	"[imagegraph][source_animator_capture]"
) {
	const std::array keys{Raw(int64_t{5}), Raw(int64_t{99}, 10)};
	CHECK(std::get<int64_t>(Captured(keys).Data) == 5);
	const std::array lone{Raw(true)};
	SourceAnimatorCaptureOptions trigger{{3}};
	trigger.Kind = SourceAnimatorCaptureKind::Trigger;
	CHECK_FALSE(std::get<bool>(Captured(lone, trigger).Data));
	auto remap = Raw(5.);
	remap.SourceDriver = KeyframeBounceDriver{};
	const std::array remapped{remap};
	CHECK(std::get<double>(Captured(remapped).Data) == 5.);
}

TEST_CASE(
	"Source animator numeric arrays share caller scratch and resident headroom atomically",
	"[imagegraph][source_animator_capture]"
) {
	ArrayValue integers{ValueType::Integer, {int64_t{5}, int64_t{9}}};
	const std::array keys{Raw(integers)};
	SourceAnimatorCaptureOptions options{{3}};
	options.AvailableOwnedBytes = 4 * sizeof(ElementValue);
	const auto result = Captured(keys, options);
	const auto &values = std::get<ArrayValue>(result.Data);
	REQUIRE(values.ElementType == ValueType::Scalar);
	REQUIRE(values.Elements.size() == 2);
	CHECK(std::get<double>(values.Elements[0]) == 6.5);
	CHECK(std::get<double>(values.Elements[1]) == 10.5);
	CHECK(result.ResidentOwnedBytes == 2 * sizeof(ElementValue));
	CHECK(result.PeakOwnedBytes == 4 * sizeof(ElementValue));
	SourceAnimatorCapture sentinel{42., 11, 12}, output = sentinel;
	Diagnostic diagnostic;
	--options.AvailableOwnedBytes;
	CHECK(CaptureSourceDisabledAnimatorValue(keys, options, output, diagnostic) == Status::LimitExceeded);
	CHECK(output == sentinel);
	CHECK(diagnostic.Port == "value");
	// A caller retaining the first capture subtracts its residency before another invocation.
	options.AvailableOwnedBytes = result.PeakOwnedBytes + result.ResidentOwnedBytes;
	options.AvailableOwnedBytes -= result.ResidentOwnedBytes;
	CHECK(CaptureSourceDisabledAnimatorValue(keys, options, output, diagnostic) == Status::Ok);
	--options.AvailableOwnedBytes;
	output = sentinel;
	CHECK(CaptureSourceDisabledAnimatorValue(keys, options, output, diagnostic) == Status::LimitExceeded);
	CHECK(output == sentinel);
}

TEST_CASE(
	"Source animator rejects invalid ordered keys without changing caller output",
	"[imagegraph][source_animator_capture]"
) {
	std::array keys{Raw(5.), Raw(9.)};
	SourceAnimatorCapture sentinel{42., 11, 12}, output = sentinel;
	Diagnostic diagnostic;
	CHECK(CaptureSourceDisabledAnimatorValue(keys, {{3}}, output, diagnostic) == Status::InvalidValue);
	CHECK(output == sentinel);
	keys[1].Tick = 10;
	keys[1].Port = "other";
	CHECK(CaptureSourceDisabledAnimatorValue(keys, {{3}}, output, diagnostic) == Status::InvalidValue);
	CHECK(output == sentinel);
}
