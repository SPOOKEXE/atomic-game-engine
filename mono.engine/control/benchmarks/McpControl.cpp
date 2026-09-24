// Release cost of MCP tool discovery, dispatch, and optional capture-hook lifetime.

#include <engine/control/HookRegistry.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataCapture.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Name.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.control.bench.mcp-control")

namespace {
	using Clock = std::chrono::steady_clock;
	using Nanoseconds = std::chrono::nanoseconds;
	using namespace engine;
	using script::DataCaptureBridge;
	using script::DataCaptureBridgeCapabilities;
	using script::DataCaptureBridgePoll;
	using script::DataCaptureBridgeRequest;

	constexpr size_t OPERATIONS_PER_BATCH = 16;
	constexpr size_t WARMUP_OPERATIONS = 8;
	constexpr size_t CAPTURE_PAYLOAD_BYTES = 64 * 1024;

	struct CaptureState {
		uint64_t NextTicket = 1;
		uint64_t Ticket = 0;
		uint32_t DrainPumps = 0;
		bool Pending = false;
		bool CancelRequested = false;
		bool Terminal = false;
		std::vector<std::byte> Payload;
	};

	class FixtureBridge final : public DataCaptureBridge {
	  public:
		DataCaptureBridgeCapabilities Capabilities() const override {
			return {
				.Available = true,
				.Channels = {"rgb_linear_hdr"},
				.StorageProfiles = {"lossless"},
				.TrainingCompactLimitations = {},
				.NoiseLimitations = {},
				.HookRecords = {},
				.MaximumCaptureTickets = 6,
				.MaximumPendingPumps = 600,
				.Detail = "benchmark bridge",
			};
		}

		bool Queue(
			std::string_view, const DataCaptureBridgeRequest &, uint64_t &ticket, std::string &detail
		) override {
			if (State.Pending || State.Terminal) {
				detail = "benchmark bridge has retained work";
				return false;
			}
			ticket = State.NextTicket++;
			State.Ticket = ticket;
			State.Pending = true;
			State.CancelRequested = false;
			State.DrainPumps = 0;
			State.Payload.assign(CAPTURE_PAYLOAD_BYTES, std::byte{0x5a});
			return true;
		}

		bool
		Poll(std::string_view, uint64_t ticket, DataCaptureBridgePoll &poll, std::string &detail) override {
			if (ticket != State.Ticket || (!State.Pending && !State.Terminal)) {
				detail = "unknown benchmark ticket";
				return false;
			}
			poll.Status = State.Pending ? "pending" : "ready";
			return true;
		}

		bool ReadPlane(
			std::string_view,
			uint64_t ticket,
			std::string_view,
			size_t offset,
			size_t maximumBytes,
			std::vector<std::byte> &bytes,
			std::string &detail
		) override {
			if (ticket != State.Ticket || !State.Terminal || offset > State.Payload.size()) {
				detail = "benchmark ticket is not readable";
				return false;
			}
			const size_t count = std::min(maximumBytes, State.Payload.size() - offset);
			bytes.assign(State.Payload.begin() + offset, State.Payload.begin() + offset + count);
			return true;
		}

		bool Release(std::string_view, uint64_t ticket, std::string &detail) override {
			if (ticket != State.Ticket || !State.Terminal) {
				detail = "benchmark ticket is not terminal";
				return false;
			}
			std::vector<std::byte>().swap(State.Payload);
			State.Terminal = false;
			++ReleasedTickets;
			return true;
		}

		void Cancel(std::string_view, uint64_t ticket) override {
			if (ticket == State.Ticket && State.Pending) State.CancelRequested = true;
		}

		void CancelPending() {
			if (State.Pending) Cancel("mcp-bench-world", State.Ticket);
		}

		void Pump() {
			if (!State.Pending || !State.CancelRequested) return;
			if (++State.DrainPumps < 3) return;
			State.Pending = false;
			State.CancelRequested = false;
			State.Terminal = true;
		}

		bool HasOutstanding() const {
			return State.Pending || State.Terminal;
		}

		CaptureState State;
		uint64_t ReleasedTickets = 0;
	};

	struct Distribution {
		std::vector<uint64_t> Nanoseconds;
		std::vector<uint64_t> AllocationBlocks;
		bool AllocationTrackingAvailable = true;

		void Reserve(size_t count) {
			Nanoseconds.reserve(count);
			AllocationBlocks.reserve(count);
		}

		template <class Action> void Measure(Action &&action) {
			const core::HeapTotals before = core::HeapProfile::Totals();
			const auto started = Clock::now();
			action();
			const auto elapsed =
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
			const core::HeapTotals after = core::HeapProfile::Totals();
			AllocationTrackingAvailable &= before.Nodes != 0 && after.Nodes != 0;
			Nanoseconds.push_back(static_cast<uint64_t>(elapsed));
			AllocationBlocks.push_back(after.TotalBlocks - std::min(after.TotalBlocks, before.TotalBlocks));
		}
	};

	struct Report {
		Distribution ToolsList;
		Distribution ToolDispatch;
		Distribution CaptureSubmit;
		Distribution HookActivate;
		Distribution HookDrain;
	};

	template <class Registration> void KeepCaptureCleanupDuringDrain(Registration &registration) {
		if constexpr (requires(Registration &candidate) { candidate.KeepToolDuringDrain(std::string{}); }) {
			for (const char *name : {"poll_capture", "get_resource", "release_capture", "cancel_capture"})
				registration.KeepToolDuringDrain(name);
		}
	}

	struct Fixture {
		world::Universe Universe;
		world::DataFactorySession Session{Universe};
		std::shared_ptr<FixtureBridge> Bridge = std::make_shared<FixtureBridge>();
		control::Surface Surface{"mcp-bench", "MCP control benchmark."};
		control::HookLease Lease;
		Report Measurements;
		std::string Failure;
		size_t NextCaptureOperation = 0;
		const std::string ListRequest = R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})";
		const std::string DispatchRequest =
			R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"capture","arguments":{}}})";

		Fixture() {
			world::WorldSettings settings;
			settings.Name = core::Name("mcp-bench-world");
			Universe.Create(settings);
			Measurements.ToolsList.Reserve(4096);
			Measurements.ToolDispatch.Reserve(4096);
			Measurements.CaptureSubmit.Reserve(4096);
			Measurements.HookActivate.Reserve(4096);
			Measurements.HookDrain.Reserve(4096);
		}

		std::string CaptureRequest() {
			const auto current = Session.Inspect("mcp-bench-world");
			return nlohmann::json{
				{"jsonrpc", "2.0"},
				{"id", 1},
				{"method", "tools/call"},
				{"params",
				 {{"name", "capture"},
				  {"arguments",
				   {{"instance_id", "mcp-bench-world"},
					{"snapshot_id", "mcp-bench-snapshot"},
					{"pipeline", "default_pbr"},
					{"capture_node", "capture"},
					{"view_slot", 0},
					{"channels", nlohmann::json::array({"rgb_linear_hdr"})},
					{"temporal_history", "preserve"},
					{"operation_id", "mcp-bench-capture-" + std::to_string(NextCaptureOperation++)},
					{"expected_tick", current.Clock.Tick},
					{"expected_world_epoch", current.WorldEpoch},
					{"expected_world_version", current.WorldVersion}}}}}
			}.dump();
		}

		control::HookLease Activate(std::string_view hookId = "client.data-capture") {
			return Surface.ActivateHook(
				{.Id = std::string(hookId),
				 .Revision = "v1",
				 .Purpose = "Owns benchmark capture tickets.",
				 .Dependencies = {},
				 .Limits = {{.Name = "maximum_in_flight", .Maximum = 6}}},
				[this, hookId = std::string(hookId)](control::HookRegistration &registration) {
					KeepCaptureCleanupDuringDrain(registration);
					registration.SetDrain([bridge = Bridge] {
						bridge->CancelPending();
						bridge->Pump();
						return !bridge->HasOutstanding();
					});
					registration.SetRelease([this] { Surface.SetDataCaptureAvailabilityProvider({}); });
					Surface.AddDataCaptureTools(Session, Bridge);
					Surface.SetDataCaptureAvailabilityProvider([this, hookId] {
						for (const control::HookStatus &hook : Surface.Hooks().Active())
							if (hook.Descriptor.Id == hookId && hook.State == control::HookState::Active) {
								const auto capabilities = Bridge->Capabilities();
								return control::DataCaptureAvailability{
									.Available = capabilities.Available,
									.Channels = capabilities.Channels,
									.Detail = capabilities.Detail,
								};
							}
						return control::DataCaptureAvailability{};
					});
				},
				Failure
			);
		}

		void EnsureActive() {
			if (Lease.IsValid()) return;
			Lease = Activate();
			if (!Failure.empty() || !Lease.IsValid())
				throw std::runtime_error("MCP capture hook activation failed");
		}

		void FinishWithoutTickets() {
			Lease.Close();
			Surface.PumpHooks();
			if (Lease.IsValid()) throw std::runtime_error("capture hook did not drain without tickets");
		}
	};

	void PrintReport();

	Fixture &BenchFixture() {
		static Fixture fixture;
		static const bool reportRegistered = [] {
			std::atexit(PrintReport);
			return true;
		}();
		(void)reportRegistered;
		return fixture;
	}

	uint64_t Percentile(std::vector<uint64_t> values, size_t percentile) {
		if (values.empty()) return 0;
		std::ranges::sort(values);
		return values[(percentile * (values.size() - 1) + 99) / 100];
	}

	void PrintDistribution(std::string_view name, const Distribution &values) {
		const uint64_t durationMaximum =
			values.Nanoseconds.empty() ? 0 : *std::ranges::max_element(values.Nanoseconds);
		const uint64_t allocationMaximum =
			values.AllocationBlocks.empty() ? 0 : *std::ranges::max_element(values.AllocationBlocks);
		std::cout << "mcp-control-profile op=" << name << " samples=" << values.Nanoseconds.size()
				  << " ns_p50=" << Percentile(values.Nanoseconds, 50)
				  << " ns_p99=" << Percentile(values.Nanoseconds, 99) << " ns_max=" << durationMaximum
				  << " allocation_blocks_p50=" << Percentile(values.AllocationBlocks, 50)
				  << " allocation_blocks_p99=" << Percentile(values.AllocationBlocks, 99)
				  << " allocation_blocks_max=" << allocationMaximum << " allocation_tracking="
				  << (values.AllocationTrackingAvailable ? "available" : "unavailable")
				  << (values.AllocationTrackingAvailable ? "" : " reason=heap_profile_disabled") << '\n';
	}

	void PrintReport() {
		Fixture &fixture = BenchFixture();
		std::cout << "mcp-control-profile preset=bench optimization=release"
				  << " calls_per_batch=" << OPERATIONS_PER_BATCH << '\n';
		PrintDistribution("tools_list", fixture.Measurements.ToolsList);
		PrintDistribution("tool_dispatch", fixture.Measurements.ToolDispatch);
		PrintDistribution("capture_submit", fixture.Measurements.CaptureSubmit);
		PrintDistribution("hook_activate", fixture.Measurements.HookActivate);
		PrintDistribution("hook_drain", fixture.Measurements.HookDrain);
	}

	void RunToolsList() {
		Fixture &fixture = BenchFixture();
		fixture.EnsureActive();
		for (size_t index = 0; index < WARMUP_OPERATIONS; ++index)
			if (fixture.Surface.Answer(fixture.ListRequest).empty())
				throw std::runtime_error("tools/list warmup returned no response");
		for (size_t index = 0; index < OPERATIONS_PER_BATCH; ++index)
			fixture.Measurements.ToolsList.Measure([&] {
				if (fixture.Surface.Answer(fixture.ListRequest).empty())
					throw std::runtime_error("tools/list returned no response");
			});
	}

	void RunToolDispatch() {
		Fixture &fixture = BenchFixture();
		fixture.EnsureActive();
		for (size_t index = 0; index < WARMUP_OPERATIONS; ++index)
			if (fixture.Surface.Answer(fixture.DispatchRequest).empty())
				throw std::runtime_error("tools/call warmup returned no response");
		for (size_t index = 0; index < OPERATIONS_PER_BATCH; ++index)
			fixture.Measurements.ToolDispatch.Measure([&] {
				if (fixture.Surface.Answer(fixture.DispatchRequest).empty())
					throw std::runtime_error("tools/call returned no response");
			});
	}

	void RunCaptureSubmission() {
		Fixture &fixture = BenchFixture();
		fixture.EnsureActive();
		const auto submit = [&](bool measured) {
			const std::string request = fixture.CaptureRequest();
			std::string reply;
			if (measured)
				fixture.Measurements.CaptureSubmit.Measure([&] { reply = fixture.Surface.Answer(request); });
			else
				reply = fixture.Surface.Answer(request);
			const nlohmann::json response = nlohmann::json::parse(reply);
			const nlohmann::json &result = response.at("result");
			if (result.value("isError", true))
				throw std::runtime_error("capture submission returned an MCP error result");
			const nlohmann::json content =
				nlohmann::json::parse(result.at("content").at(0).at("text").get<std::string>());
			if (content.value("status", "") != "queued")
				throw std::runtime_error("capture submission did not queue a ticket");
			const uint64_t ticket = content.at("ticket").get<uint64_t>();
			fixture.Bridge->CancelPending();
			for (size_t pump = 0; pump < 3; ++pump)
				fixture.Bridge->Pump();
			std::string detail;
			if (!fixture.Bridge->Release("mcp-bench-world", ticket, detail))
				throw std::runtime_error("capture submission cleanup failed: " + detail);
		};
		for (size_t index = 0; index < WARMUP_OPERATIONS; ++index)
			submit(false);
		for (size_t index = 0; index < OPERATIONS_PER_BATCH; ++index)
			submit(true);
	}

	void RunHookLifecycle() {
		Fixture &fixture = BenchFixture();
		if (fixture.Lease.IsValid()) fixture.FinishWithoutTickets();
		for (size_t index = 0; index < WARMUP_OPERATIONS; ++index) {
			control::HookLease lease = fixture.Activate("bench.capture-lifecycle");
			if (!lease.IsValid() || !fixture.Failure.empty())
				throw std::runtime_error("capture lifecycle warmup activation failed");
			lease.Close();
			fixture.Surface.PumpHooks();
			if (lease.IsValid()) throw std::runtime_error("capture lifecycle warmup did not drain");
		}
		for (size_t index = 0; index < OPERATIONS_PER_BATCH; ++index) {
			control::HookLease lease;
			fixture.Measurements.HookActivate.Measure([&] {
				lease = fixture.Activate("bench.capture-lifecycle");
				if (!lease.IsValid() || !fixture.Failure.empty())
					throw std::runtime_error("capture lifecycle hook activation failed");
			});
			fixture.Measurements.HookDrain.Measure([&] {
				lease.Close();
				fixture.Surface.PumpHooks();
				if (lease.IsValid()) throw std::runtime_error("capture lifecycle hook did not drain");
			});
		}
	}

	struct LivePoint {
		size_t Cycle = 0;
		int64_t LiveBytes = 0;
	};

	struct SoakReport {
		Distribution Activate;
		Distribution DisableToDraining;
		Distribution TerminalRelease;
		std::vector<LivePoint> Live;
		size_t Cycles = 0;
		uint64_t InitialReleasedTickets = 0;
		size_t DrainingChecks = 0;
		size_t RefusedSubmissions = 0;
		int64_t InitialLiveBytes = 0;
		int64_t FinalLiveBytes = 0;
		double LiveBytesSlopePerCycle = 0.0;
		double LiveBytesFit = 0.0;
		bool HeapTrackingAvailable = true;
		bool Started = false;
	};

	void PrintDistribution(std::string_view name, const Distribution &values);

	SoakReport &CaptureSoakReport() {
		static SoakReport report;
		return report;
	}

	void PrintSoakReport() {
		const SoakReport &report = CaptureSoakReport();
		std::cout << "mcp-capture-hook-soak preset=bench optimization=release"
				  << " adapter=control_data_capture_hook"
				  << " bridge=synthetic_ticket cycles=" << report.Cycles
				  << " retained_bytes_per_ticket=" << CAPTURE_PAYLOAD_BYTES << " released_tickets="
				  << BenchFixture().Bridge->ReleasedTickets - report.InitialReleasedTickets
				  << " draining_checks=" << report.DrainingChecks
				  << " refused_submissions=" << report.RefusedSubmissions
				  << " live_bytes_initial=" << report.InitialLiveBytes
				  << " live_bytes_final=" << report.FinalLiveBytes
				  << " live_bytes_slope_per_cycle=" << report.LiveBytesSlopePerCycle
				  << " live_bytes_fit_r2=" << report.LiveBytesFit
				  << " heap_tracking=" << (report.HeapTrackingAvailable ? "available" : "unavailable")
				  << (report.HeapTrackingAvailable ? "" : " reason=heap_profile_disabled")
				  << " note=synthetic_bridge_ticket_no_renderer_gpu_resources\n";
		PrintDistribution("capture_activate", report.Activate);
		PrintDistribution("capture_disable_to_draining", report.DisableToDraining);
		PrintDistribution("capture_terminal_release", report.TerminalRelease);
	}

	size_t SoakCycles() {
		const char *value = std::getenv("MONO_MCP_CAPTURE_SOAK_CYCLES");
		if (value == nullptr) return 128;
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(value, &end, 10);
		if (end == value || *end != '\0' || parsed == 0 || parsed > 10000)
			throw std::runtime_error("MONO_MCP_CAPTURE_SOAK_CYCLES must be in [1, 10000]");
		return static_cast<size_t>(parsed);
	}

	void MeasureCaptureSoak() {
		if (std::getenv("MONO_MCP_CAPTURE_SOAK") == nullptr) return;
		Fixture &fixture = BenchFixture();
		SoakReport &report = CaptureSoakReport();
		if (report.Started) return;
		const size_t cycles = SoakCycles();
		const size_t total = report.Cycles + cycles;
		const size_t pointsNeeded = (total + 15) / 16 + 2;
		report.Activate.Reserve(total);
		report.DisableToDraining.Reserve(total);
		report.TerminalRelease.Reserve(total);
		if (report.Live.capacity() < pointsNeeded) report.Live.reserve(pointsNeeded);
		if (!report.Started) {
			report.InitialReleasedTickets = fixture.Bridge->ReleasedTickets;
			const core::HeapTotals totals = core::HeapProfile::Totals();
			report.HeapTrackingAvailable = totals.Nodes != 0;
			report.InitialLiveBytes = totals.LiveBytes;
			report.Live.push_back({.Cycle = 0, .LiveBytes = report.InitialLiveBytes});
			report.Started = true;
		}
		for (size_t index = 0; index < cycles; ++index) {
			if (report.Cycles > report.Live.back().Cycle && report.Cycles % 16 == 0)
				report.Live.push_back(
					{.Cycle = report.Cycles, .LiveBytes = core::HeapProfile::Totals().LiveBytes}
				);
			control::HookLease lease;
			report.Activate.Measure([&] {
				lease = fixture.Activate("client.data-capture");
				if (!lease.IsValid() || !fixture.Failure.empty())
					throw std::runtime_error("capture soak hook activation failed");
			});
			DataCaptureBridgeRequest request;
			uint64_t ticket = 0;
			std::string detail;
			if (!fixture.Bridge->Queue("mcp-bench-world", request, ticket, detail))
				throw std::runtime_error("synthetic capture ticket queue failed");
			lease.Close();
			const core::HeapTotals drainBefore = core::HeapProfile::Totals();
			const auto disabledAt = Clock::now();
			fixture.Surface.PumpHooks();
			const std::vector<control::HookStatus> draining = fixture.Surface.Hooks().Active();
			if (draining.size() != 1 || draining.front().State != control::HookState::Draining ||
				fixture.Surface.Hooks().VisibleTool("capture") ||
				!fixture.Surface.Hooks().VisibleTool("release_capture"))
				throw std::runtime_error("capture hook did not deny submissions while draining");
			++report.DrainingChecks;
			report.DisableToDraining.Nanoseconds.push_back(
				static_cast<uint64_t>(
					std::chrono::duration_cast<Nanoseconds>(Clock::now() - disabledAt).count()
				)
			);
			const core::HeapTotals drainAfter = core::HeapProfile::Totals();
			report.DisableToDraining.AllocationTrackingAvailable &=
				drainBefore.Nodes != 0 && drainAfter.Nodes != 0;
			report.DisableToDraining.AllocationBlocks.push_back(
				drainAfter.TotalBlocks - std::min(drainAfter.TotalBlocks, drainBefore.TotalBlocks)
			);
			const uint64_t queueCount = fixture.Bridge->State.NextTicket;
			(void)fixture.Surface.Answer(fixture.DispatchRequest);
			if (fixture.Bridge->State.NextTicket == queueCount) ++report.RefusedSubmissions;
			fixture.Bridge->Pump();
			DataCaptureBridgePoll terminal;
			if (!fixture.Bridge->Poll("mcp-bench-world", ticket, terminal, detail) ||
				terminal.Status != "ready")
				throw std::runtime_error("capture soak ticket did not reach terminal state");
			fixture.Surface.PumpHooks();
			if (!lease.IsValid() || !fixture.Surface.Hooks().VisibleTool("release_capture"))
				throw std::runtime_error("capture soak lost terminal cleanup tool");
			const core::HeapTotals releaseBefore = core::HeapProfile::Totals();
			const auto releaseStarted = Clock::now();
			if (!fixture.Bridge->Release("mcp-bench-world", ticket, detail))
				throw std::runtime_error("capture soak terminal ticket release failed");
			fixture.Surface.PumpHooks();
			report.TerminalRelease.Nanoseconds.push_back(
				static_cast<uint64_t>(
					std::chrono::duration_cast<Nanoseconds>(Clock::now() - releaseStarted).count()
				)
			);
			const core::HeapTotals releaseAfter = core::HeapProfile::Totals();
			report.TerminalRelease.AllocationTrackingAvailable &=
				releaseBefore.Nodes != 0 && releaseAfter.Nodes != 0;
			report.TerminalRelease.AllocationBlocks.push_back(
				releaseAfter.TotalBlocks - std::min(releaseAfter.TotalBlocks, releaseBefore.TotalBlocks)
			);
			if (lease.IsValid() || fixture.Bridge->State.Pending || fixture.Bridge->State.Terminal ||
				!fixture.Bridge->State.Payload.empty())
				throw std::runtime_error("capture soak retained ticket resources after release");
			++report.Cycles;
		}
		if (report.Cycles > report.Live.back().Cycle)
			report.Live.push_back(
				{.Cycle = report.Cycles, .LiveBytes = core::HeapProfile::Totals().LiveBytes}
			);
		if (report.RefusedSubmissions != report.Cycles ||
			fixture.Bridge->ReleasedTickets - report.InitialReleasedTickets != report.Cycles)
			throw std::runtime_error("capture soak did not refuse or release every ticket exactly once");
		const core::HeapTotals finalTotals = core::HeapProfile::Totals();
		report.HeapTrackingAvailable &= finalTotals.Nodes != 0;
		report.FinalLiveBytes = finalTotals.LiveBytes;
		if (report.Live.size() >= 2) {
			double meanCycle = 0.0;
			double meanBytes = 0.0;
			for (const LivePoint &point : report.Live) {
				meanCycle += static_cast<double>(point.Cycle);
				meanBytes += static_cast<double>(point.LiveBytes);
			}
			meanCycle /= static_cast<double>(report.Live.size());
			meanBytes /= static_cast<double>(report.Live.size());
			double numerator = 0.0;
			double denominator = 0.0;
			double residualSquares = 0.0;
			for (const LivePoint &point : report.Live) {
				const double cycleDelta = static_cast<double>(point.Cycle) - meanCycle;
				const double byteDelta = static_cast<double>(point.LiveBytes) - meanBytes;
				numerator += cycleDelta * byteDelta;
				denominator += cycleDelta * cycleDelta;
			}
			report.LiveBytesSlopePerCycle = denominator == 0.0 ? 0.0 : numerator / denominator;
			for (const LivePoint &point : report.Live) {
				const double predicted = meanBytes + report.LiveBytesSlopePerCycle *
														 (static_cast<double>(point.Cycle) - meanCycle);
				const double residual = static_cast<double>(point.LiveBytes) - predicted;
				residualSquares += residual * residual;
			}
			double totalSquares = 0.0;
			for (const LivePoint &point : report.Live) {
				const double delta = static_cast<double>(point.LiveBytes) - meanBytes;
				totalSquares += delta * delta;
			}
			report.LiveBytesFit = totalSquares == 0.0 ? 0.0 : 1.0 - residualSquares / totalSquares;
		}
		static const bool registered = [] {
			std::atexit(PrintSoakReport);
			return true;
		}();
		(void)registered;
	}
}

BENCH("tools/list | active capture provider", 1) {
	RunToolsList();
}

BENCH("tools/call | capture validation dispatch", 1) {
	RunToolDispatch();
}

BENCH("tools/call | successful capture submission", 1) {
	RunCaptureSubmission();
}

BENCH("capture hook | activate then drain", 1) {
	RunHookLifecycle();
}

BENCH("capture hook | activate cancel drain and release retained ticket", 1) {
	MeasureCaptureSoak();
}
