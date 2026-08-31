#include <engine/core/Paths.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/script/ComputeJobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

TEST_SUITE_ID("engine.script.computejobs")

using engine::parallel::JobContext;
using engine::script::ComputeCompletion;
using engine::script::ComputeJobs;
using engine::script::NoiseGridRequest;

namespace {
	std::filesystem::path Self() {
		return engine::core::Paths::Base() / engine::core::Paths::Program("test_script");
	}

	NoiseGridRequest Request() {
		return NoiseGridRequest{
			.Width = 32,
			.Depth = 16,
			.OriginX = 0.125,
			.OriginY = -0.375,
			.OriginZ = 0.75,
			.Step = 0.03125,
		};
	}

	ComputeCompletion Await(ComputeJobs &jobs) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (std::chrono::steady_clock::now() < deadline) {
			jobs.Poll();
			if (!jobs.Completions().empty()) {
				return jobs.Completions().front();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		FAIL("compute job did not finish before the deadline");
		return {};
	}
}

TEST_CASE("serial and threaded noise grids are byte-identical", "[compute]") {
	ComputeJobs serial;
	ComputeJobs threaded;
	REQUIRE(serial.SubmitNoise(Request(), JobContext::Serial) != 0);
	REQUIRE(threaded.SubmitNoise(Request(), JobContext::Threaded) != 0);

	const ComputeCompletion first = Await(serial);
	const ComputeCompletion second = Await(threaded);
	CHECK(first.Error.empty());
	CHECK(second.Error.empty());
	CHECK(first.Values == second.Values);
	CHECK(first.Values.size() == 32 * 16);
	CHECK(std::abs(first.Values[17]) > 0.001f);
}

TEST_CASE("a malformed or oversized noise grid is refused before dispatch", "[compute]") {
	ComputeJobs jobs;
	NoiseGridRequest invalid = Request();
	invalid.Width = 0;
	CHECK(jobs.SubmitNoise(invalid, JobContext::Threaded) == 0);
	CHECK_FALSE(jobs.LastError().empty());

	invalid = Request();
	invalid.Width = 2048;
	invalid.Depth = 2048;
	CHECK(jobs.SubmitNoise(invalid, JobContext::Threaded) == 0);
}

TEST_CASE("completion heartbeat depends on sample count rather than worker speed", "[compute]") {
	ComputeJobs jobs;
	NoiseGridRequest request = Request();
	request.Width = 129;
	request.Depth = 96;
	REQUIRE(jobs.SubmitNoise(request, JobContext::Threaded) != 0);

	const size_t beats = (request.Width * request.Depth + ComputeJobs::SAMPLES_PER_HEARTBEAT - 1) /
						 ComputeJobs::SAMPLES_PER_HEARTBEAT;
	for (size_t beat = 1; beat < beats; beat++) {
		jobs.Poll();
		CHECK(jobs.Completions().empty());
	}
	jobs.Poll();
	REQUIRE(jobs.Completions().size() == 1);
	CHECK(jobs.Completions().front().Values.size() == request.Width * request.Depth);
}

TEST_CASE("destroying compute jobs cancels and joins threaded work", "[compute]") {
	NoiseGridRequest request = Request();
	request.Width = 1024;
	request.Depth = 1024;

	{
		ComputeJobs jobs;
		REQUIRE(jobs.SubmitNoise(request, JobContext::Threaded) != 0);
	}

	SUCCEED();
}

TEST_CASE("compute process worker child", "[.child]") {
	if (!engine::parallel::HasInheritedChannel()) {
		return;
	}
	CHECK(engine::script::RunComputeWorker() == 0);
}

TEST_CASE("processed noise crosses an owned byte channel", "[compute]") {
	engine::script::ConfigureComputeWorkerProgram(Self(), {"compute process worker child"});
	ComputeJobs jobs;
	REQUIRE(jobs.SubmitNoise(Request(), JobContext::Processed) != 0);
	// Let the child answer before the first deterministic publish heartbeat, so
	// this case validates the process result rather than the bounded fallback.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const ComputeCompletion processed = Await(jobs);
	engine::script::ConfigureComputeWorkerProgram({});

	ComputeJobs serial;
	REQUIRE(serial.SubmitNoise(Request(), JobContext::Serial) != 0);
	const ComputeCompletion expected = Await(serial);
	CHECK(processed.Error.empty());
	CHECK(processed.Values == expected.Values);
}

TEST_CASE("processed work is refused until its program configures a worker", "[compute]") {
	engine::script::ConfigureComputeWorkerProgram({});
	ComputeJobs jobs;
	CHECK(jobs.SubmitNoise(Request(), JobContext::Processed) == 0);
	CHECK(jobs.LastError().find("did not configure") != std::string::npos);
}
