#include "DemoReport.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

TEST_SUITE_ID("tools.benchrunner.demo-report")

namespace {
	using namespace benchrunner;

	struct DemoFixture {
		std::filesystem::path Directory =
			std::filesystem::temp_directory_path() /
			("mono-demo-report-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		DemoJson Report = {
			{"schema", 1},
			{"frame_count", 300},
			{"duration_seconds", 5.01},
			{"heap_compiled", true},
			{"dropped_spans", 0},
			{"heap_dropped_scopes", 0},
			{"metrics",
			 {{"cpu_live_bytes", 100},
			  {"cpu_peak_bytes", 200},
			  {"cpu_allocated_bytes", 300},
			  {"cpu_allocations", 30},
			  {"render_ms_per_frame", 1},
			  {"physics_ms_per_frame", 2},
			  {"replication_ms_per_frame", 0},
			  {"script_ms_per_frame", 0},
			  {"imagegraph_ms_per_frame", 0},
			  {"frame_ms_per_frame", 4},
			  {"draw_calls_per_frame", 12}}}
		};
		DemoJson Manifest = {
			{"schema", 1},
			{"source_revision", "fixture"},
			{"source_dirty", false},
			{"label", "Fixture"},
			{"machine", {{"cpu", "test cpu"}}},
			{"settings",
			 {{"runs", 3}, {"seconds", 5}, {"preset", "profile"}, {"width", 960}, {"height", 540}}},
			{"workloads",
			 {{{"name", "Demo"}, {"status", "measured"}, {"reports", {"1.json", "2.json", "3.json"}}}}}
		};
		~DemoFixture() {
			std::filesystem::remove_all(Directory);
		}
		void Write() const {
			std::filesystem::create_directories(Directory);
			std::ofstream(Directory / "manifest.json") << Manifest;
			for (const char *name : {"1.json", "2.json", "3.json"})
				std::ofstream(Directory / name) << Report;
		}
	};
}

TEST_CASE("demo collection averages independent runs and normalizes churn", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	fixture.Report["metrics"]["render_ms_per_frame"] = 4;
	std::ofstream(fixture.Directory / "3.json") << fixture.Report;
	const auto collection = DemoLoad(fixture.Directory);
	const auto &metrics = collection.Workloads.at("Demo").Metrics;
	CHECK(metrics.at("render_ms_per_frame").Mean == 2);
	CHECK(metrics.at("render_ms_per_frame").Minimum == 1);
	CHECK(metrics.at("render_ms_per_frame").Maximum == 4);
	CHECK(std::abs(metrics.at("render_ms_per_frame").Deviation - std::sqrt(2.0)) < 1e-12);
	CHECK(metrics.at("cpu_allocations_per_frame").Mean == 0.1);
}

TEST_CASE("demo reports reject incomplete collection and incomplete measurements", "[benchrunner]") {
	DemoFixture fixture;
	SECTION("short runtime") {
		fixture.Report["duration_seconds"] = 4.9;
	}
	SECTION("runtime shorter than the requested window") {
		fixture.Manifest["settings"]["seconds"] = 15;
	}
	SECTION("requested window below five seconds") {
		fixture.Manifest["settings"]["seconds"] = 4.9;
	}
	SECTION("nonfinite requested window") {
		fixture.Manifest["settings"]["seconds"] = std::numeric_limits<double>::infinity();
		CHECK_THROWS(DemoValidateManifest(fixture.Manifest));
	}
	SECTION("missing heap hooks") {
		fixture.Report["heap_compiled"] = false;
	}
	SECTION("empty frame window") {
		fixture.Report["frame_count"] = 0;
	}
	SECTION("dropped spans") {
		fixture.Report["dropped_spans"] = 1;
	}
	SECTION("missing required metric") {
		fixture.Report["metrics"].erase("physics_ms_per_frame");
	}
	SECTION("negative metric") {
		fixture.Report["metrics"]["cpu_live_bytes"] = -1;
	}
	SECTION("invalid numeric metric") {
		fixture.Report["metrics"]["cpu_live_bytes"] = "bad";
	}
	SECTION("repeated report") {
		fixture.Manifest["workloads"][0]["reports"][2] = "1.json";
	}
	SECTION("escaping report") {
		fixture.Manifest["workloads"][0]["reports"][0] = "../1.json";
	}
	SECTION("missing report") {
		fixture.Manifest["workloads"][0]["reports"][0] = "missing.json";
	}
	SECTION("wrong repetition count") {
		fixture.Manifest["settings"]["runs"] = 2;
	}
	fixture.Write();
	CHECK_THROWS(DemoLoad(fixture.Directory));
}

TEST_CASE("demo reports reject inconsistent metric coverage between runs", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	fixture.Report["metrics"]["extra_bytes"] = 9;
	std::ofstream(fixture.Directory / "3.json") << fixture.Report;
	CHECK_THROWS(DemoLoad(fixture.Directory));
}

TEST_CASE("demo reports do not duplicate runtime normalized metrics", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["metrics"]["cpu_allocations_per_frame"] = 0.1;
	fixture.Write();
	const auto collection = DemoLoad(fixture.Directory);
	CHECK(collection.Workloads.at("Demo").Metrics.at("cpu_allocations_per_frame").Mean == 0.1);
}

TEST_CASE(
	"demo comparison distinguishes regression, diagnostic throughput and new workload", "[benchrunner]"
) {
	DemoFixture fixture;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	auto current = baseline;
	current.Workloads.at("Demo").Metrics.at("render_ms_per_second").Mean = 100;
	current.Workloads.at("Demo").Metrics.at("draw_calls_per_frame").Mean = 100;
	current.Workloads.emplace("New demo", current.Workloads.at("Demo"));
	std::ostringstream document;
	CHECK_FALSE(DemoCompareTable(document, baseline, current, DemoLimits{}));
	CHECK(document.str().find("FAIL") != std::string::npos);
	CHECK(document.str().find("Diagnostic") != std::string::npos);
	CHECK(document.str().find("Added workload") != std::string::npos);
}

TEST_CASE("demo comparison refuses lost baseline coverage", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	auto baseline = DemoLoad(fixture.Directory);
	auto current = baseline;
	SECTION("workload removed") {
		current.Workloads.clear();
	}
	SECTION("workload unavailable") {
		current.Workloads.at("Demo").Status = "unavailable";
	}
	SECTION("added workload absent on both revisions") {
		baseline.Workloads.at("Demo").Status = "unavailable";
		current.Workloads.at("Demo").Status = "unavailable";
	}
	SECTION("new unavailable workload") {
		DemoWorkload missing;
		missing.Status = "unavailable";
		current.Workloads.emplace("Missing new demo", std::move(missing));
	}
	SECTION("metric removed") {
		current.Workloads.at("Demo").Metrics.erase("physics_ms_per_frame");
	}
	std::ostringstream document;
	bool coverageValid = true;
	CHECK_FALSE(DemoCompareTable(document, baseline, current, DemoLimits{}, &coverageValid));
	CHECK_FALSE(coverageValid);
}

TEST_CASE("demo cost ceilings handle zero baselines and configurable absolute floors", "[benchrunner]") {
	DemoLimits limits;
	CHECK(DemoCeiling(0, "physics_ms_per_frame", limits) == 0.1);
	CHECK(DemoCeiling(0, "cpu_live_bytes", limits) == 1048576);
	CHECK(DemoCeiling(0, "cpu_allocations_per_frame", limits) == 100);
	limits.Bytes = 0;
	CHECK(DemoCeiling(0, "cpu_live_bytes", limits) == 0);
	CHECK(DemoCeiling(100, "render_ms_per_frame", limits) == 115);
}

TEST_CASE(
	"demo CLI writes reviewable comparisons and gates incompatible machines or settings", "[benchrunner]"
) {
	DemoFixture baseline;
	DemoFixture current;
	baseline.Write();
	SECTION("different machine") {
		current.Manifest["machine"]["cpu"] = "another cpu";
	}
	SECTION("different requested duration") {
		current.Manifest["settings"]["seconds"] = 15;
		current.Report["duration_seconds"] = 15.01;
	}
	current.Write();
	const auto document = current.Directory / "impact.md";
	std::vector<std::string> arguments{
		"benchrunner",
		"--demo-compare",
		baseline.Directory.string(),
		current.Directory.string(),
		"--demo-document",
		document.string()
	};
	std::vector<char *> pointers;
	for (std::string &argument : arguments)
		pointers.push_back(argument.data());
	std::ostringstream captured;
	auto *previous = std::cout.rdbuf(captured.rdbuf());
	const int status = DemoReportMain(static_cast<int>(pointers.size()), pointers.data());
	std::cout.rdbuf(previous);
	CHECK(status == 1);
	CHECK(std::filesystem::file_size(document) > 0);
	CHECK(captured.str().find("Machine or settings mismatch: FAIL") != std::string::npos);
	arguments.push_back("--demo-advisory");
	pointers.clear();
	for (std::string &argument : arguments)
		pointers.push_back(argument.data());
	previous = std::cout.rdbuf(captured.rdbuf());
	const int advisory = DemoReportMain(static_cast<int>(pointers.size()), pointers.data());
	std::cout.rdbuf(previous);
	CHECK(advisory == 0);
	current.Manifest["workloads"][0]["status"] = "unavailable";
	current.Write();
	previous = std::cout.rdbuf(captured.rdbuf());
	const int missingCoverage = DemoReportMain(static_cast<int>(pointers.size()), pointers.data());
	std::cout.rdbuf(previous);
	CHECK(missingCoverage == 1);
}

TEST_CASE("embedded baseline roundtrips exact aggregates without runtime files", "[benchrunner]") {
	DemoFixture fixture;
	SECTION("historical five-second window") {}
	SECTION("fifteen-second window") {
		fixture.Manifest["settings"]["seconds"] = 15;
		fixture.Report["duration_seconds"] = 15.01;
	}
	fixture.Report["cpu_peak_basis"] = "frame_end_samples";
	fixture.Report["metrics"]["render_ms_per_frame"] = 1.234567891234;
	fixture.Write();
	const auto collected = DemoLoad(fixture.Directory);
	std::ostringstream document;
	DemoEmbeddedBaseline(document, collected);
	const auto path = fixture.Directory / "baseline.md";
	std::ofstream(path) << document.str();
	std::filesystem::remove(fixture.Directory / "1.json");
	const auto restored = DemoLoadBaseline(path);
	CHECK(restored.Manifest == collected.Manifest);
	CHECK(restored.Workloads.at("Demo").Basis == collected.Workloads.at("Demo").Basis);
	for (const auto &[metric, expected] : collected.Workloads.at("Demo").Metrics) {
		const auto &actual = restored.Workloads.at("Demo").Metrics.at(metric);
		CHECK(actual.Mean == expected.Mean);
		CHECK(actual.Deviation == expected.Deviation);
		CHECK(actual.Minimum == expected.Minimum);
		CHECK(actual.Maximum == expected.Maximum);
	}
}

TEST_CASE("embedded baseline rejects malformed aggregates and metadata", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	auto aggregate = DemoAggregateJson(DemoLoad(fixture.Directory));
	SECTION("missing metric") {
		aggregate["workloads"]["Demo"]["metrics"].erase("physics_ms_per_frame");
	}
	SECTION("negative mean") {
		aggregate["workloads"]["Demo"]["metrics"]["cpu_live_bytes"]["mean"] = -1;
	}
	SECTION("inconsistent range") {
		aggregate["workloads"]["Demo"]["metrics"]["cpu_live_bytes"]["min"] = 101;
	}
	SECTION("short measurement") {
		aggregate["workloads"]["Demo"]["metrics"]["duration_seconds"]["min"] = 4;
	}
	SECTION("measurement shorter than the requested window") {
		aggregate["manifest"]["settings"]["seconds"] = 15;
	}
	SECTION("settings changed") {
		aggregate["manifest"]["settings"]["runs"] = 1;
	}
	SECTION("status changed") {
		aggregate["workloads"]["Demo"]["status"] = "unavailable";
	}
	SECTION("extra workload") {
		aggregate["workloads"]["unexpected"] = aggregate["workloads"]["Demo"];
	}
	CHECK_THROWS(DemoLoadAggregate(aggregate));
}

TEST_CASE("demo comparison rejects differing measurement semantics", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	auto current = baseline;
	current.Workloads.at("Demo").Basis["cpu_peak_basis"] = "different";
	bool coverage = true;
	std::ostringstream document;
	CHECK_FALSE(DemoCompareTable(document, baseline, current, DemoLimits{}, &coverage));
	CHECK_FALSE(coverage);
	CHECK(DemoDiagnostic("tick_rate_hz"));
	CHECK(DemoDiagnostic("tick_count"));
	CHECK(DemoDiagnostic("clients_admitted"));
	CHECK(DemoDiagnostic("network_packets_sent"));
	CHECK(DemoFloor("cpu_allocated_bytes_per_frame", DemoLimits{}) == 1024);
}

TEST_CASE("demo collection includes whole-process peak RSS and preserves its basis", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	for (const char *name : {"1.json.rss", "2.json.rss", "3.json.rss"}) {
		std::ofstream(fixture.Directory / name) << "12345\n";
	}
	const auto collection = DemoLoad(fixture.Directory);
	CHECK(collection.Workloads.at("Demo").Metrics.at("process_peak_rss_bytes").Mean == 12345 * 1024);
	CHECK(collection.Workloads.at("Demo").Basis.at("process_peak_rss_basis") == "whole_process_lifetime");
	const auto restored = DemoLoadAggregate(DemoAggregateJson(collection));
	CHECK(restored.Workloads.at("Demo").Metrics.at("process_peak_rss_bytes").Mean == 12345 * 1024);
	CHECK(restored.Workloads.at("Demo").Basis == collection.Workloads.at("Demo").Basis);
}

TEST_CASE("demo collection refuses invalid or incomplete external RSS coverage", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	std::string value = "invalid";
	SECTION("noninteger") {
		value = "12.5";
	}
	SECTION("negative") {
		value = "-12";
	}
	SECTION("zero") {
		value = "0";
	}
	SECTION("trailing garbage") {
		value = "123 extra";
	}
	SECTION("out of range") {
		value = std::string(400, '9');
	}
	SECTION("inconsistent coverage") {
		value = "123";
	}
	std::ofstream(fixture.Directory / "1.json.rss") << value;
	CHECK_THROWS(DemoLoad(fixture.Directory));
}

TEST_CASE("demo reports distinguish expected off-thread spans from incomplete owner spans", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["owner_dropped_spans"] = 0;
	fixture.Report["off_thread_dropped_spans"] = 428;
	fixture.Report["dropped_spans"] = 428;
	fixture.Report["frame_basis"] = "update_iteration_or_server_tick";
	fixture.Write();
	const auto collected = DemoLoad(fixture.Directory);
	CHECK(collected.Workloads.at("Demo").Metrics.at("framegraph_off_thread_dropped_spans").Mean == 428);
	const auto aggregate = DemoAggregateJson(collected);
	CHECK(aggregate.at("dropped_spans") == 428);
	CHECK_NOTHROW(DemoLoadAggregate(aggregate));
	fixture.Report["owner_dropped_spans"] = 1;
	fixture.Report["off_thread_dropped_spans"] = 427;
	fixture.Write();
	CHECK_THROWS(DemoLoad(fixture.Directory));
	fixture.Report["owner_dropped_spans"] = 0;
	fixture.Write();
	CHECK_THROWS(DemoLoad(fixture.Directory));
}

TEST_CASE("demo cost normalization uses actual wall-clock window and per-second floors", "[benchrunner]") {
	DemoFixture fixture;
	double actualSeconds = 5.01;
	SECTION("historical five-second window") {}
	SECTION("fifteen-second window") {
		fixture.Manifest["settings"]["seconds"] = 15;
		actualSeconds = 15.01;
	}
	fixture.Report["duration_seconds"] = actualSeconds;
	fixture.Write();
	const auto collected = DemoLoad(fixture.Directory);
	const auto &metrics = collected.Workloads.at("Demo").Metrics;
	CHECK(std::abs(metrics.at("cpu_allocations_per_second").Mean - 30 / actualSeconds) < 1e-12);
	CHECK(std::abs(metrics.at("render_ms_per_second").Mean - 300 / actualSeconds) < 1e-12);
	std::ostringstream document;
	DemoMetadata(document, collected, "Fixture");
	const std::string requestedSeconds = fixture.Manifest["settings"]["seconds"].dump();
	CHECK(
		document.str().find("Three independent " + requestedSeconds + "-second runs.") != std::string::npos
	);
	CHECK_FALSE(DemoDiagnostic("render_ms_per_frame"));
	CHECK_FALSE(DemoDiagnostic("render_ms_per_second"));
	CHECK(DemoFloor("render_ms_per_second", DemoLimits{}) == 5);
	CHECK(DemoFloor("cpu_allocations_per_second", DemoLimits{}) == 1000);
	CHECK(DemoFloor("cpu_allocated_bytes_per_second", DemoLimits{}) == 65536);
}

TEST_CASE("equal fifteen-second windows still reject GPU allocation rate regressions", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Manifest["settings"]["seconds"] = 15;
	fixture.Report["duration_seconds"] = 15.01;
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 15'010'000;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 18'012'000;
	fixture.Write();
	const auto current = DemoLoad(fixture.Directory);
	CHECK(
		std::abs(
			baseline.Workloads.at("Demo").Metrics.at("gpu_allocated_bytes_per_second").Mean - 1'000'000
		) < 1e-6
	);
	CHECK(
		std::abs(current.Workloads.at("Demo").Metrics.at("gpu_allocated_bytes_per_second").Mean - 1'200'000) <
		1e-6
	);
	std::ostringstream document;
	CHECK_FALSE(DemoCompareTable(document, baseline, current, DemoLimits{}));
	CHECK(
		document.str().find(
			"gpu_allocated_bytes_per_second | 1000000.000 | 1200000.000 | +200000.000 (20.000%) | "
			"1150000.000 | FAIL"
		) != std::string::npos
	);
	std::ostringstream iterationDocument;
	CHECK_FALSE(DemoCompareTable(
		iterationDocument, baseline, current, DemoLimits{}, nullptr, DemoCostBasis::Iteration
	));
	CHECK(iterationDocument.str().find("gpu_allocated_bytes_per_frame") != std::string::npos);
}

TEST_CASE("iteration cost basis gates per-frame churn independently of frame rate", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 3'000'000;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	SECTION("faster frames with unchanged per-frame churn pass iteration and fail capacity") {
		fixture.Report["frame_count"] = 450;
		fixture.Report["metrics"]["gpu_allocated_bytes"] = 4'500'000;
	}
	SECTION("slower frames with higher per-frame churn fail iteration despite a lower rate") {
		fixture.Report["frame_count"] = 150;
		fixture.Report["metrics"]["gpu_allocated_bytes"] = 1'800'000;
	}
	fixture.Write();
	const auto current = DemoLoad(fixture.Directory);
	std::ostringstream iterationDocument;
	const bool iterationPassed = DemoCompareTable(
		iterationDocument, baseline, current, DemoLimits{}, nullptr, DemoCostBasis::Iteration
	);
	std::ostringstream capacityDocument;
	const bool capacityPassed = DemoCompareTable(capacityDocument, baseline, current, DemoLimits{});
	const bool faster = fixture.Report["frame_count"] == 450;
	CHECK(iterationPassed == faster);
	CHECK(capacityPassed == !faster);
	if (faster) {
		CHECK(
			iterationDocument.str().find("gpu_allocated_bytes_per_frame | 10000.000 | 10000.000") !=
			std::string::npos
		);
		CHECK(DemoDiagnostic("gpu_allocated_bytes_per_second", DemoCostBasis::Iteration));
		CHECK(DemoDiagnostic("render_ms_per_second", DemoCostBasis::Iteration));
		CHECK(capacityDocument.str().find("gpu_allocated_bytes_per_second") != std::string::npos);
	} else {
		CHECK(
			iterationDocument.str().find("gpu_allocated_bytes_per_frame | 10000.000 | 12000.000") !=
			std::string::npos
		);
		CHECK(iterationDocument.str().find("gpu_allocated_bytes_per_second") != std::string::npos);
	}
}

TEST_CASE("iteration cost basis fails when paired per-frame coverage is missing", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 3'000;
	fixture.Write();
	auto baseline = DemoLoad(fixture.Directory);
	auto current = baseline;
	SECTION("frame denominator missing") {
		current.Workloads.at("Demo").Metrics.erase("frame_count");
	}
	SECTION("matching churn per-frame counter missing") {
		baseline.Workloads.at("Demo").Metrics.erase("gpu_allocated_bytes_per_frame");
		current.Workloads.at("Demo").Metrics.erase("gpu_allocated_bytes_per_frame");
	}
	std::ostringstream document;
	bool coverage = true;
	CHECK_FALSE(
		DemoCompareTable(document, baseline, current, DemoLimits{}, &coverage, DemoCostBasis::Iteration)
	);
	CHECK_FALSE(coverage);
	CHECK(
		document.str().find("collect fresh reports with matching counters and denominators") !=
		std::string::npos
	);
}

TEST_CASE("iteration cost basis rejects faster frames with higher per-frame churn", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["frame_count"] = 300;
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 3'000'000;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	fixture.Report["frame_count"] = 450;
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 5'400'000;
	fixture.Write();
	const auto current = DemoLoad(fixture.Directory);
	std::ostringstream iterationDocument;
	CHECK_FALSE(DemoCompareTable(
		iterationDocument, baseline, current, DemoLimits{}, nullptr, DemoCostBasis::Iteration
	));
	CHECK(
		iterationDocument.str().find("gpu_allocated_bytes_per_frame | 10000.000 | 12000.000") !=
		std::string::npos
	);
}

TEST_CASE("iteration cost basis rejects stale or missing raw churn counters", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["metrics"]["gpu_allocated_bytes"] = 3'000'000;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	SECTION("stale explicit per-frame value leaves wall policy unchanged") {
		fixture.Report["metrics"]["gpu_allocated_bytes_per_frame"] = 12'000;
	}
	SECTION("per-frame and rate values lack their raw counter") {
		fixture.Report["metrics"].erase("gpu_allocated_bytes");
		fixture.Report["metrics"]["gpu_allocated_bytes_per_frame"] = 10'000;
		fixture.Report["metrics"]["gpu_allocated_bytes_per_second"] = 598'802.3952095808;
	}
	fixture.Write();
	const auto current = DemoLoad(fixture.Directory);
	std::ostringstream iterationDocument;
	bool iterationCoverage = true;
	CHECK_FALSE(DemoCompareTable(
		iterationDocument, baseline, current, DemoLimits{}, &iterationCoverage, DemoCostBasis::Iteration
	));
	CHECK_FALSE(iterationCoverage);
	CHECK(
		iterationDocument.str().find("collect fresh reports with matching counters and denominators") !=
		std::string::npos
	);
	if (fixture.Report["metrics"].contains("gpu_allocated_bytes")) {
		std::ostringstream capacityDocument;
		CHECK(DemoCompareTable(capacityDocument, baseline, current, DemoLimits{}));
	}
}

TEST_CASE("iteration cost basis rejects mismatched timing-rate provenance", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["metrics"]["render_ms_per_second"] = 300.0 / 5.01;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	fixture.Report["metrics"]["render_ms_per_second"] = 10;
	fixture.Write();
	const auto current = DemoLoad(fixture.Directory);
	std::ostringstream iterationDocument;
	bool iterationCoverage = true;
	CHECK_FALSE(DemoCompareTable(
		iterationDocument, baseline, current, DemoLimits{}, &iterationCoverage, DemoCostBasis::Iteration
	));
	CHECK_FALSE(iterationCoverage);
	std::ostringstream capacityDocument;
	CHECK(DemoCompareTable(capacityDocument, baseline, current, DemoLimits{}));
}

TEST_CASE("legacy aggregate input cannot claim iteration provenance", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	const auto collection = DemoLoad(fixture.Directory);
	auto aggregate = DemoAggregateJson(collection);
	const auto legacy = DemoLoadAggregate(aggregate);
	std::ostringstream iterationDocument;
	bool iterationCoverage = true;
	CHECK_FALSE(DemoCompareTable(
		iterationDocument, legacy, legacy, DemoLimits{}, &iterationCoverage, DemoCostBasis::Iteration
	));
	CHECK_FALSE(iterationCoverage);
	std::ostringstream capacityDocument;
	CHECK(DemoCompareTable(capacityDocument, legacy, legacy, DemoLimits{}));
}

TEST_CASE("iteration coverage validates measured Added workloads", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Write();
	const auto measured = DemoLoad(fixture.Directory);
	auto baseline = measured;
	auto current = measured;
	current.Workloads.at("Demo").IterationBasisValid = false;
	SECTION("baseline marks the workload unavailable") {
		baseline.Workloads.at("Demo").Status = "unavailable";
		baseline.Workloads.at("Demo").Metrics.clear();
	}
	SECTION("current contains a new workload") {
		baseline.Workloads.clear();
	}
	std::ostringstream iterationDocument;
	bool iterationCoverage = true;
	CHECK_FALSE(DemoCompareTable(
		iterationDocument, baseline, current, DemoLimits{}, &iterationCoverage, DemoCostBasis::Iteration
	));
	CHECK_FALSE(iterationCoverage);
	CHECK(iterationDocument.str().find("FAIL: coverage") != std::string::npos);
	CHECK(iterationDocument.str().find("Added") != std::string::npos);
	std::ostringstream capacityDocument;
	CHECK(DemoCompareTable(capacityDocument, baseline, current, DemoLimits{}));
}

TEST_CASE("iteration cost basis keeps unknown per-second metrics strict", "[benchrunner]") {
	DemoFixture fixture;
	fixture.Report["metrics"]["background_jobs_per_second"] = 10;
	fixture.Report["metrics"]["network_packets_per_second"] = 10;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	fixture.Report["metrics"]["background_jobs_per_second"] = 12;
	fixture.Report["metrics"]["network_packets_per_second"] = 12;
	fixture.Write();
	const auto current = DemoLoad(fixture.Directory);
	std::ostringstream document;
	CHECK_FALSE(
		DemoCompareTable(document, baseline, current, DemoLimits{}, nullptr, DemoCostBasis::Iteration)
	);
	CHECK(document.str().find("background_jobs_per_second | 10.000 | 12.000") != std::string::npos);
	CHECK(
		document.str().find(
			"background_jobs_per_second | 10.000 | 12.000 | +2.000 (20.000%) | 11.500 | FAIL"
		) != std::string::npos
	);
	CHECK(DemoDiagnostic("network_packets_per_second"));
	CHECK_FALSE(DemoDiagnostic("network_packets_per_second", DemoCostBasis::Iteration));
	CHECK(document.str().find("network_packets_per_second | 10.000 | 12.000") != std::string::npos);
}

TEST_CASE("demo CLI accepts and documents the opt-in iteration cost basis", "[benchrunner]") {
	DemoFixture baseline;
	DemoFixture current;
	baseline.Write();
	current.Write();
	std::vector<std::string> arguments{
		"benchrunner",
		"--demo-compare",
		baseline.Directory.string(),
		current.Directory.string(),
		"--demo-cost-basis",
		"iteration"
	};
	std::vector<char *> pointers;
	for (std::string &argument : arguments)
		pointers.push_back(argument.data());
	std::ostringstream captured;
	auto *previous = std::cout.rdbuf(captured.rdbuf());
	const int status = DemoReportMain(static_cast<int>(pointers.size()), pointers.data());
	std::cout.rdbuf(previous);
	CHECK(status == 0);
	CHECK(captured.str().find("Cost basis: iteration efficiency") != std::string::npos);
	CHECK(
		captured.str().find(
			"Known derived timing and churn rates per second remain visible as diagnostics"
		) != std::string::npos
	);
	arguments = {
		"benchrunner", "--demo-table", baseline.Directory.string(), "--demo-cost-basis", "iteration"
	};
	pointers.clear();
	for (std::string &argument : arguments)
		pointers.push_back(argument.data());
	CHECK(DemoReportMain(static_cast<int>(pointers.size()), pointers.data()) == 2);
}

TEST_CASE(
	"demo comparison discloses changed input hashes and gates elapsed per-frame cost", "[benchrunner]"
) {
	DemoFixture fixture;
	fixture.Manifest["workloads"][0]["asset_sha256"] = "old";
	fixture.Report["metrics"]["render_wall_ms_per_frame"] = 1;
	fixture.Write();
	const auto baseline = DemoLoad(fixture.Directory);
	auto current = baseline;
	current.Manifest["workloads"][0]["asset_sha256"] = "new";
	current.Workloads.at("Demo").Metrics.at("render_wall_ms_per_frame").Mean = 2;
	std::ostringstream document;
	CHECK_FALSE(DemoCompareTable(document, baseline, current, DemoLimits{}));
	CHECK(document.str().find("Input warning") != std::string::npos);
	CHECK_FALSE(DemoDiagnostic("render_wall_ms_per_frame"));
}

TEST_CASE("demo measurements require real GPU submissions and admitted viewers", "[benchrunner]") {
	DemoFixture fixture;
	SECTION("client submits no GPU work") {
		fixture.Manifest["workloads"][0]["kind"] = "client";
		fixture.Report["metrics"]["submitted_frames"] = 0;
	}
	SECTION("replication server admits no viewer") {
		fixture.Manifest["workloads"][0]["kind"] = "server-replica";
		fixture.Report["metrics"]["clients_admitted"] = 0;
	}
	fixture.Write();
	CHECK_THROWS(DemoLoad(fixture.Directory));
}
