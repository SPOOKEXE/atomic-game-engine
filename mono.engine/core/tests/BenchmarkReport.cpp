#include <engine/core/BenchmarkReport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.core.benchmarkreport")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.heapprofile")

namespace {
	struct ReportFile {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() / "mono-benchmark-report-test.json";
		~ReportFile() {
			std::error_code ignored;
			std::filesystem::remove(Path, ignored);
		}
	};
	double Metric(const std::string &report, std::string_view name) {
		const std::string key = "\"" + std::string(name) + "\": ";
		const size_t position = report.find(key);
		REQUIRE(position != std::string::npos);
		return std::stod(report.substr(position + key.size()));
	}
}

TEST_CASE(
	"benchmark aggregates all frames without adding nested or reported work twice", "[benchmarkreport]"
) {
	using namespace engine::core;
	const HeapTotals initial{.LiveBytes = 100, .TotalBytes = 500, .TotalBlocks = 10};
	BenchmarkReport benchmark(initial);
	std::array<FrameSpan, 7> spans = {
		{{.Name = "render", .SelfMilliseconds = 2, .Category = ProfileCategory::Render},
		 {.Name = "imagegraph.processor", .Depth = 1, .Parent = 0, .SelfMilliseconds = 3},
		 {.Name = "imagegraph.child", .Depth = 2, .Parent = 1, .SelfMilliseconds = 4},
		 {.Name = "kernel",
		  .Depth = 3,
		  .Parent = 2,
		  .SelfMilliseconds = 5,
		  .Category = ProfileCategory::Physics},
		 {.Name = "worker", .SelfMilliseconds = 99, .Category = ProfileCategory::Render, .Reported = true},
		 {.Name = "apply", .SelfMilliseconds = 6, .Category = ProfileCategory::Network},
		 {.Name = "scripts", .SelfMilliseconds = 7, .Category = ProfileCategory::Script}}
	};
	HeapTotals final{
		.LiveBytes = 150, .LiveBlocks = 2, .TotalBytes = 700, .TotalBlocks = 14, .PeakBytes = 300
	};
	benchmark.AddFrame(spans, 30, 2, final, 2);
	final.LiveBytes = 80;
	benchmark.AddFrame({}, 10, 1, final);
	REQUIRE(benchmark.FrameCount() == 2);
	ReportFile file;
	REQUIRE(benchmark.Write(file.Path, 5.0, 5.0, final, true));
	std::ifstream input(file.Path);
	std::ostringstream contents;
	contents << input.rdbuf();
	const std::string report = contents.str();
	CHECK(Metric(report, "render_ms_per_frame") == 1);
	CHECK(Metric(report, "physics_ms_per_frame") == 2.5);
	CHECK(Metric(report, "replication_ms_per_frame") == 3);
	CHECK(Metric(report, "script_ms_per_frame") == 3.5);
	CHECK(Metric(report, "imagegraph_ms_per_frame") == 6);
	CHECK(Metric(report, "frame_ms_per_frame") == 20);
	CHECK(Metric(report, "cpu_live_bytes") == 80);
	CHECK(Metric(report, "cpu_peak_bytes") == 150);
	CHECK(Metric(report, "cpu_process_peak_bytes") == 300);
	CHECK(Metric(report, "cpu_allocated_bytes") == 200);
	CHECK(Metric(report, "cpu_allocations_per_frame") == 2);
	CHECK(Metric(report, "dropped_spans") == 3);
	CHECK(Metric(report, "off_thread_dropped_spans") == 2);
	CHECK(Metric(report, "owner_dropped_spans") == 1);
	CHECK(Metric(report, "cpu_allocated_bytes_per_second") == 40);
	CHECK(Metric(report, "cpu_allocations_per_second") == 0.8);
	CHECK(Metric(report, "render_ms_per_second") == 0.4);
	CHECK(Metric(report, "physics_ms_per_second") == 1);
}

TEST_CASE("benchmark refuses unavailable incomplete and invalid measurements", "[benchmarkreport]") {
	using namespace engine::core;
	BenchmarkReport benchmark({});
	ReportFile file;
	CHECK_FALSE(benchmark.Write(file.Path, 5, 5, {}, true));
	benchmark.AddFrame({}, 1, 0, {});
	CHECK_FALSE(benchmark.Write(file.Path, 5, 5, {}, false));
	CHECK_FALSE(benchmark.Write(file.Path, 4.9, 5, {}, true));
	CHECK_FALSE(benchmark.Write(file.Path, 5, 0, {}, true));
	CHECK_FALSE(benchmark.Write(file.Path, std::numeric_limits<double>::infinity(), 5, {}, true));
	const std::array<BenchmarkMetric, 1> invalid{{{"bad\"name", 1}}};
	CHECK_FALSE(benchmark.Write(file.Path, 5, 5, {}, true, invalid));
	benchmark.AddFrame({}, std::numeric_limits<double>::quiet_NaN(), 0, {});
	CHECK_FALSE(benchmark.Write(file.Path, 5, 5, {}, true));
}

TEST_CASE(
	"benchmark wall time includes nested waits but avoids repeated category roots", "[benchmarkreport]"
) {
	using namespace engine::core;
	BenchmarkReport benchmark({});
	const std::array<FrameSpan, 6> spans = {
		{{.Name = "render", .Milliseconds = 10, .SelfMilliseconds = 1, .Category = ProfileCategory::Render},
		 {.Name = "imagegraph.processor",
		  .Depth = 1,
		  .Parent = 0,
		  .Milliseconds = 8,
		  .SelfMilliseconds = 2,
		  .Category = ProfileCategory::Render},
		 {.Name = "imagegraph.child", .Depth = 2, .Parent = 1, .Milliseconds = 5, .SelfMilliseconds = 1},
		 {.Name = "device wait",
		  .Depth = 3,
		  .Parent = 2,
		  .Milliseconds = 4,
		  .SelfMilliseconds = 4,
		  .Category = ProfileCategory::Idle},
		 {.Name = "physics", .Milliseconds = 3, .SelfMilliseconds = 3, .Category = ProfileCategory::Physics},
		 {.Name = "worker",
		  .Milliseconds = 99,
		  .SelfMilliseconds = 99,
		  .Category = ProfileCategory::Render,
		  .Reported = true}}
	};
	benchmark.AddFrame(spans, 20, 0, {});
	ReportFile file;
	REQUIRE(benchmark.Write(file.Path, 5, 5, {}, true));
	std::ifstream input(file.Path);
	std::ostringstream contents;
	contents << input.rdbuf();
	const std::string report = contents.str();
	CHECK(Metric(report, "render_ms_per_frame") == 3);
	CHECK(Metric(report, "render_wall_ms_per_frame") == 10);
	CHECK(Metric(report, "imagegraph_ms_per_frame") == 3);
	CHECK(Metric(report, "imagegraph_wall_ms_per_frame") == 8);
	CHECK(Metric(report, "physics_wall_ms_per_frame") == 3);
	CHECK(Metric(report, "render_wall_ms_per_second") == 2);
}
