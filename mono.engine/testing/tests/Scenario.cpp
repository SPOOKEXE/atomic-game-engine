#include <engine/testing/Scenario.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

TEST_SUITE_ID("engine.testing.scenario")
TEST_TIER("fast")

using namespace engine::testing;

namespace {
	ScenarioKey Key() {
		return {
			.Scene = "copied-scene",
			.ContentRevision = "content-1",
			.Configuration = "default",
			.TickSchedule = "0,1",
			.Actions = "none",
			.ObservationSchema = "integer-v1",
			.CheckSet = "checks-v1"
		};
	}

	ScenarioCheck<int> Check(std::string name = "positive", std::string hook = "tick") {
		ScenarioCheck<int> check;
		check.Name = std::move(name);
		check.SourceSuite = "engine.testing.scenario";
		check.Requirements.Hooks = {{std::move(hook), 1}};
		check.Observe = [](const Observation<int> &observation, CheckResult &output) {
			output.Record("positive", observation.Payload > 0, "copied value must be positive");
		};
		return check;
	}

	ScenarioDriver<int> Driver(int &setups, int &finishes) {
		return {
			.Prepare = [&](ScenarioSession<int> &) { setups++; },
			.Run =
				[](ScenarioSession<int> &session) {
					session.Publish({.Hook = "tick", .Tick = 0, .Payload = 3});
					session.Publish({.Hook = "tick", .Tick = 1, .Payload = 7});
				},
			.Finish = [&] { finishes++; }
		};
	}

	struct CallbackCopies {
		size_t Count = 0;
		bool Throw = false;
	};

	struct CopyCheckedCallback {
		std::shared_ptr<CallbackCopies> Copies;
		explicit CopyCheckedCallback(std::shared_ptr<CallbackCopies> copies) : Copies(std::move(copies)) {}
		CopyCheckedCallback(const CopyCheckedCallback &source) : Copies(source.Copies) {
			Copies->Count++;
			if (Copies->Throw) throw std::runtime_error("callback was copied");
		}
		CopyCheckedCallback(CopyCheckedCallback &&) noexcept = default;
		void operator()(const Observation<int> &observation, CheckResult &output) const {
			output.Record("copied callback", observation.Payload > 0);
		}
	};

	struct Pool {
		bool PreviousForceSerial = engine::parallel::ForceSerialCompute();
		Pool() {
			engine::parallel::Jobs::Stop();
			engine::parallel::SetForceSerialCompute(false);
			engine::parallel::Jobs::Start(2);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
			engine::parallel::SetForceSerialCompute(PreviousForceSerial);
		}
	};
}

TEST_CASE("one native driver fans the same immutable observation to independent checks", "[scenario]") {
	Pool pool;
	int setups = 0;
	int finishes = 0;
	std::array<const Observation<int> *, 2> seen{};
	std::array<std::thread::id, 2> threads{};
	REQUIRE(engine::parallel::Jobs::WorkerCount() == 2);
	std::barrier rendezvous(2);
	std::vector<ScenarioCheck<int>> checks;
	for (size_t index = 0; index < seen.size(); index++) {
		auto check = Check("check-" + std::to_string(index));
		check.Observe = [&, index](const Observation<int> &observation, CheckResult &output) {
			seen[index] = &observation;
			threads[index] = std::this_thread::get_id();
			// A fresh available pool and an unforced top-level dispatch guarantee
			// two ranges. Rendezvous proves overlap without scheduling deadlines.
			rendezvous.arrive_and_wait();
			output.Record("copied", observation.Payload == 3 || observation.Payload == 7);
		};
		checks.push_back(std::move(check));
	}
	const auto report = RunScenario(Key(), std::move(checks), Driver(setups, finishes));
	REQUIRE(report.Passed());
	CHECK(setups == 1);
	CHECK(finishes == 1);
	CHECK(seen[0] == seen[1]);
	CHECK(threads[0] != threads[1]);
	CHECK(report.Metrics.SetupCount == 1);
	CHECK(report.Metrics.PublishedSamples == 2);
	CHECK(report.Metrics.PeakRetainedSamples == 0);
	CHECK(report.Checks[0].Samples == 2);
	CHECK(report.Checks[1].Samples == 2);
}

TEST_CASE("complete keys reuse successful reports and reject structural aliases", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	ScenarioCache<int> cache(2);
	const auto first = cache.Run(Key(), {Check()}, Driver(setups, finishes));
	const auto reused = cache.Run(Key(), {Check()}, Driver(setups, finishes));
	REQUIRE(first.Passed());
	CHECK(reused.Passed());
	CHECK(reused.Reused);
	CHECK(reused.Metrics.SetupCount == 0);
	CHECK(reused.Checks == first.Checks);
	CHECK(setups == 1);
	CHECK(finishes == 1);
	auto changed = Key();
	changed.Configuration = "different initial state";
	CHECK(cache.Run(changed, {Check()}, Driver(setups, finishes)).Passed());
	CHECK(setups == 2);
	CHECK(cache.Size() == 2);
	CHECK_FALSE(cache.Run(Key(), {Check("other")}, Driver(setups, finishes)).Passed());
	CHECK(setups == 2);
	changed.Seed = 9;
	CHECK(cache.Run(changed, {Check()}, Driver(setups, finishes)).Passed());
	CHECK(cache.Size() == 2);
	cache.Clear();
	CHECK(cache.Size() == 0);
}

TEST_CASE("missing empty incomplete and lost hook evidence fail only dependent checks", "[scenario]") {
	int finished = 0;
	ScenarioDriver<int> driver{
		.Run =
			[](ScenarioSession<int> &session) {
				session.Publish({.Hook = "tick", .Payload = 1});
				session.Publish({.Hook = "physics", .Payload = 1, .LostSamples = 4});
				session.Publish(
					{.Hook = "render",
					 .Payload = 1,
					 .Available = false,
					 .Complete = false,
					 .Failure = "readback failed"}
				);
			},
		.Finish = [&] { finished++; }
	};
	const auto report = RunScenario(
		Key(),
		{Check(), Check("missing", "absent"), Check("loss", "physics"), Check("readback", "render")},
		driver
	);
	CHECK_FALSE(report.Passed());
	CHECK(report.Checks[0].Passed);
	CHECK_FALSE(report.Checks[1].Passed);
	CHECK_FALSE(report.Checks[2].Passed);
	CHECK_FALSE(report.Checks[3].Passed);
	CHECK(report.Checks[1].Samples == 0);
	CHECK(finished == 1);
	const auto empty = RunScenario<int>(Key(), {Check()}, {.Run = [](ScenarioSession<int> &) {}});
	CHECK_FALSE(empty.Passed());
}

TEST_CASE("check exceptions remain attributed while peers finish every sample", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	auto broken = Check("throws");
	broken.Observe = [](const Observation<int> &, CheckResult &) { throw std::runtime_error("bad check"); };
	const auto report = RunScenario(Key(), {broken, Check()}, Driver(setups, finishes));
	CHECK_FALSE(report.Passed());
	CHECK_FALSE(report.Checks[0].Passed);
	CHECK(report.Checks[0].Findings[0].Message == "bad check");
	CHECK(report.Checks[1].Passed);
	CHECK(report.Checks[1].Samples == 2);
	CHECK(finishes == 1);
	auto completion = Check("completion-throws");
	completion.Complete = [](auto, CheckResult &) { throw 42; };
	const auto nonstandard = RunScenario(Key(), {completion, Check()}, Driver(setups, finishes));
	CHECK_FALSE(nonstandard.Checks[0].Passed);
	CHECK(nonstandard.Checks[1].Passed);
}

TEST_CASE("streaming continues when the bounded temporal archive fills", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	auto timeline = Check("timeline");
	timeline.Requirements.RetainTimeline = true;
	timeline.Complete = [](auto observations, CheckResult &output) {
		output.Record("bounded history", observations.size() == 1);
	};
	ScenarioOptions options;
	options.MaximumRetainedSamples = 1;
	options.MaximumRetainedBytes = sizeof(int) + 4;
	const auto report = RunScenario(Key(), {timeline, Check()}, Driver(setups, finishes), options);
	CHECK_FALSE(report.Checks[0].Passed);
	CHECK(report.Checks[1].Passed);
	CHECK(report.Metrics.DroppedSamples == 1);
	CHECK(report.Metrics.DroppedBytes == sizeof(int) + 4);
	CHECK(report.Metrics.PeakRetainedSamples == 1);
	CHECK(report.Metrics.PeakRetainedBytes == sizeof(int) + 4);
	options.MaximumRetainedSamples = 3;
	options.MaximumRetainedBytes = 0;
	const auto bytes = RunScenario(Key(), {timeline, Check()}, Driver(setups, finishes), options);
	CHECK(bytes.Metrics.DroppedSamples == 2);
	CHECK(bytes.Metrics.PeakRetainedBytes == 0);
	CHECK(bytes.Checks[1].Passed);
}

TEST_CASE("timeline payloads expire before the completed report escapes", "[scenario]") {
	std::weak_ptr<const int> lifetime;
	ScenarioCheck<std::shared_ptr<const int>> check;
	check.Name = "owned";
	check.SourceSuite = "engine.testing.scenario";
	check.Requirements.RetainTimeline = true;
	check.Complete = [](auto observations, CheckResult &output) {
		output.Record("owned value", observations.size() == 1 && *observations[0]->Payload == 5);
	};
	ScenarioDriver<std::shared_ptr<const int>> driver{.Run = [&](auto &session) {
		auto payload = std::make_shared<const int>(5);
		lifetime = payload;
		session.Publish({.Hook = "tick", .Payload = std::move(payload), .RetainedBytes = sizeof(int)});
	}};
	CHECK(RunScenario(Key(), {check}, driver).Passed());
	CHECK(lifetime.expired());
}

TEST_CASE("native exceptions cancellation timeout and cleanup failures run teardown once", "[scenario]") {
	for (const int phase : {0, 1, 2, 3}) {
		int finished = 0;
		std::stop_source cancellation;
		ScenarioOptions options;
		options.Cancellation = cancellation.get_token();
		ScenarioDriver<int> driver{
			.Prepare =
				[&](auto &) {
					if (phase == 0) throw std::runtime_error("prepare failed");
				},
			.Run =
				[&](auto &session) {
					if (phase == 1) throw std::runtime_error("native failed");
					if (phase == 2) cancellation.request_stop();
					session.Checkpoint();
				},
			.Finish = [&] { finished++; }
		};
		if (phase == 3) options.Timeout = std::chrono::milliseconds(0);
		const auto report = RunScenario(Key(), {Check()}, driver, options);
		CHECK_FALSE(report.Passed());
		CHECK_FALSE(report.Failures.empty());
		CHECK(finished == 1);
	}
	const auto cleanup = RunScenario<int>(
		Key(),
		{Check()},
		{.Run = [](auto &session) { session.Publish({.Hook = "tick", .Payload = 1}); },
		 .Finish = [] { throw std::runtime_error("cleanup failed"); }}
	);
	CHECK_FALSE(cleanup.Passed());
	CHECK(cleanup.Failures[0] == "cleanup: cleanup failed");
}

TEST_CASE("serial and pooled evaluation preserve ordered findings", "[scenario]") {
	Pool pool;
	int setups = 0;
	int finishes = 0;
	ScenarioOptions serial;
	serial.Context = engine::parallel::JobContext::Serial;
	const auto inlineReport =
		RunScenario(Key(), {Check("one"), Check("two")}, Driver(setups, finishes), serial);
	const auto pooled = RunScenario(Key(), {Check("one"), Check("two")}, Driver(setups, finishes));
	CHECK(inlineReport.Passed());
	CHECK(pooled.Passed());
	CHECK(inlineReport.Checks == pooled.Checks);
}

TEST_CASE("invalid declarations and oversized publications cannot produce success", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	auto invalid = Check();
	invalid.Requirements.Hooks[0].MinimumSamples = 0;
	CHECK_FALSE(RunScenario(Key(), {invalid}, Driver(setups, finishes)).Passed());
	CHECK(setups == 0);
	CHECK(finishes == 1);
	ScenarioOptions options;
	options.Context = engine::parallel::JobContext::Processed;
	CHECK_FALSE(RunScenario(Key(), {Check()}, Driver(setups, finishes), options).Passed());
	options.Context = engine::parallel::JobContext::Serial;
	options.MaximumObservationBytes = sizeof(int) - 1;
	CHECK_FALSE(RunScenario(Key(), {Check()}, Driver(setups, finishes), options).Passed());
	CHECK(finishes == 3);
	CHECK(
		engine::testing::detail::ScenarioSum(std::numeric_limits<size_t>::max(), 1) ==
		std::numeric_limits<size_t>::max()
	);
}

TEST_CASE("failed runs are retried and owner affinity is enforced", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	ScenarioCache<int> cache;
	auto bad = Check();
	bad.Observe = [](const Observation<int> &, CheckResult &output) { output.Record("bad", false); };
	CHECK_FALSE(cache.Run(Key(), {bad}, Driver(setups, finishes)).Passed());
	CHECK(cache.Size() == 0);
	CHECK(cache.Run(Key(), {Check()}, Driver(setups, finishes)).Passed());
	CHECK(setups == 2);
	bool refused = false;
	ScenarioDriver<int> driver{.Run = [&](auto &session) {
		std::thread other([&] {
			try {
				session.Checkpoint();
			} catch (const std::logic_error &) {
				refused = true;
			}
		});
		other.join();
		session.Publish({.Hook = "tick", .Payload = 1});
	}};
	CHECK(RunScenario(Key(), {Check()}, driver).Passed());
	CHECK(refused);
}

TEST_CASE("completion sees only matching retained hooks and isolates timeline gaps", "[scenario]") {
	auto ticks = Check("ticks", "tick");
	ticks.Requirements.RetainTimeline = true;
	ticks.Complete = [](auto observations, CheckResult &output) {
		output.Record("tick history", observations.size() == 1 && observations[0]->Hook == "tick");
	};
	auto physics = Check("physics", "physics");
	physics.Requirements.RetainTimeline = true;
	physics.Complete = [](auto observations, CheckResult &output) {
		output.Record("physics history", observations.size() == 1 && observations[0]->Hook == "physics");
	};
	auto stream = Check("stream");
	stream.Complete = [](auto observations, CheckResult &output) {
		output.Record("no undeclared retention", observations.empty());
	};
	ScenarioOptions options;
	options.MaximumRetainedSamples = 2;
	ScenarioDriver<int> driver;
	driver.Run = [](auto &session) {
		session.Publish({.Hook = "tick", .Payload = 1});
		session.Publish({.Hook = "physics", .Payload = 2});
		session.Publish({.Hook = "tick", .Payload = 3});
	};
	const auto report = RunScenario(Key(), {ticks, physics, stream}, driver, options);
	CHECK_FALSE(report.Checks[0].Passed);
	CHECK(report.Checks[1].Passed);
	CHECK(report.Checks[2].Passed);
	CHECK(report.Checks[0].Samples == 2);
	CHECK(report.Checks[1].Samples == 1);
	CHECK(report.Checks[2].Samples == 2);
	CHECK(report.Metrics.DroppedSamples == 1);
}

TEST_CASE("bounded findings preserve failed and overflow evidence", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	auto check = Check();
	check.Observe = [](const Observation<int> &, CheckResult &output) {
		output.Record("ok", true, "yes");
		output.Record("bad", false, "hidden after cap");
	};
	ScenarioOptions options;
	options.MaximumFindingsPerCheck = 1;
	options.MaximumFindingBytesPerCheck = 5;
	const auto report = RunScenario(Key(), {check}, Driver(setups, finishes), options);
	CHECK_FALSE(report.Passed());
	REQUIRE(report.Checks[0].Findings.size() == 1);
	CHECK(report.Checks[0].Findings[0].Passed);
	CHECK(report.Checks[0].DroppedFindings == 3);
	CHECK(report.Checks[0].DroppedFindingBytes == 43);
	CHECK(report.Checks[0].FindingBytes() == 5);
	CHECK(report.Checks[0].Samples == 2);
	options.MaximumFindingsPerCheck = 100;
	options.MaximumFindingBytesPerCheck = 0;
	const auto noText = RunScenario(Key(), {check}, Driver(setups, finishes), options);
	CHECK_FALSE(noText.Passed());
	CHECK(noText.Checks[0].Findings.empty());
	CHECK(noText.Checks[0].DroppedFindings == 4);
}

TEST_CASE("diagnostic and identity text cannot escape their declared bounds", "[scenario]") {
	ScenarioOptions options;
	options.MaximumFailureBytes = 8;
	ScenarioDriver<int> driver;
	driver.Run = [](auto &) { throw std::runtime_error(std::string(1024, 'x')); };
	driver.Finish = [] { throw std::runtime_error(std::string(2048, 'y')); };
	const auto report = RunScenario(Key(), {Check()}, driver, options);
	CHECK_FALSE(report.Passed());
	CHECK(report.Failures.empty());
	CHECK(report.DroppedFailures == 2);
	CHECK(report.DroppedFailureBytes == 1024 + 2048 + 9);
	options.MaximumIdentityBytes = 4;
	const auto refused = RunScenario(Key(), {Check()}, driver, options);
	CHECK_FALSE(refused.Passed());
	CHECK(refused.Key.Scene.empty());
	CHECK(refused.Checks.empty());
	CHECK(refused.Metrics.SetupCount == 0);
	options.MaximumIdentityBytes = 4096;
	options.MaximumObservationBytes = sizeof(int) + 3;
	driver.Finish = {};
	driver.Run = [](auto &session) { session.Publish({.Hook = "tick", .Payload = 1}); };
	const auto metadata = RunScenario(Key(), {Check()}, driver, options);
	CHECK_FALSE(metadata.Passed());
	CHECK(metadata.Metrics.PublishedSamples == 0);
}

TEST_CASE("cache byte bounds include report text identities and deterministic FIFO eviction", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	ScenarioCache<int> probe;
	REQUIRE(probe.Run(Key(), {Check()}, Driver(setups, finishes)).Passed());
	const size_t entryBytes = probe.Bytes();
	CHECK(entryBytes > sizeof(ScenarioReport));
	ScenarioCache<int> cache(8, entryBytes * 2 - 1);
	REQUIRE(cache.Run(Key(), {Check()}, Driver(setups, finishes)).Passed());
	auto secondKey = Key();
	secondKey.Seed = 1;
	REQUIRE(cache.Run(secondKey, {Check()}, Driver(setups, finishes)).Passed());
	CHECK(cache.Size() == 1);
	CHECK(cache.Bytes() == entryBytes);
	CHECK(cache.Run(secondKey, {Check()}, Driver(setups, finishes)).Reused);
	CHECK_FALSE(cache.Run(Key(), {Check()}, Driver(setups, finishes)).Reused);
	ScenarioCache<int> tiny(8, 1);
	CHECK(tiny.Run(Key(), {Check()}, Driver(setups, finishes)).Passed());
	CHECK(tiny.Size() == 0);
	CHECK(tiny.Bytes() == 0);
	cache.Clear();
	CHECK(cache.Bytes() == 0);
}

TEST_CASE("cache validates revisions declarations and new budgets before reuse", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	ScenarioCache<int> cache;
	REQUIRE(cache.Run(Key(), {Check()}, Driver(setups, finishes)).Passed());
	auto noRevision = Key();
	noRevision.CheckSet.clear();
	CHECK_FALSE(cache.Run(noRevision, {Check()}, Driver(setups, finishes)).Passed());
	CHECK(setups == 1);
	auto revision = Key();
	revision.CheckSet = "checks-v2";
	auto revisedCheck = Check();
	revisedCheck.Observe = [](const Observation<int> &, CheckResult &output) {
		output.Record("revised callback", true);
	};
	const auto revised = cache.Run(revision, {revisedCheck}, Driver(setups, finishes));
	CHECK_FALSE(revised.Reused);
	REQUIRE_FALSE(revised.Checks[0].Findings.empty());
	CHECK(revised.Checks[0].Findings[0].Property == "revised callback");
	CHECK(setups == 2);
	CHECK_FALSE(cache.Run(Key(), {Check()}, {}).Passed());
	ScenarioOptions options;
	options.MaximumFindingsPerCheck = 1;
	CHECK_FALSE(cache.Run(Key(), {Check()}, Driver(setups, finishes), options).Passed());
	options = {};
	options.MaximumFindingBytesPerCheck = 10;
	CHECK_FALSE(cache.Run(Key(), {Check()}, Driver(setups, finishes), options).Passed());
	options = {};
	options.MaximumIdentityBytes = 1024;
	CHECK_FALSE(cache.Run(Key(), {Check()}, Driver(setups, finishes), options).Passed());
	options = {};
	options.MaximumFailureBytes = 512;
	CHECK_FALSE(cache.Run(Key(), {Check()}, Driver(setups, finishes), options).Passed());
	CHECK(setups == 2);
}

TEST_CASE("all cache access stays on its original owner", "[scenario]") {
	ScenarioCache<int> cache;
	std::array<bool, 4> refused{};
	std::thread other([&] {
		try {
			static_cast<void>(cache.Run(Key(), {Check()}, {}));
		} catch (const std::logic_error &) {
			refused[0] = true;
		}
		try {
			cache.Clear();
		} catch (const std::logic_error &) {
			refused[1] = true;
		}
		try {
			static_cast<void>(cache.Size());
		} catch (const std::logic_error &) {
			refused[2] = true;
		}
		try {
			static_cast<void>(cache.Bytes());
		} catch (const std::logic_error &) {
			refused[3] = true;
		}
	});
	other.join();
	CHECK(std::all_of(refused.begin(), refused.end(), [](bool rejected) { return rejected; }));
	CHECK(cache.Size() == 0);
}

TEST_CASE("documented inline pool fallback preserves results", "[scenario]") {
	Pool pool;
	int setups = 0;
	int finishes = 0;
	const auto normal = RunScenario(Key(), {Check("one"), Check("two")}, Driver(setups, finishes));
	engine::parallel::SetForceSerialCompute(true);
	const auto forced = RunScenario(Key(), {Check("one"), Check("two")}, Driver(setups, finishes));
	engine::parallel::SetForceSerialCompute(false);
	engine::parallel::Jobs::Stop();
	const auto absent = RunScenario(Key(), {Check("one"), Check("two")}, Driver(setups, finishes));
	CHECK(normal.Checks == forced.Checks);
	CHECK(normal.Checks == absent.Checks);
	CHECK(normal.Passed());
	CHECK(forced.Passed());
	CHECK(absent.Passed());
}

TEST_CASE("cache admission and hits never copy transferred callback captures", "[scenario]") {
	int setups = 0;
	int finishes = 0;
	ScenarioCache<int> cache;
	auto copies = std::make_shared<CallbackCopies>();
	auto ownedChecks = [&] {
		auto check = Check();
		check.Observe = CopyCheckedCallback(copies);
		std::vector<ScenarioCheck<int>> checks;
		checks.push_back(std::move(check));
		return checks;
	};
	// Once supplied to std::function by move, every further callback copy is
	// observable and throws. Input ownership is transferred into each call.
	copies->Throw = true;
	auto refusedKey = Key();
	refusedKey.Configuration.assign(1024 * 1024, 'x');
	ScenarioOptions noIdentity;
	noIdentity.MaximumIdentityBytes = 0;
	const auto refused =
		cache.Run(std::move(refusedKey), ownedChecks(), Driver(setups, finishes), noIdentity);
	CHECK_FALSE(refused.Passed());
	CHECK(refused.Key.Configuration.empty());
	CHECK(setups == 0);
	CHECK(copies->Count == 0);
	const auto fresh = cache.Run(Key(), ownedChecks(), Driver(setups, finishes));
	REQUIRE(fresh.Passed());
	CHECK_FALSE(fresh.Reused);
	CHECK(setups == 1);
	CHECK(copies->Count == 0);
	const auto hit = cache.Run(Key(), ownedChecks(), Driver(setups, finishes));
	CHECK(hit.Passed());
	CHECK(hit.Reused);
	CHECK(setups == 1);
	CHECK(copies->Count == 0);
}
