#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

TEST_SUITE_ID("engine.imagegraph.bench.argument-colour")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::Metrics;
	constexpr uint64_t BUDGET = 1024 * 1024;
	void Check(bool valid, const char *message) {
		if (!valid) throw std::runtime_error(message);
	}
	struct Fixture {
		Document Graph;
		Plan Compiled;
		SourceArgumentHost Observed, Missing;
		std::array<std::array<AuthoredValue, 3>, 6> Controls;
		std::array<HostNodeCapture, 6> Captures;
		std::array<int64_t, 6> Expected{0, 0x00332211, 0x7f332211, 0x80332211, 0xff332211, 4294967295};
		uint64_t CaptureBytes = 0, OutputBytes = 0, Checksum = 0;
		Fixture() {
			const std::array<Colour, 6> colours{
				{{0, 0, 0, 0},
				 {17, 34, 51, 0},
				 {17, 34, 51, 127},
				 {17, 34, 51, 128},
				 {17, 34, 51, 255},
				 {255, 255, 255, 255}}
			};
			Graph.FormatVersion = 9;
			std::vector<AuthoredValue> table;
			for (size_t index = 0; index < colours.size(); ++index) {
				const std::string id = "argument" + std::to_string(index);
				const std::string colourId = "colour" + std::to_string(index);
				Graph.Nodes.push_back(
					{id, "pc.argument", "", {}, {{"tag", std::string{"missing"}}, {"type", EnumValue{1}}}}
				);
				Graph.Nodes.push_back({colourId, "pc.color", "", {}, {{"color", colours[index]}}});
				Graph.Links.push_back({colourId, "color", id, "default_value"});
				Graph.Outputs.push_back({id, id, "value"});
				Controls[index] = {{{"tag", id}, {"type", EnumValue{1}}, {"default_value", 0.}}};
				table.push_back({id, colours[index]});
			}
			Diagnostic diagnostic;
			Check(Compile(Graph, Compiled, diagnostic) == Status::Ok, "colour graph compile refused");
			Check(Observed.Prepare(table, BUDGET, diagnostic) == Status::Ok, "colour table refused");
			Check(Missing.Prepare({}, BUDGET, diagnostic) == Status::Ok, "empty table refused");
		}
		void Run() {
			CaptureBytes = OutputBytes = Checksum = 0;
			EvaluationRequest request;
			request.HostProvider = &Missing;
			Diagnostic diagnostic;
			std::string failure;
			for (size_t repeat = 0; repeat < 8; ++repeat)
				for (size_t index = 0; index < Controls.size(); ++index) {
					const auto &owner = Graph.Nodes[index * 2];
					const HostNodeInvocation invocation{owner, request, Controls[index], {}, BUDGET};
					Check(Observed.Capture(invocation, Captures[index], failure), "colour capture refused");
					Check(
						Captures[index].Outputs.size() == 1 &&
							Captures[index].Outputs[0].Data == Value{Expected[index]},
						"colour receipt changed"
					);
					const auto bytes = HostCaptureRetainedPayloadBytes(Captures[index]);
					Check(bytes.has_value(), "colour receipt measurement refused");
					CaptureBytes += *bytes;
					EvaluatedValue output;
					Check(
						EvaluateValue(Graph, Compiled, owner.Id, request, output, diagnostic) == Status::Ok,
						"linked colour evaluation refused"
					);
					Check(output.Data == Value{Expected[index]}, "linked colour value changed");
					const auto payload = ValueClonePayloadBytes(output.Data);
					Check(payload.has_value(), "colour output measurement refused");
					OutputBytes += *payload;
					Checksum += uint64_t(Expected[index]);
				}
			Metrics::Count("imagegraph.argument_colour.captures", 96);
			Metrics::Count("imagegraph.argument_colour.linked_evaluations", 48);
			Metrics::Count("imagegraph.argument_colour.published_receipt_bytes", double(CaptureBytes));
			Metrics::Count("imagegraph.argument_colour.output_payload_bytes", double(OutputBytes));
			engine::testing::Consume(Checksum);
		}
	};
}
BENCH("CPU Argument Colour six alpha variants provider capture and linked Color", 1) {
	static Fixture fixture;
	const bool previous = FrameGraph::IsEnabled();
	FrameGraph::SetEnabled(true);
	const auto heapBefore = engine::core::HeapProfile::Totals();
	FrameGraph::BeginFrame();
	try {
		ENGINE_PROFILE("imagegraph.argument_colour.sample");
		fixture.Run();
	} catch (...) {
		FrameGraph::EndFrame();
		FrameGraph::SetEnabled(previous);
		throw;
	}
	FrameGraph::EndFrame();
	const auto heapAfter = engine::core::HeapProfile::Totals();
	Check(!FrameGraph::Dropped(), "colour profile dropped scopes");
	const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_ARGUMENT_PRESET");
	std::printf(
		"# argument-colour preset=%s backend=cpu variants=6 captures=96 linked_evaluations=48 "
		"published_receipt_bytes=%llu output_payload_bytes=%llu checksum=%llu owner_ms=%.6f "
		"unmarked_ms=%.6f heap_compiled=%d allocated_bytes=%llu allocated_blocks=%llu\n",
		preset ? preset : "unreported",
		(unsigned long long)fixture.CaptureBytes,
		(unsigned long long)fixture.OutputBytes,
		(unsigned long long)fixture.Checksum,
		FrameGraph::FrameMilliseconds(),
		FrameGraph::UnmarkedMilliseconds(),
		engine::core::HeapProfile::IsCompiledIn(),
		(unsigned long long)(heapAfter.TotalBytes - heapBefore.TotalBytes),
		(unsigned long long)(heapAfter.TotalBlocks - heapBefore.TotalBlocks)
	);
	Metrics::Drain();
	FrameGraph::SetEnabled(previous);
}
