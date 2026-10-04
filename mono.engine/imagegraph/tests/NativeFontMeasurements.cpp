#include <engine/imagegraph/SourceFont.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.native_font_measurements")
using namespace engine::imagegraph;
namespace {
	constexpr uint64_t Bytes = Limits::MaximumEvaluationBytes;
	constexpr uint64_t Work = 16 * 1024 * 1024;
	FontValue MetricFont() {
		FontValue value;
		auto &font = value.Data.emplace();
		font.Raster = FontRasterProfile::NativeGlyphCoverage;
		font.Characters = FontCharacterProfile::UnicodeScalar;
		font.GlyphMapComplete = false;
		font.LineHeight = 10;
		font.SpaceAdvance = 4;
		font.Identity = "literal owned metrics";
		font.HasCharacterRange = true;
		font.FirstCharacter = 32;
		font.LastCharacter = 66;
		// Hand-owned measurement font. No decoded or licensed host provenance is claimed.
		font.Glyphs = {
			{9, false, {}, 0, 0, 10, {}, {}, 0},
			{32, true, {}, 4, 4, 10, {}, {}, 0},
			{65, true, {}, 8, 8, 10, {}, {}, 0},
			{66, false, {}, 0, 0, 10, {}, {}, 0}
		};
		return value;
	}
	std::vector<FontMeasurement> Prior() {
		std::vector<FontMeasurement> value{{"unchanged prior", 19, -1, 23, 29}};
		value.reserve(64);
		value[0].Text.reserve(8192);
		return value;
	}
	void Check(
		const FontMeasurement &result,
		std::string_view text,
		double wrap,
		double gap,
		double width,
		double height
	) {
		CHECK(result.Text == text);
		CHECK(result.MaximumLineWidth == wrap);
		CHECK(result.LineGap == gap);
		CHECK(result.Width == width);
		CHECK(result.Height == height);
	}
}
TEST_CASE("native measurement raw wrap slices have literal advances and independent newline rules") {
	const auto font = MetricFont();
	REQUIRE(SourceFontValueRetainedBytes(font));
	struct Sample {
		const char *Text;
		double Wrap, Width, Height;
	};
	// Independently hand-traced pinned Split_TextBlock, not a helper-produced golden.
	const std::array samples{
		Sample{"A A", 12, 8, 20},
		Sample{"AA AA", 12, 16, 20},
		Sample{"AA", 16, 16, 10},
		Sample{"A\tA", 12, 16, 10},
		Sample{" A ", 12, 12, 10},
		Sample{"    ", 4, 8, 10},
		Sample{"A\nA", 12, 8, 20},
		Sample{"A\rA", 12, 16, 10},
		Sample{"A\r\nA", 12, 8, 20},
		Sample{"\n\nA", 12, 8, 30},
		Sample{"A\n", 12, 8, 10},
		Sample{"", 12, 0, 0},
		Sample{"A", 7, 0, 0},
		Sample{"A", .5, 0, 0},
		Sample{"B A", 12, 12, 10}
	};
	for (const auto &sample : samples) {
		INFO(sample.Text);
		INFO(sample.Wrap);
		const std::array requests{FontMeasurement{sample.Text, sample.Wrap, -1, 0, 0}};
		auto result = Prior();
		std::string failure;
		const auto status = MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure);
		INFO(failure);
		REQUIRE(status == Status::Ok);
		REQUIRE(result.size() == 1);
		Check(result[0], sample.Text, sample.Wrap, -1, sample.Width, sample.Height);
		CHECK(requests[0].Width == 0);
		CHECK(requests[0].Height == 0);
	}
}
TEST_CASE(
	"signed widths preserve int32 boundaries fractional identity and exact positive sentinel behavior"
) {
	const auto font = MetricFont();
	struct Sample {
		double Wrap, Width, Height;
	};
	const std::array samples{
		Sample{-12.75, 8, 20},
		Sample{-.5, 0, 0},
		Sample{double(std::numeric_limits<int32_t>::min()), 8, 20},
		Sample{10000000, 8, 20},
		Sample{10000000.75, 8, 20},
		Sample{10000001, 16, 10},
		Sample{double(std::numeric_limits<int32_t>::max()), 16, 10}
	};
	for (const auto &sample : samples) {
		INFO(sample.Wrap);
		const std::array requests{FontMeasurement{"A\rA", sample.Wrap, -1, 0, 0}};
		std::vector<FontMeasurement> result;
		std::string failure;
		REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
		REQUIRE(result.size() == 1);
		Check(result[0], "A\rA", sample.Wrap, -1, sample.Width, sample.Height);
	}
	for (const char *raw : {"A\nA", "A\r\nA", "A\n\rA"}) {
		const std::array requests{
			FontMeasurement{raw, -1, -1, 0, 0}, FontMeasurement{raw, 10000000.75, -1, 0, 0}
		};
		std::vector<FontMeasurement> result;
		std::string failure;
		REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
		REQUIRE(result.size() == 2);
		Check(result[0], raw, -1, -1, 8, 20);
		Check(result[1], raw, 10000000.75, -1, 8, 20);
	}
	const std::array fractions{
		FontMeasurement{"AA AA", 12.25, -1, 0, 0}, FontMeasurement{"AA AA", 12.75, -1, 0, 0}
	};
	std::vector<FontMeasurement> results;
	std::string failure;
	REQUIRE(MeasureNativeSourceFont(font, fractions, Bytes, Work, results, failure) == Status::Ok);
	REQUIRE(results.size() == 2);
	Check(results[0], "AA AA", 12.25, -1, 16, 20);
	Check(results[1], "AA AA", 12.75, -1, 16, 20);
	for (double invalid :
		 {double(std::numeric_limits<int32_t>::min()) - 1, double(std::numeric_limits<int32_t>::max()) + 1}) {
		const std::array requests{FontMeasurement{"A", invalid, -1, 0, 0}};
		auto result = Prior();
		const auto prior = result;
		CHECK(
			MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) ==
			Status::UnsupportedExecution
		);
		CHECK(result == prior);
		CHECK_FALSE(failure.empty());
	}
	const std::array separated{FontMeasurement{"A\nA", 12, 2.75, 0, 0}};
	REQUIRE(MeasureNativeSourceFont(font, separated, Bytes, Work, results, failure) == Status::Ok);
	Check(results[0], "A\nA", 12, 2.75, 8, 12);
}
TEST_CASE(
	"exact observed measurement identity overrides native arithmetic and unavailable source record refuses "
	"atomically"
) {
	auto font = MetricFont();
	font.Data->Raster = FontRasterProfile::SourceObserved;
	font.Data->Measurements = {{"A A", 12.75, -1, 3, 7}, {"A", 2147483648.0, -1, 5, 6}};
	const std::array requests{
		FontMeasurement{"A A", 12.75, -1, 0, 0}, FontMeasurement{"A", 2147483648.0, -1, 0, 0}
	};
	auto result = Prior();
	std::string failure;
	REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
	REQUIRE(result.size() == 2);
	Check(result[0], "A A", 12.75, -1, 3, 7);
	Check(result[1], "A", 2147483648.0, -1, 5, 6);
	const auto prior = result;
	const std::array missing{FontMeasurement{"A A", 12.25, -1, 0, 0}};
	CHECK(
		MeasureNativeSourceFont(font, missing, Bytes, Work, result, failure) == Status::UnsupportedExecution
	);
	CHECK(result == prior);
	font.Data->Raster = FontRasterProfile::NativeGlyphCoverage;
	REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
	CHECK(result == prior);
	font.Data->Measurements.push_back(font.Data->Measurements[0]);
	CHECK(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::DuplicateId);
	CHECK(result == prior);
}
TEST_CASE(
	"malformed Unicode incomplete glyph evidence and duplicate requests preserve prior measurement ownership"
) {
	const auto font = MetricFont();
	std::string failure;
	const std::array malformed{
		FontMeasurement{std::string{"\xc0\xaf", 2}, 12, -1, 0, 0},
		FontMeasurement{std::string{"\xed\xa0\x80", 3}, 12, -1, 0, 0},
		FontMeasurement{std::string{"\xf0\x9f", 2}, 12, -1, 0, 0},
		FontMeasurement{"A", 12, -1, 1, 0},
		FontMeasurement{"A", std::numeric_limits<double>::infinity(), -1, 0, 0}
	};
	for (const auto &request : malformed) {
		auto result = Prior();
		const auto prior = result;
		const std::array requests{request};
		CHECK(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) != Status::Ok);
		CHECK(result == prior);
		CHECK_FALSE(failure.empty());
	}
	auto incomplete = font;
	incomplete.Data->Glyphs.erase(incomplete.Data->Glyphs.begin() + 2);
	const std::array requests{FontMeasurement{"A", 12, -1, 0, 0}};
	auto result = Prior();
	const auto prior = result;
	CHECK(
		MeasureNativeSourceFont(incomplete, requests, Bytes, Work, result, failure) ==
		Status::UnsupportedExecution
	);
	CHECK(result == prior);
	const std::array duplicate{requests[0], requests[0]};
	CHECK(MeasureNativeSourceFont(font, duplicate, Bytes, Work, result, failure) == Status::DuplicateId);
	CHECK(result == prior);
	REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
	REQUIRE(result.size() == 1);
	Check(result[0], "A", 12, -1, 8, 10);
	CHECK(prior[0].Text == "unchanged prior");
	auto aliased = font;
	aliased.Data->Measurements = {{"A", 12, -1, 8, 10}};
	const auto oldFont = aliased;
	const std::array replacement{FontMeasurement{"AA", 16, -1, 0, 0}};
	CHECK(
		MeasureNativeSourceFont(aliased, replacement, 1, Work, aliased.Data->Measurements, failure) ==
		Status::LimitExceeded
	);
	CHECK(aliased == oldFont);
	REQUIRE(
		MeasureNativeSourceFont(aliased, replacement, Bytes, Work, aliased.Data->Measurements, failure) ==
		Status::Ok
	);
	REQUIRE(aliased.Data->Measurements.size() == 1);
	Check(aliased.Data->Measurements[0], "AA", 16, -1, 16, 10);
	CHECK(oldFont.Data->Measurements[0].Text == "A");
}
TEST_CASE("measurement byte and aggregate work caps preserve old backing before successful retry") {
	const auto font = MetricFont();
	const std::array requests{FontMeasurement{"A A", 12.75, -1, 0, 0}};
	auto result = Prior();
	const auto prior = result;
	const auto priorCapacity = result.capacity();
	const auto priorTextCapacity = result[0].Text.capacity();
	std::string failure;
	// Font and tiny request fit4096; the simultaneously retained8KiB old string does not.
	REQUIRE(*SourceFontValueRetainedBytes(font) < 4096);
	CHECK(MeasureNativeSourceFont(font, requests, 4096, Work, result, failure) == Status::LimitExceeded);
	CHECK(result == prior);
	CHECK(result.capacity() == priorCapacity);
	CHECK(result[0].Text.capacity() == priorTextCapacity);
	CHECK(MeasureNativeSourceFont(font, requests, Bytes, 1, result, failure) == Status::LimitExceeded);
	CHECK(result == prior);
	REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
	Check(result[0], "A A", 12.75, -1, 8, 20);
	const std::array expensive{
		FontMeasurement{std::string(2100, ' '), 4, -1, 0, 0},
		FontMeasurement{std::string(2100, ' '), 4.75, -1, 0, 0}
	};
	const auto previous = result;
	CHECK(MeasureNativeSourceFont(font, expensive, Bytes, Work, result, failure) == Status::LimitExceeded);
	CHECK(result == previous);
	for (const auto &request : expensive) {
		const std::array single{request};
		REQUIRE(MeasureNativeSourceFont(font, single, Bytes, Work, result, failure) == Status::Ok);
		REQUIRE(result.size() == 1);
		CHECK(result[0].Text == request.Text);
		CHECK(result[0].MaximumLineWidth == request.MaximumLineWidth);
	}
	REQUIRE(MeasureNativeSourceFont(font, requests, Bytes, Work, result, failure) == Status::Ok);
	Check(result[0], "A A", 12.75, -1, 8, 20);
}
