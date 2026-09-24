// Named installed-pipeline ownership and inspection.

#include "PipelineCompiler.hpp"
#include "RendererState.hpp"

#include <engine/core/Log.hpp>
#include <engine/graph/PipelineDocument.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace engine::render {
	namespace {
		PipelineAdmissionResult
		Refused(PipelineAdmissionStage stage, core::Name offender, std::string reason) {
			return {
				.Failure = PipelineFailure{
					.Stage = stage,
					.Offender = offender,
					.Reason = std::move(reason),
				}
			};
		}

		void LogPipelineFailure(core::Name name, const PipelineFailure &failure) {
			ENGINE_ERROR("pipeline '{}': {}", name.Text(), FormatPipelineFailure(failure));
		}

		bool BuildPipelineDocument(
			const graph::PipelineDocument &document,
			graph::RenderGraph &pipeline,
			core::Name &offender,
			PipelineAdmissionResult &failure
		) {
			const graph::PipelineDocumentStatus status = graph::Build(document, pipeline, offender);
			if (status == graph::PipelineDocumentStatus::Ok) {
				return true;
			}
			// `Invalid` means the document's schedule failed. The renderer compiler
			// runs next to recover its detailed boundary, or an earlier graph error.
			if (status == graph::PipelineDocumentStatus::Invalid) return true;
			failure = Refused(PipelineAdmissionStage::Graph, offender, graph::Describe(status));
			return false;
		}
	}

	bool Renderer::SetPipeline(core::Name name, const graph::RenderGraph &pipeline) {
		return static_cast<bool>(SetPipelineWithResult(name, pipeline));
	}

	PipelineAdmissionResult
	Renderer::SetPipelineWithResult(core::Name name, const graph::RenderGraph &pipeline) {
		RequireOwningThread("SetPipelineWithResult");
		if (!name.IsValid()) {
			PipelineAdmissionResult result = Refused(
				PipelineAdmissionStage::Validation, name, "a render pipeline needs a name a view can select"
			);
			LogPipelineFailure(name, *result.Failure);
			return result;
		}

		std::vector<core::Name> customKinds;
		customKinds.reserve(CustomNodeHandlers.size());
		for (const InstalledNodeHandler &installed : CustomNodeHandlers) {
			customKinds.push_back(installed.Kind);
		}
		const DeviceCaps *caps = State->Device != nullptr ? &State->Caps : nullptr;
		PipelineCompilation compilation = CompilePipeline(name, pipeline, caps, customKinds);
		if (!compilation) {
			PipelineAdmissionResult result{.Failure = compilation.Failure};
			LogPipelineFailure(name, *result.Failure);
			return result;
		}

		for (const InstalledPipeline &installed : State->InstalledPipelines) {
			if (installed.Name != name) {
				continue;
			}
			if (State->Device != nullptr) {
				(void)WaitForFrame();
			}
			(void)State->RetireNamed(name);
			State->InstallNamed(std::move(*compilation.Package));
			return {};
		}

		State->InstallNamed(std::move(*compilation.Package));
		return {};
	}

	PipelineAdmissionResult
	Renderer::SetPipelineDocument(core::Name name, const graph::PipelineDocument &document) {
		RequireOwningThread("SetPipelineDocument");
		if (!name.IsValid()) {
			PipelineAdmissionResult result = Refused(
				PipelineAdmissionStage::Validation, name, "a render pipeline needs a name a view can select"
			);
			LogPipelineFailure(name, *result.Failure);
			return result;
		}

		graph::RenderGraph pipeline;
		core::Name offender;
		PipelineAdmissionResult failure;
		if (!BuildPipelineDocument(document, pipeline, offender, failure)) {
			LogPipelineFailure(name, *failure.Failure);
			return failure;
		}

		// Build collapses schedule errors to Invalid. Renderer admission retains
		// the detailed failure from the same graph.
		return SetPipelineWithResult(name, pipeline);
	}

	PipelineAdmissionResult
	Renderer::ValidatePipelineDocument(const graph::PipelineDocument &document) const {
		RequireOwningThread("ValidatePipelineDocument");
		graph::RenderGraph pipeline;
		core::Name offender;
		PipelineAdmissionResult failure;
		if (!BuildPipelineDocument(document, pipeline, offender, failure)) return failure;

		std::vector<core::Name> customKinds;
		customKinds.reserve(CustomNodeHandlers.size());
		for (const InstalledNodeHandler &installed : CustomNodeHandlers)
			customKinds.push_back(installed.Kind);
		const DeviceCaps *caps = State->Device != nullptr ? &State->Caps : nullptr;
		PipelineCompilation compilation =
			CompilePipeline(core::Name("pipeline document validation"), pipeline, caps, customKinds);
		if (!compilation) return {.Failure = std::move(compilation.Failure)};
		return {};
	}

	std::optional<Renderer::RenderGraphSnapshot>
	Renderer::DescribePipeline(core::Name name, uint32_t viewWidth, uint32_t viewHeight) const {
		RequireOwningThread("DescribePipeline");
		if (State == nullptr || !name.IsValid() || viewWidth == 0 || viewHeight == 0 || viewWidth > 16384 ||
			viewHeight > 16384) {
			return std::nullopt;
		}
		const auto boundedName = [](core::Name value, bool required) {
			return (!required || value.IsValid()) &&
				   (!value.IsValid() || (!value.Text().empty() && value.Text().size() <= 128));
		};
		const Impl::InstalledPipeline *installed = State->Installed(name);
		if (installed == nullptr || !boundedName(installed->Name, true) || installed->Graph.Count() > 256 ||
			installed->Graph.ResourceCount() > 512) {
			return std::nullopt;
		}
		for (uint32_t value = 1; value <= installed->Graph.Count(); ++value) {
			const graph::Node *node = installed->Graph.Find(graph::NodeId{value});
			if (node == nullptr || !boundedName(node->Name, true) || !boundedName(node->Kind, true)) {
				return std::nullopt;
			}
			for (const core::Name port : node->ReadPorts) {
				if (!boundedName(port, false)) {
					return std::nullopt;
				}
			}
			for (const core::Name port : node->WritePorts) {
				if (!boundedName(port, false)) {
					return std::nullopt;
				}
			}
		}
		for (uint32_t value = 1; value <= installed->Graph.ResourceCount(); ++value) {
			const graph::ResourceDesc *resource = installed->Graph.FindResource(graph::ResourceId{value});
			if (resource == nullptr || !boundedName(resource->Name, true) ||
				!boundedName(resource->Owner, false)) {
				return std::nullopt;
			}
		}
		return State->Snapshot(*installed, viewWidth, viewHeight);
	}

	std::optional<Renderer::PipelineIdentity> Renderer::ResolvePipelineIdentity(core::Name requested) const {
		RequireOwningThread("ResolvePipelineIdentity");
		if (State == nullptr) return std::nullopt;
		const Impl::InstalledPipeline *installed = State->PipelineFor(requested);
		if (installed == nullptr || !installed->Name.IsValid() || installed->Revision == 0)
			return std::nullopt;
		return PipelineIdentity{.Name = installed->Name, .Revision = installed->Revision};
	}

	bool Renderer::HasPipelineRevision(core::Name name, uint64_t revision) const {
		RequireOwningThread("HasPipelineRevision");
		if (State == nullptr || !name.IsValid() || revision == 0) return false;
		for (const Impl::InstalledPipeline &candidate : State->InstalledPipelines)
			if (candidate.Name == name) return candidate.Revision == revision;
		return State->EngineDefault && State->EngineDefault->Name == name &&
			   State->EngineDefault->Revision == revision;
	}
	bool Renderer::RemovePipeline(core::Name name) {
		RequireOwningThread("RemovePipeline");
		for (size_t index = 0; index < State->InstalledPipelines.size(); index++) {
			if (State->InstalledPipelines[index].Name != name) {
				continue;
			}
			if (State->Device != nullptr) {
				(void)WaitForFrame();
			}
			return State->RetireNamed(name);
		}
		return false;
	}

	std::vector<core::Name> Renderer::Pipelines() const {
		std::vector<core::Name> names;
		names.reserve(State->InstalledPipelines.size());
		for (const Impl::InstalledPipeline &pipeline : State->InstalledPipelines) {
			names.push_back(pipeline.Name);
		}
		std::sort(names.begin(), names.end(), [](core::Name first, core::Name second) {
			return first.Text() < second.Text();
		});
		return names;
	}

	void Renderer::ResetPipelines() {
		RequireOwningThread("ResetPipelines");
		if (State->Device != nullptr && !State->InstalledPipelines.empty()) {
			(void)WaitForFrame();
		}
		State->RetireAllNamed();
	}

	bool Renderer::InstallEngineDefault(const graph::PipelineDocument &document) {
		graph::RenderGraph pipeline;
		core::Name offender;
		PipelineAdmissionResult failure;
		if (!BuildPipelineDocument(document, pipeline, offender, failure)) {
			ENGINE_ERROR("engine default render graph {}", FormatPipelineFailure(*failure.Failure));
			return false;
		}

		PipelineCompilation compilation =
			CompilePipeline(core::Name("Engine Default"), pipeline, nullptr, {});
		if (!compilation) {
			ENGINE_ERROR("engine default render graph {}", FormatPipelineFailure(compilation.Failure));
			return false;
		}
		State->InstallDefault(std::move(*compilation.Package));
		return true;
	}
}
