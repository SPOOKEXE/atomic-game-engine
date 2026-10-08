#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.argument-tags")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr size_t CALLS = 64;
	constexpr uint64_t BUDGET = 1024 * 1024;
	void Check(bool valid, const char *message) {
		if (!valid) throw std::runtime_error(message);
	}
	struct Fixture {
		unsigned Kind;
		Node Owner;
		SourceArgumentHost Provider;
		EvaluationRequest Request;
		std::array<std::array<AuthoredValue, 3>, 2> Controls;
		std::array<HostNodeCapture, CALLS> Captures;
		std::string Failure;
		uint64_t RetainedBytes = 0, CaptureBytes = 0, Checksum = 0;
		explicit Fixture(unsigned kind) : Kind(kind) {
			Owner.Id = "argument";
			Owner.Type = "pc.argument";
			const std::array<Value, 6> tags{
				std::string{"x_y"},
				true,
				double{73},
				double{.125},
				double{1e21},
				int64_t{std::numeric_limits<int64_t>::max()}
			};
			const std::array<std::string, 6> keys{"x y", "1", "73", "0.13", "1e+21", "9223372036854775807"};
			std::vector<AuthoredValue> table;
			for (size_t index = 0; index < 63; ++index)
				table.push_back({"unused" + std::to_string(index), int64_t{-1}});
			table.push_back({keys.at(kind), std::string{"91.25"}});
			Diagnostic diagnostic;
			Check(
				Provider.Prepare(table, BUDGET, diagnostic) == Status::Ok, "argument provider table refused"
			);
			RetainedBytes = Provider.RetainedBytes();
			for (size_t mode = 0; mode < 2; ++mode)
				Controls[mode] = {
					{{"tag", tags.at(kind)},
					 {"type", EnumValue{int64_t(mode)}},
					 {"default_value", std::string{"fallback"}}}
				};
		}
		void Run() {
			for (size_t index = 0; index < CALLS; ++index) {
				const HostNodeInvocation invocation{Owner, Request, Controls[index % 2], {}, BUDGET};
				Check(
					Provider.Capture(invocation, Captures[index], Failure),
					"argument provider capture refused"
				);
			}
			CaptureBytes = 0;
			for (const auto &capture : Captures) {
				const auto bytes = HostCaptureRetainedPayloadBytes(capture);
				Check(bytes.has_value(), "argument receipt byte measurement refused");
				CaptureBytes += *bytes;
			}
			Metrics::Count("imagegraph.argument_tags.captures", CALLS);
			Metrics::SetGauge("imagegraph.argument_tags.table_bytes", double(RetainedBytes));
			Metrics::SetGauge("imagegraph.argument_tags.capture_bytes", double(CaptureBytes));
		}
		void Verify() {
			uint64_t hash = 14695981039346656037ull;
			const auto word = [&](uint64_t value) {
				for (size_t shift = 0; shift < 64; shift += 8)
					hash = (hash ^ ((value >> shift) & 255)) * 1099511628211ull;
			};
			for (size_t index = 0; index < CALLS; ++index) {
				const auto &capture = Captures[index];
				const auto &controls = Controls[index % 2];
				Check(
					capture.Authored == Owner && capture.Tick == 0 && capture.Subframe == 0 &&
						!capture.NegativeFrame && capture.State == HostCaptureState::Recorded &&
						capture.Failure.empty() &&
						capture.Inputs == std::vector<AuthoredValue>(controls.begin(), controls.end()),
					"argument provider changed receipt identity or controls"
				);
				const Value expected = index % 2 ? Value{double{91.25}} : Value{std::string{"91.25"}};
				Check(
					capture.Outputs.size() == 1 && capture.Outputs[0].Port == "value" &&
						capture.Outputs[0].Data == expected,
					"argument lookup or Number conversion changed"
				);
				word(index);
				word(Kind);
				word(expected.index());
				if (index % 2)
					word(std::bit_cast<uint64_t>(double{91.25}));
				else
					for (unsigned char byte : std::string_view{"91.25"})
						word(byte);
			}
			if (Checksum) Check(Checksum == hash, "argument repeated checksum changed");
			Checksum = hash;
			engine::testing::Consume(hash);
		}
	};
	struct Reading {
		float Milliseconds = 0, Unmarked = 0;
		engine::core::HeapTotals Before{}, After{};
		std::vector<std::pair<std::string, engine::core::FrameSpan>> Spans;
		uint64_t RetainedBytes = 0, CaptureBytes = 0;
	};
	struct Profile {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		explicit Profile(unsigned kind) : Graph(kind) {}
		void Measure() {
			Check(Count < Readings.size(), "argument tags profile supports one to five samples");
			const bool previousEnabled = FrameGraph::IsEnabled();
			FrameGraph::SetEnabled(true);
			Metrics::Drain();
			auto &reading = Readings[Count];
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraph.argument_tags.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				FrameGraph::SetEnabled(previousEnabled);
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Milliseconds = FrameGraph::FrameMilliseconds();
			reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
			reading.RetainedBytes = Graph.RetainedBytes;
			reading.CaptureBytes = Graph.CaptureBytes;
			Check(
				!FrameGraph::Dropped() && reading.Before.DroppedScopes == reading.After.DroppedScopes,
				"argument tags profile dropped scopes"
			);
			size_t captures = 0;
			for (const auto &span : FrameGraph::Spans()) {
				captures += span.Name == "imagegraph source argument capture";
				Check(!span.Reported, "argument CPU profile contains reported worker time");
				reading.Spans.emplace_back(std::string(span.Name), span);
			}
			Check(captures == CALLS, "argument profile missed provider capture calls");
			Metrics::Drain();
			FrameGraph::SetEnabled(previousEnabled);
			Graph.Verify();
			if (Count)
				Check(
					reading.RetainedBytes == Readings[0].RetainedBytes &&
						reading.CaptureBytes == Readings[0].CaptureBytes,
					"argument repeated cycle changed retained state bounds"
				);
			++Count;
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_ARGUMENT_PRESET");
			for (size_t index = 0; index < Count; ++index) {
				const auto &r = Readings[index];
				std::printf(
					"# argument-profile preset=%s backend=cpu variant=%u captures=64 call=%zu warmup=%d "
					"state_fnv=%llu table_bytes=%llu capture_bytes=%llu budget_bytes=%llu owner_ms=%.6f "
					"unmarked_ms=%.6f heap_compiled=%d allocated_bytes=%llu allocated_blocks=%llu "
					"process_live_bytes=%lld process_peak_bytes=%lld interval_live_bytes_delta=%lld "
					"profiler_overhead_bytes=%lld\n",
					preset ? preset : "unreported",
					Graph.Kind,
					index + 1,
					index < 8,
					(unsigned long long)Graph.Checksum,
					(unsigned long long)r.RetainedBytes,
					(unsigned long long)r.CaptureBytes,
					(unsigned long long)BUDGET,
					r.Milliseconds,
					r.Unmarked,
					HeapProfile::IsCompiledIn(),
					(unsigned long long)(r.After.TotalBytes - r.Before.TotalBytes),
					(unsigned long long)(r.After.TotalBlocks - r.Before.TotalBlocks),
					(long long)r.After.LiveBytes,
					(long long)r.After.PeakBytes,
					(long long)(r.After.LiveBytes - r.Before.LiveBytes),
					(long long)r.After.OverheadBytes
				);
				for (size_t spanIndex = 0; spanIndex < r.Spans.size(); ++spanIndex) {
					const auto &[name, span] = r.Spans[spanIndex];
					std::printf(
						"# argument-span call=%zu index=%zu parent=%u depth=%u inclusive_ms=%.6f "
						"self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						index + 1,
						spanIndex,
						span.Parent,
						span.Depth,
						span.Milliseconds,
						span.SelfMilliseconds,
						span.IdleMilliseconds,
						span.Reported,
						name.c_str()
					);
				}
			}
		}
	};
}
BENCH("CPU Argument String fallback space tags sixty-four provider captures", 1) {
	static Profile p(0);
	p.Measure();
}
BENCH("CPU Argument Boolean tags sixty-four provider captures", 1) {
	static Profile p(1);
	p.Measure();
}
BENCH("CPU Argument Int32 Number tags sixty-four provider captures", 1) {
	static Profile p(2);
	p.Measure();
}
BENCH("CPU Argument Fractional tie tags sixty-four provider captures", 1) {
	static Profile p(3);
	p.Measure();
}
BENCH("CPU Argument Large finite Number tags sixty-four provider captures", 1) {
	static Profile p(4);
	p.Measure();
}
BENCH("CPU Argument Integer Long tags sixty-four provider captures", 1) {
	static Profile p(5);
	p.Measure();
}
