// Actual render catalogue, document installation and device-free frame planning.
// Every workload row performs one operation. The Just job prepares warm fixtures
// before main and runs startup registration in a separate, unwarmed process.

#include <engine/core/HeapProfile.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graphcore/Schema.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.graph.bench.pipelinecatalogue")

namespace render_catalogue_bench {

	using namespace engine::graph;
	using engine::core::HeapProfile;
	using engine::core::HeapTotals;
	using engine::core::Name;
	using engine::testing::Consume;

	constexpr uint32_t WIDTH = 1920;
	constexpr uint32_t HEIGHT = 1080;
	constexpr std::array<uint64_t, 4> WORLDS = {11, 11, 29, 29};

	void Require(bool condition, const char *failure) {
		if (!condition) throw std::runtime_error(failure);
	}

	bool WantsMemory() {
		const char *setting = std::getenv("MONO_BENCH_MEMORY");
		return setting != nullptr && std::string_view(setting) == "1";
	}

	void Number(std::string &text, uint64_t number) {
		text += std::to_string(number);
		text.push_back(',');
	}

	void Named(std::string &text, std::string_view name) {
		Number(text, name.size());
		text.append(name);
		text.push_back(',');
	}

	std::string CatalogueOracle() {
		std::string oracle;
		size_t ports = 0;
		std::string_view previous;
		for (const NodeKindSpec &spec : NodeCatalogue::All()) {
			Require(previous.empty() || previous < spec.Kind.Text(), "catalogue kind order changed");
			previous = spec.Kind.Text();
			Require(spec.Declaration.Id == spec.Kind.Text(), "common render kind identity changed");
			Require(
				spec.Declaration.Ports.size() == spec.Inputs.size() + spec.Outputs.size(),
				"common render port count changed"
			);
			Named(oracle, spec.Declaration.Id);
			Number(oracle, static_cast<uint8_t>(spec.Queue));
			Number(oracle, static_cast<uint8_t>(spec.Scope));
			Number(oracle, static_cast<uint8_t>(spec.Lifetime));
			Number(oracle, spec.HistoryReads);
			Number(oracle, spec.Inputs.size());
			Number(oracle, spec.Outputs.size());
			for (const PortDirection direction : {PortDirection::Input, PortDirection::Output}) {
				const auto &authored = direction == PortDirection::Input ? spec.Inputs : spec.Outputs;
				for (size_t index = 0; index < authored.size(); ++index) {
					const auto *port = spec.DeclaredPort(direction, index);
					Require(
						port != nullptr && port->Id == authored[index].Name.Text() &&
							port->ValueType == ResourcePortType(authored[index].Kind) && port->Optional &&
							port->Direction == (direction == PortDirection::Input
													? engine::graphcore::PortDirection::Input
													: engine::graphcore::PortDirection::Output) &&
							port->MaximumConnections == 1 && spec.DeclaredPort(direction, port->Id) == port,
						"common render port projection changed"
					);
					Named(oracle, port->Id);
					Named(oracle, port->ValueType);
					Number(oracle, static_cast<uint8_t>(port->Direction));
					++ports;
				}
			}
			const engine::graphcore::Limits limits{
				.MaximumSchemas = 1,
				.MaximumPorts = NodeCatalogue::MAXIMUM_SCHEMA_PORTS,
				.MaximumIdentityBytes = NodeCatalogue::MAXIMUM_SCHEMA_IDENTITY_BYTES,
			};
			engine::graphcore::Diagnostic diagnostic;
			Require(
				engine::graphcore::ValidateSchemas(std::span(&spec.Declaration, 1), limits, diagnostic) ==
					engine::graphcore::CompileStatus::Ok,
				"installed render declaration is invalid"
			);
		}
		Require(
			NodeCatalogue::Find(Name("world")) != nullptr && NodeCatalogue::Find(Name("blit")) != nullptr &&
				NodeCatalogue::Find(Name("dispatch")) != nullptr &&
				NodeCatalogue::Find(Name("output-image")) != nullptr,
			"builtin render registration is incomplete"
		);
		Number(oracle, ports);
		return oracle;
	}

	PipelineDocument ChainDocument(uint32_t nodes) {
		Require(nodes >= 3 && nodes <= RenderGraph::MAXIMUM_NODES, "invalid render benchmark chain bound");
		PipelineDocument document;
		for (uint32_t index = 0; index + 1 < nodes; ++index) {
			Edit resource;
			resource.Kind = EditKind::AddResource;
			resource.Name = Name("bench.image." + std::to_string(index));
			resource.Resource = index != 0 && index % 2 == 0 ? ResourceKind::Storage : ResourceKind::Colour;
			resource.Format = ResourceFormat::RGBA16F;
			document.Record(std::move(resource));
		}
		for (uint32_t index = 0; index < nodes; ++index) {
			const bool output = index + 1 == nodes;
			const bool compute = !output && index != 0 && index % 2 == 0;
			Edit node;
			node.Kind = EditKind::AddNode;
			node.Name = Name("bench.pass." + std::to_string(index));
			node.NodeKind = Name(
				output		 ? "output-image"
				: index == 0 ? "clear"
				: compute	 ? "dispatch"
							 : "blit"
			);
			node.Scope = output ? NodeScope::Frame : NodeScope::View;
			document.Record(std::move(node));
			if (index != 0) {
				Edit read;
				read.Kind = EditKind::Reads;
				read.Target = Name("bench.image." + std::to_string(index - 1));
				read.Key = Name(output ? "image" : "source");
				document.Record(std::move(read));
			}
			if (!output) {
				Edit write;
				write.Kind = EditKind::Writes;
				write.Target = Name("bench.image." + std::to_string(index));
				write.Key = Name(index == 0 ? "target" : compute ? "result" : "colour");
				document.Record(std::move(write));
			}
		}
		return document;
	}

	struct Pipeline {
		RenderGraph Graph;
		ExecutionSchedule Schedule;
		FrameExecutionPlan Frame;
	};

	Pipeline Install(const PipelineDocument &document) {
		Pipeline pipeline;
		Name offender;
		Require(
			Build(document, pipeline.Graph, offender) == PipelineDocumentStatus::Ok,
			"render benchmark document build refused"
		);
		Require(
			CompileSchedule(pipeline.Graph, pipeline.Schedule, offender) == ScheduleStatus::Ok,
			"render benchmark scheduling refused"
		);
		Require(
			PlanFrame(pipeline.Graph, pipeline.Schedule, WORLDS, WIDTH, HEIGHT, pipeline.Frame, offender) ==
				ExecutionPlanStatus::Ok,
			"render benchmark frame planning refused"
		);
		return pipeline;
	}

	void ScheduledOracle(std::string &oracle, const ScheduledNode &node, const RenderGraph &graph) {
		const Node *authored = graph.Find(node.Node);
		Require(authored != nullptr && authored->Enabled, "schedule contains an absent or disabled node");
		const NodeKindSpec *spec = NodeCatalogue::Find(authored->Kind);
		Require(
			spec != nullptr && node.Queue == spec->Queue && node.Async == AsyncPolicy::Automatic &&
				node.Culling == CullingMode::Inherit && node.GroupsX == 1 && node.GroupsY == 1 &&
				node.GroupsZ == 1 &&
				node.AsyncEligible ==
					(spec->Queue == ExecutionQueue::Compute || spec->Queue == ExecutionQueue::Transfer),
			"render scheduling metadata differs from installed catalogue"
		);
		Named(oracle, authored->Name.Text());
		Named(oracle, authored->Kind.Text());
		Number(oracle, static_cast<uint8_t>(authored->Scope));
		Number(oracle, static_cast<uint8_t>(node.Queue));
		Number(oracle, static_cast<uint8_t>(node.Async));
		Number(oracle, static_cast<uint8_t>(node.Culling));
		Number(oracle, node.AsyncEligible);
		Number(oracle, node.GroupsX);
		Number(oracle, node.GroupsY);
		Number(oracle, node.GroupsZ);
	}

	// Stable text carries every schedule/plan field, including names instead of interned ids.
	std::string PipelineOracle(const Pipeline &pipeline) {
		std::string oracle;
		const auto &graph = pipeline.Graph;
		std::vector<uint8_t> seen(graph.Count());
		size_t enabled = 0;
		for (uint32_t index = 1; index <= graph.Count(); ++index)
			enabled += graph.Find(NodeId{index})->Enabled;
		size_t scheduled = 0;
		Require(pipeline.Schedule.Waves.size() == pipeline.Frame.Waves.size(), "frame wave count changed");
		Number(oracle, graph.Count());
		Number(oracle, graph.ResourceCount());
		Number(oracle, pipeline.Schedule.Waves.size());
		for (size_t index = 0; index < pipeline.Schedule.Waves.size(); ++index) {
			const auto &wave = pipeline.Schedule.Waves[index];
			Number(oracle, wave.Nodes.size());
			Number(oracle, wave.Concurrent);
			uint32_t previous = 0;
			for (const auto &node : wave.Nodes) {
				Require(
					node.Node.Value > previous && node.Node.Value <= seen.size() &&
						!seen[node.Node.Value - 1],
					"schedule duplicate or authored wave order changed"
				);
				previous = node.Node.Value;
				seen[node.Node.Value - 1] = 1;
				++scheduled;
				ScheduledOracle(oracle, node, graph);
			}
			const auto &frame = pipeline.Frame.Waves[index];
			Require(frame.ConcurrentQueues == wave.Concurrent, "frame queue concurrency changed");
			Number(oracle, frame.Invocations.size());
			Number(oracle, frame.ConcurrentQueues);
			Number(oracle, frame.IndependentWorlds);
			Number(oracle, frame.IndependentViews);
			size_t invocation = 0;
			for (const auto &node : wave.Nodes) {
				const auto scope = graph.Find(node.Node)->Scope;
				const size_t copies = scope == NodeScope::World	 ? 2
									  : scope == NodeScope::View ? WORLDS.size()
																 : 1;
				for (size_t copy = 0; copy < copies; ++copy) {
					Require(invocation < frame.Invocations.size(), "frame dropped an invocation");
					const auto &expanded = frame.Invocations[invocation++];
					Require(
						expanded.Scheduled.Node == node.Node && expanded.Scope == scope &&
							expanded.View == (scope == NodeScope::View ? copy : RunContext::WHOLE_FRAME) &&
							expanded.World == (scope == NodeScope::Frame   ? RunContext::WHOLE_FRAME
											   : scope == NodeScope::World ? copy
																		   : copy / 2) &&
							expanded.WorldKey == (scope == NodeScope::Frame	  ? 0
												  : scope == NodeScope::World ? WORLDS[copy * 2]
																			  : WORLDS[copy]),
						"frame scope multiplicity changed"
					);
					ScheduledOracle(oracle, expanded.Scheduled, graph);
					Number(oracle, expanded.View == RunContext::WHOLE_FRAME ? UINT64_MAX : expanded.View);
					Number(oracle, expanded.World == RunContext::WHOLE_FRAME ? UINT64_MAX : expanded.World);
					Number(oracle, expanded.WorldKey);
					Number(oracle, expanded.ReadBytes);
					Number(oracle, expanded.WriteBytes);
				}
			}
			Require(invocation == frame.Invocations.size(), "frame added an unexpected invocation");
		}
		Require(scheduled == enabled, "schedule omitted an enabled node");
		Number(oracle, pipeline.Frame.Transfers.size());
		uint64_t transferBytes = 0;
		for (const auto &transfer : pipeline.Frame.Transfers) {
			const auto *resource = graph.FindResourceDesc(transfer.Resource);
			const auto *producer = graph.Find(transfer.Producer);
			const auto *consumer = graph.Find(transfer.Consumer);
			Require(
				resource != nullptr && transfer.From != transfer.To && producer != nullptr &&
					consumer != nullptr && transfer.Bytes == resource->Bytes(WIDTH, HEIGHT),
				"frame queue handoff changed"
			);
			Named(oracle, resource->Name.Text());
			Named(oracle, producer->Name.Text());
			Named(oracle, consumer->Name.Text());
			Number(oracle, static_cast<uint8_t>(transfer.From));
			Number(oracle, static_cast<uint8_t>(transfer.To));
			Number(oracle, transfer.View == RunContext::WHOLE_FRAME ? UINT64_MAX : transfer.View);
			Number(oracle, transfer.World == RunContext::WHOLE_FRAME ? UINT64_MAX : transfer.World);
			Number(oracle, transfer.Bytes);
			transferBytes += transfer.Bytes;
		}
		Require(transferBytes == pipeline.Frame.QueueTransferBytes, "frame handoff byte sum changed");
		Number(oracle, pipeline.Frame.ReadBytes);
		Number(oracle, pipeline.Frame.WriteBytes);
		Number(oracle, pipeline.Frame.QueueTransferBytes);
		return oracle;
	}

	void VerifyChain(const Pipeline &pipeline, uint32_t nodes) {
		Require(
			pipeline.Graph.Count() == nodes && pipeline.Graph.ResourceCount() == nodes - 1 &&
				pipeline.Schedule.Waves.size() == nodes,
			"render chain size or wave count changed"
		);
		size_t transfers = 0;
		ExecutionQueue previous = ExecutionQueue::Graphics;
		for (uint32_t index = 0; index < nodes; ++index) {
			const auto &wave = pipeline.Schedule.Waves[index];
			const auto expected = index + 1 == nodes || index % 2 == 1 ? ExecutionQueue::Transfer
								  : index == 0						   ? ExecutionQueue::Graphics
																	   : ExecutionQueue::Compute;
			Require(
				wave.Nodes.size() == 1 && wave.Nodes[0].Node.Value == index + 1 &&
					wave.Nodes[0].Queue == expected && !wave.Concurrent,
				"render chain schedule changed"
			);
			if (index != 0 && previous != expected) transfers += WORLDS.size();
			previous = expected;
		}
		const uint64_t imageBytes = uint64_t{WIDTH} * HEIGHT * 8;
		const uint64_t expectedBytes = imageBytes * (nodes - 1) * WORLDS.size();
		Require(
			pipeline.Frame.ReadBytes == expectedBytes && pipeline.Frame.WriteBytes == expectedBytes &&
				pipeline.Frame.Transfers.size() == transfers &&
				pipeline.Frame.QueueTransferBytes == transfers * imageBytes,
			"render chain traffic changed"
		);
	}

	void PrintWork(const char *name, const Pipeline &pipeline) {
		size_t scheduled = 0;
		size_t invocations = 0;
		for (const auto &wave : pipeline.Schedule.Waves)
			scheduled += wave.Nodes.size();
		for (const auto &wave : pipeline.Frame.Waves)
			invocations += wave.Invocations.size();
		std::cout << "# b0 renderer work v1\t" << name << '\t' << pipeline.Graph.Count() << '\t'
				  << pipeline.Graph.ResourceCount() << '\t' << scheduled << '\t'
				  << pipeline.Schedule.Waves.size() << '\t' << invocations << '\t'
				  << pipeline.Frame.Transfers.size() << '\t' << pipeline.Frame.ReadBytes << '\t'
				  << pipeline.Frame.WriteBytes << '\t' << pipeline.Frame.QueueTransferBytes << '\n';
	}

	void PrintHeap(const char *name, const HeapTotals &before, const HeapTotals &after) {
		std::cout << "# b0 renderer heap v1\t" << name << '\t' << after.TotalBytes - before.TotalBytes << '\t'
				  << after.TotalBlocks - before.TotalBlocks << '\t' << after.LiveBytes - before.LiveBytes
				  << '\t' << after.LiveBlocks - before.LiveBlocks << '\t' << after.LiveBytes << '\t'
				  << after.LiveBlocks << '\t' << before.PeakBytes << '\t' << after.PeakBytes << '\t'
				  << after.OverheadBytes << '\n';
	}

	template <class Work> void ProbeHeap(const char *name, Work work) {
		if (!WantsMemory()) return;
		Require(HeapProfile::IsCompiledIn(), "renderer catalogue heap probe requires MONO_HEAP_PROFILE");
		const auto before = HeapProfile::Totals();
		{
			ENGINE_HEAP_SCOPE(name);
			work();
		}
		PrintHeap(name, before, HeapProfile::Totals());
	}

	void CompilePrepared(const Pipeline &pipeline) {
		ExecutionSchedule schedule;
		Name offender;
		Require(
			CompileSchedule(pipeline.Graph, schedule, offender) == ScheduleStatus::Ok,
			"prepared schedule refused"
		);
		Consume(schedule.Waves.size());
	}

	void PlanPrepared(const Pipeline &pipeline) {
		FrameExecutionPlan frame;
		Name offender;
		Require(
			PlanFrame(pipeline.Graph, pipeline.Schedule, WORLDS, WIDTH, HEIGHT, frame, offender) ==
				ExecutionPlanStatus::Ok,
			"prepared frame refused"
		);
		Consume(frame.ReadBytes);
		Consume(frame.WriteBytes);
		Consume(frame.Transfers.size());
	}

	void InstallDocument(const PipelineDocument &document) {
		const auto installed = Install(document);
		Consume(installed.Frame.ReadBytes);
		Consume(installed.Frame.Transfers.size());
	}

	struct Prepared {
		std::array<PipelineDocument, 4> Documents;
		std::array<Pipeline, 4> Pipelines;
		NodeKindSpec Replacement;
		NodeKindSpec Refusal;
	};

	Prepared Prepare() {
		RegisterRenderNodeKinds();
		const std::string catalogue = CatalogueOracle();
		Prepared prepared;
		prepared.Replacement = *NodeCatalogue::Find(Name("blit"));
		prepared.Replacement.Declaration.Id = "deliberately-stale-projection";
		Require(NodeCatalogue::Register(prepared.Replacement), "render kind replacement refused");
		Require(CatalogueOracle() == catalogue, "registration trusted a stale derived declaration");
		prepared.Refusal = prepared.Replacement;
		prepared.Refusal.Outputs.push_back(prepared.Refusal.Outputs.front());
		Require(
			!NodeCatalogue::Register(prepared.Refusal) && CatalogueOracle() == catalogue,
			"refused render kind replaced an installed declaration"
		);
		NodeCatalogue::Reset();
		RegisterRenderNodeKinds();
		Require(CatalogueOracle() == catalogue, "render registry rebuild changed common declarations");
		prepared.Documents = {
			PipelineDocument{},
			ChainDocument(3),
			DefaultPbrDocument(),
			ChainDocument(RenderGraph::MAXIMUM_NODES)
		};
		constexpr std::array<const char *, 4> names = {"empty", "simple", "default-pbr", "stress-256"};
		constexpr std::array<const char *, 4> retainNames = {
			"retain empty install",
			"retain simple install",
			"retain default PBR install",
			"retain stress install"
		};
		constexpr std::array<const char *, 4> compileNames = {
			"compile empty", "compile simple", "compile default PBR", "compile stress"
		};
		constexpr std::array<const char *, 4> frameNames = {
			"frame empty", "frame simple", "frame default PBR", "frame stress"
		};
		std::cout << "# b0 renderer settings v1\tdevice-free\t" << WIDTH << '\t' << HEIGHT
				  << "\tviews=4\tworlds=2\theap_hooks=" << HeapProfile::IsCompiledIn() << '\n';
		std::cout
			<< "# b0 renderer work columns v1\tname\tnodes\tresources\tscheduled_nodes\twaves\tinvocations"
			   "\tqueue_handoffs\tlogical_read_bytes\tlogical_write_bytes\tlogical_handoff_bytes\n";
		std::cout << "# b0 renderer heap columns "
					 "v1\tname\tallocated_bytes\tallocated_blocks\tretained_bytes\tretained_blocks"
					 "\tprocess_live_bytes\tprocess_live_blocks\tprocess_peak_before\tprocess_peak_"
					 "after\tprocess_profiler_overhead_bytes\n";
		std::cout << "# b0 renderer catalogue oracle v1\t" << NodeCatalogue::All().size() << '\t' << catalogue
				  << '\n';
		for (size_t index = 0; index < prepared.Documents.size(); ++index) {
			const auto retain = [&] { prepared.Pipelines[index] = Install(prepared.Documents[index]); };
			if (WantsMemory())
				ProbeHeap(retainNames[index], retain);
			else
				retain();
			const auto &pipeline = prepared.Pipelines[index];
			const std::string oracle = PipelineOracle(pipeline);
			Require(
				PipelineOracle(Install(prepared.Documents[index])) == oracle,
				"render pipeline is not deterministic"
			);
			std::cout << "# b0 renderer semantic oracle v1\t" << names[index] << '\t' << oracle << '\n';
			PrintWork(names[index], pipeline);
			ProbeHeap(names[index], [&] { InstallDocument(prepared.Documents[index]); });
			ProbeHeap(compileNames[index], [&] { CompilePrepared(pipeline); });
			ProbeHeap(frameNames[index], [&] { PlanPrepared(pipeline); });
		}
		Require(
			prepared.Pipelines[0].Graph.Count() == 0 && prepared.Pipelines[0].Frame.Waves.empty() &&
				prepared.Pipelines[0].Frame.ReadBytes == 0 && prepared.Pipelines[0].Frame.WriteBytes == 0,
			"empty render pipeline changed"
		);
		VerifyChain(prepared.Pipelines[1], 3);
		VerifyChain(prepared.Pipelines[3], RenderGraph::MAXIMUM_NODES);
		ProbeHeap("register retained builtins", [] { RegisterRenderNodeKinds(); });
		ProbeHeap("reset and rebuild retained registry", [] {
			NodeCatalogue::Reset();
			RegisterRenderNodeKinds();
		});
		ProbeHeap("replace one kind", [&] {
			Require(NodeCatalogue::Register(prepared.Replacement), "replacement refused");
		});
		ProbeHeap("refuse duplicate port", [&] {
			Require(!NodeCatalogue::Register(prepared.Refusal), "invalid replacement accepted");
		});
		Require(CatalogueOracle() == catalogue, "heap probes changed installed declarations");
		return prepared;
	}

	Prepared &Fixtures() {
		static Prepared prepared = Prepare();
		return prepared;
	}

	size_t startupCalls = 0;
	HeapTotals startupBefore;
	HeapTotals startupAfter;
	bool startupHeap = false;

	void RegisterStartup() {
		Require(startupCalls++ == 0, "startup requires --samples 1 --warmup-samples 0 and a fresh process");
		Require(NodeCatalogue::All().empty(), "startup render catalogue was already registered");
		if (startupHeap) startupBefore = HeapProfile::Totals();
		{
			ENGINE_HEAP_SCOPE("renderer catalogue startup");
			RegisterRenderNodeKinds();
		}
		if (startupHeap) startupAfter = HeapProfile::Totals();
	}

	void VerifyStartup() {
		Require(startupCalls == 1, "startup registration did not run exactly once");
		std::cout << "# b0 renderer startup oracle v1\t" << NodeCatalogue::All().size() << '\t'
				  << CatalogueOracle() << '\n';
		if (startupHeap) PrintHeap("fresh-process registration", startupBefore, startupAfter);
	}

	void Declare(const char *name, std::function<void()> body) {
		engine::testing::BenchRegistry::Declare(
			{.Suite = MonoTestSuiteId, .Name = name, .Iterations = 1, .Body = std::move(body)}
		);
	}

	[[maybe_unused]] const bool DECLARED = [] {
		const char *setting = std::getenv("MONO_RENDER_CATALOGUE_COHORT");
		const std::string_view cohort = setting == nullptr ? "warm" : setting;
		if (cohort == "startup") {
			startupHeap = WantsMemory();
			Require(
				!startupHeap || HeapProfile::IsCompiledIn(), "startup heap capture requires MONO_HEAP_PROFILE"
			);
			Declare(
				"RegisterRenderNodeKinds fresh process (includes guard and optional heap snapshots)",
				RegisterStartup
			);
			// This row checks the prior row outside its timer; it is not a workload latency.
			Declare("Startup oracle verification only (exclude from workload timings)", VerifyStartup);
			return true;
		}
		Require(cohort == "warm", "MONO_RENDER_CATALOGUE_COHORT must be startup or warm");
		// Explicit warm mode prepares all fixtures and heap probes before harness timing.
		// Ordinary suite discovery leaves other graph benchmark workloads untouched.
		if (setting != nullptr) (void)Fixtures();
		Declare("RegisterRenderNodeKinds retained replacement", [] {
			(void)Fixtures();
			RegisterRenderNodeKinds();
		});
		Declare("Reset and RegisterRenderNodeKinds retained capacity", [] {
			(void)Fixtures();
			NodeCatalogue::Reset();
			RegisterRenderNodeKinds();
		});
		Declare("Register one render kind replacement", [] {
			Require(NodeCatalogue::Register(Fixtures().Replacement), "render replacement refused");
		});
		Declare("Refuse duplicate render port preserving installed kind", [] {
			Require(!NodeCatalogue::Register(Fixtures().Refusal), "invalid render replacement accepted");
		});
		constexpr std::array<const char *, 4> installNames = {
			"Build + CompileSchedule + PlanFrame empty",
			"Build + CompileSchedule + PlanFrame simple",
			"Build + CompileSchedule + PlanFrame default PBR",
			"Build + CompileSchedule + PlanFrame stress 256"
		};
		constexpr std::array<const char *, 4> compileNames = {
			"CompileSchedule prepared empty",
			"CompileSchedule prepared simple",
			"CompileSchedule prepared default PBR",
			"CompileSchedule prepared stress 256"
		};
		constexpr std::array<const char *, 4> planNames = {
			"PlanFrame prepared empty",
			"PlanFrame prepared simple",
			"PlanFrame prepared default PBR",
			"PlanFrame prepared stress 256"
		};
		for (size_t index = 0; index < installNames.size(); ++index) {
			Declare(installNames[index], [index] { InstallDocument(Fixtures().Documents[index]); });
			Declare(compileNames[index], [index] { CompilePrepared(Fixtures().Pipelines[index]); });
			Declare(planNames[index], [index] { PlanPrepared(Fixtures().Pipelines[index]); });
		}
		return true;
	}();
}
