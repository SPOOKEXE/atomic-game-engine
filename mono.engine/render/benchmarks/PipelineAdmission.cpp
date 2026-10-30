// Headless admission through the renderer, including replacement and refusal.
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Log.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("engine.render.bench.pipeline-admission")

namespace {
	using namespace engine;
	using core::Name;
	using namespace graph;

	void Require(bool condition, const char *message) {
		if (!condition) throw std::runtime_error(message);
	}

	PipelineDocument Chain(uint32_t nodes) {
		PipelineDocument document;
		Edit input;
		input.Kind = EditKind::AddResource;
		input.Name = Name("admission.input");
		input.Resource = ResourceKind::Colour;
		input.Format = ResourceFormat::RGBA16F;
		input.External = true;
		document.Record(std::move(input));
		for (uint32_t index = 0; index + 1 < nodes; ++index) {
			Edit resource;
			resource.Kind = EditKind::AddResource;
			resource.Name = Name("admission.image." + std::to_string(index));
			resource.Resource = index != 0 && index % 2 == 0 ? ResourceKind::Storage : ResourceKind::Colour;
			resource.Format = ResourceFormat::RGBA16F;
			document.Record(std::move(resource));
		}
		for (uint32_t index = 0; index < nodes; ++index) {
			const bool output = index + 1 == nodes;
			const bool compute = !output && index != 0 && index % 2 == 0;
			Edit node;
			node.Kind = EditKind::AddNode;
			node.Name = Name("admission.pass." + std::to_string(index));
			node.NodeKind = Name(output ? "output-image" : compute ? "dispatch" : "blit");
			node.Scope = output ? NodeScope::Frame : NodeScope::View;
			document.Record(std::move(node));
			{
				Edit read;
				read.Kind = EditKind::Reads;
				read.Target = index == 0 ? Name("admission.input")
										 : Name("admission.image." + std::to_string(index - 1));
				read.Key = Name(output ? "image" : "source");
				document.Record(std::move(read));
			}
			if (!output) {
				Edit write;
				write.Kind = EditKind::Writes;
				write.Target = Name("admission.image." + std::to_string(index));
				write.Key = Name(compute ? "result" : "colour");
				document.Record(std::move(write));
			}
		}
		return document;
	}

	void Text(std::ostringstream &out, std::string_view text) {
		out << text.size() << ':' << text << ',';
	}

	std::string GraphOracle(const RenderGraph &graph) {
		std::ostringstream out;
		out << graph.Count() << ',' << graph.ResourceCount() << ',';
		for (uint32_t index = 1; index <= graph.ResourceCount(); ++index) {
			const auto *resource = graph.FindResourceDesc(ResourceId{index});
			Require(resource != nullptr, "admission missing resource");
			Text(out, resource->Name.Text());
			out << static_cast<int>(resource->Kind) << ',' << static_cast<int>(resource->Format) << ','
				<< resource->Width << ',' << resource->Height << ',' << resource->External << ','
				<< resource->Divisor << ',' << static_cast<int>(resource->Access) << ',' << resource->Samples
				<< ',' << resource->Depth << ',' << resource->Layers << ',' << resource->FirstMip << ','
				<< resource->MipCount << ',' << static_cast<int>(resource->ColourSpace) << ','
				<< static_cast<int>(resource->AlphaSpace) << ',' << resource->BufferStride << ','
				<< static_cast<int>(resource->Lifetime) << ',' << resource->HistoryGeneration << ',';
			Text(out, resource->Owner.Text());
		}
		for (uint32_t index = 1; index <= graph.Count(); ++index) {
			const auto *node = graph.Find(NodeId{index});
			Require(node != nullptr, "admission missing node");
			Text(out, node->Name.Text());
			Text(out, node->Kind.Text());
			out << static_cast<int>(node->Scope) << ',' << node->Enabled << ',';
			for (const auto *ports : {&node->Reads, &node->Writes}) {
				out << ports->size() << ',';
				for (const auto id : *ports)
					out << id.Value << ',';
			}
			for (const auto *ports : {&node->ReadPorts, &node->WritePorts}) {
				out << ports->size() << ',';
				for (const auto port : *ports)
					Text(out, port.Text());
			}
			out << node->Parameters.size() << ',';
			for (const auto &parameter : node->Parameters) {
				Text(out, parameter.Key.Text());
				Text(out, parameter.Value);
			}
		}
		return out.str();
	}

	std::string ScheduleOracle(const ExecutionSchedule &schedule) {
		std::ostringstream out;
		out << schedule.Waves.size() << ',';
		for (const auto &wave : schedule.Waves) {
			out << wave.Concurrent << ',' << wave.Nodes.size() << ',';
			for (const auto &node : wave.Nodes)
				out << node.Node.Value << ',' << static_cast<int>(node.Queue) << ','
					<< static_cast<int>(node.Async) << ',' << static_cast<int>(node.Culling) << ','
					<< node.AsyncEligible << ',' << node.GroupsX << ',' << node.GroupsY << ',' << node.GroupsZ
					<< ',';
		}
		return out.str();
	}

	std::string InstalledOracle(const render::Renderer::RenderGraphSnapshot &snapshot) {
		std::ostringstream out;
		Text(out, GraphOracle(snapshot.Graph));
		Text(out, ScheduleOracle(snapshot.Schedule));
		for (const auto *scope :
			 {&snapshot.Compiled.Shared, &snapshot.Compiled.PerView, &snapshot.Compiled.Final}) {
			out << scope->size() << ',';
			for (const auto id : *scope)
				out << id.Value << ',';
		}
		out << snapshot.Aliases.PhysicalTargets << ',' << snapshot.Aliases.AliasedResources << ','
			<< snapshot.Aliases.Allocations.size() << ',';
		for (const auto id : snapshot.Aliases.Allocations)
			out << id.Value << ',';
		return out.str();
	}

	bool SameCompiled(const CompiledGraph &left, const CompiledGraph &right) {
		return left.Shared == right.Shared && left.PerView == right.PerView && left.Final == right.Final;
	}

	struct Fixture {
		render::Renderer Renderer;
		Name Pipeline{"bench.admission"};
		PipelineDocument Document;
		bool Refusal = false;
		std::string Oracle;
		bool MemoryReported = false;

		Fixture(std::string_view name, PipelineDocument document, bool refusal = false)
			: Document(std::move(document)), Refusal(refusal) {
			const PipelineDocument initial = refusal ? DefaultPbrDocument() : Document;
			Require(
				static_cast<bool>(Renderer.SetPipelineDocument(Pipeline, initial)), "admission setup refused"
			);
			const auto installed = Renderer.DescribePipeline(Pipeline, 1920, 1080);
			Require(installed.has_value(), "admission setup not installed");
			RenderGraph reference;
			Name offender;
			Require(
				Build(initial, reference, offender) == PipelineDocumentStatus::Ok,
				"admission oracle build failed"
			);
			ExecutionSchedule schedule;
			Require(
				CompileSchedule(reference, schedule, offender) == ScheduleStatus::Ok,
				"admission oracle schedule failed"
			);
			Require(
				GraphOracle(installed->Graph) == GraphOracle(reference),
				"installed graph differs from authored graph"
			);
			Require(
				ScheduleOracle(installed->Schedule) == ScheduleOracle(schedule),
				"installed schedule differs from authored graph"
			);
			CompiledGraph authored;
			Require(
				reference.Compile(authored, offender) == GraphStatus::Ok, "admission oracle scopes failed"
			);
			CompiledGraph ordered;
			for (const auto &wave : schedule.Waves) {
				for (const auto &scheduled : wave.Nodes) {
					const auto *node = reference.Find(scheduled.Node);
					Require(node != nullptr, "admission oracle schedule has a missing node");
					if (node->Scope == NodeScope::World ||
						(node->Scope == NodeScope::Frame &&
						 std::find(authored.Shared.begin(), authored.Shared.end(), scheduled.Node) !=
							 authored.Shared.end()))
						ordered.Shared.push_back(scheduled.Node);
					else if (node->Scope == NodeScope::View)
						ordered.PerView.push_back(scheduled.Node);
					else
						ordered.Final.push_back(scheduled.Node);
				}
			}
			Require(
				SameCompiled(installed->Compiled, ordered), "installed scopes differ from schedule order"
			);
			Oracle = InstalledOracle(*installed);
			const auto result = Admit();
			Check(result);
			const auto after = Renderer.DescribePipeline(Pipeline, 1920, 1080);
			Require(
				after.has_value() && InstalledOracle(*after) == Oracle, "admission changed installed output"
			);
			Require(!refusal || after->Revision == installed->Revision, "refusal changed installed revision");
			std::cout << "# pipeline-admission-oracle case=" << name
					  << " result=" << (refusal ? "refused" : "accepted")
					  << " refusal_log=" << (refusal ? "disabled" : "not_applicable")
					  << " nodes=" << installed->Graph.Count()
					  << " resources=" << installed->Graph.ResourceCount()
					  << " waves=" << installed->Schedule.Waves.size() << " canonical=" << Oracle << '\n';
		}

		render::PipelineAdmissionResult Admit() {
			if (!Refusal) return Renderer.SetPipelineDocument(Pipeline, Document);
			struct Quiet {
				core::LogLevel Previous = core::Log::LevelOf("render");
				Quiet() {
					core::Log::SetLevel("render", core::LogLevel::Off);
				}
				~Quiet() {
					core::Log::SetLevel("render", Previous);
				}
			} quiet;
			return Renderer.SetPipelineDocument(Pipeline, Document);
		}

		void Check(const render::PipelineAdmissionResult &result) const {
			if (!Refusal) {
				Require(static_cast<bool>(result), "admission unexpectedly refused");
				return;
			}
			Require(
				result.Failure.has_value() &&
					result.Failure->Stage == render::PipelineAdmissionStage::Graph &&
					result.Failure->Offender == Name("admission.missing") &&
					result.Failure->Reason == Describe(PipelineDocumentStatus::UnknownName),
				"admission refusal changed"
			);
		}

		void Run(std::string_view name) {
			if (!MemoryReported) {
				MemoryReported = true;
				const char *setting = std::getenv("MONO_BENCH_MEMORY");
				if (setting != nullptr && std::string_view(setting) == "1") {
					Require(core::HeapProfile::IsCompiledIn(), "admission heap hooks unavailable");
					const auto before = core::HeapProfile::Totals();
					const auto result = Admit();
					const auto after = core::HeapProfile::Totals();
					Check(result);
					Require(after.DroppedScopes == before.DroppedScopes, "admission heap scopes dropped");
					std::cout << "# pipeline-admission-heap case=" << name
							  << " timing_valid=0 allocated_bytes=" << after.TotalBytes - before.TotalBytes
							  << " allocated_blocks=" << after.TotalBlocks - before.TotalBlocks
							  << " retained_bytes=" << after.LiveBytes - before.LiveBytes
							  << " retained_blocks=" << after.LiveBlocks - before.LiveBlocks
							  << " process_live_bytes=" << after.LiveBytes
							  << " process_lifetime_peak_bytes=" << after.PeakBytes << '\n';
				}
			}
			const auto result = Admit();
			Check(result);
			testing::Consume(static_cast<bool>(result));
		}
	};

	PipelineDocument Invalid() {
		PipelineDocument document;
		Edit edit;
		edit.Kind = EditKind::Reads;
		edit.Target = Name("admission.missing");
		document.Record(std::move(edit));
		return document;
	}
}

BENCH("Renderer document admission empty", 1) {
	static Fixture fixture("empty", PipelineDocument{});
	fixture.Run("empty");
}
BENCH("Renderer document admission PBR", 1) {
	static Fixture fixture("pbr", DefaultPbrDocument());
	fixture.Run("pbr");
}
BENCH("Renderer document admission stress 256", 1) {
	static Fixture fixture("stress", Chain(RenderGraph::MAXIMUM_NODES));
	fixture.Run("stress");
}
BENCH("Renderer document admission refusal", 1) {
	static Fixture fixture("refusal", Invalid(), true);
	fixture.Run("refusal");
}
