// Compiles one authored render graph into its immutable execution package.

#include "PipelineCompiler.hpp"

#include "RenderNodeExecutor.hpp"

#include <engine/graph/PipelineCatalogue.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine::render {

	const char *DescribePipelineAdmission(PipelineAdmissionStage stage) {
		switch (stage) {
		case PipelineAdmissionStage::Graph:
			return "graph";
		case PipelineAdmissionStage::Schedule:
			return "schedule";
		case PipelineAdmissionStage::Backend:
			return "backend";
		case PipelineAdmissionStage::Validation:
			return "pipeline validation";
		case PipelineAdmissionStage::Capability:
			return "device capability";
		}
		return "pipeline validation";
	}

	namespace {

		bool DependsOnKind(
			const graph::RenderGraph &pipeline,
			graph::NodeId consumer,
			core::Name required,
			std::unordered_set<uint32_t> &visited
		) {
			if (!consumer.IsValid() || !visited.insert(consumer.Value).second) return false;
			const graph::Node *node = pipeline.Find(consumer);
			if (node == nullptr || !node->Enabled) return false;
			if (node->Kind == required) return true;

			for (const graph::ResourceId resource : node->Reads) {
				graph::NodeId producer;
				for (uint32_t value = 1; value < consumer.Value; value++) {
					const graph::Node *candidate = pipeline.Find(graph::NodeId{value});
					if (candidate != nullptr && candidate->Enabled &&
						std::find(candidate->Writes.begin(), candidate->Writes.end(), resource) !=
							candidate->Writes.end()) {
						producer = graph::NodeId{value};
					}
				}
				if (DependsOnKind(pipeline, producer, required, visited)) return true;
			}
			return false;
		}

		bool DependsOnKind(const graph::RenderGraph &pipeline, graph::NodeId consumer, core::Name required) {
			std::unordered_set<uint32_t> visited;
			return DependsOnKind(pipeline, consumer, required, visited);
		}

		bool ValidatePipeline(
			const graph::RenderGraph &pipeline,
			graph::CompiledGraph &compiled,
			graph::ExecutionSchedule &schedule,
			core::Name &offender,
			std::string &reason,
			PipelineAdmissionStage &stage,
			const DeviceCaps *caps = nullptr,
			std::span<const core::Name> customKinds = {}
		) {
			const graph::GraphStatus graphStatus = pipeline.Compile(compiled, offender);
			if (graphStatus != graph::GraphStatus::Ok) {
				stage = PipelineAdmissionStage::Graph;
				reason = graph::Describe(graphStatus);
				return false;
			}
			const graph::ScheduleStatus status = graph::CompileSchedule(pipeline, schedule, offender);
			if (status != graph::ScheduleStatus::Ok) {
				stage = PipelineAdmissionStage::Schedule;
				reason = graph::Describe(status);
				return false;
			}

			// Resource edges, not canvas or declaration position, own execution.
			// SDL records each dependency wave serially, preserving the schedule on a
			// backend that exposes one portable command stream.
			const std::vector<graph::NodeId> setup = compiled.Shared;
			compiled.Shared.clear();
			compiled.PerView.clear();
			compiled.Final.clear();
			for (const graph::ExecutionWave &wave : schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					const graph::Node *node = pipeline.Find(scheduled.Node);
					if (node == nullptr) {
						offender = {};
						reason = "the compiled schedule names no node";
						return false;
					}
					switch (node->Scope) {
					case graph::NodeScope::World:
						compiled.Shared.push_back(scheduled.Node);
						break;
					case graph::NodeScope::View:
						compiled.PerView.push_back(scheduled.Node);
						break;
					case graph::NodeScope::Frame:
						if (std::find(setup.begin(), setup.end(), scheduled.Node) != setup.end()) {
							compiled.Shared.push_back(scheduled.Node);
						} else {
							compiled.Final.push_back(scheduled.Node);
						}
						break;
					}
				}
			}

			NodeTable available = BackendTable([](const graph::RunContext &) { return true; });
			for (const core::Name kind : customKinds) {
				available.Set(kind, [](const graph::RunContext &) { return true; });
			}
			const std::vector<core::Name> missing = available.Missing(pipeline);
			if (!missing.empty()) {
				stage = PipelineAdmissionStage::Backend;
				offender = missing.front();
				reason = "the renderer has no backend node for this kind";
				return false;
			}

			std::unordered_set<uint32_t> seen;
			for (const graph::ExecutionWave &wave : schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					const graph::Node *node = pipeline.Find(scheduled.Node);
					if (node == nullptr) {
						offender = {};
						reason = "the compiled schedule names no node";
						return false;
					}

					const graph::NodeKindSpec *spec = graph::NodeCatalogue::Find(node->Kind);
					if (spec == nullptr) {
						offender = node->Name;
						reason = "the render catalogue has no declaration for this kind";
						return false;
					}
					if (node->Kind == core::Name("shadow") &&
						!DependsOnKind(pipeline, scheduled.Node, core::Name("mesh-residency"))) {
						offender = node->Name;
						reason = "shadow work has no graph dependency on mesh-residency";
						return false;
					}
					if (node->Kind == core::Name("delta-upload") &&
						!DependsOnKind(pipeline, scheduled.Node, core::Name("mesh-residency"))) {
						offender = node->Name;
						reason = "delta-upload has no graph dependency on mesh-residency";
						return false;
					}
					const bool consumesInstanceUploads =
						node->Kind == core::Name("select-lod") ||
						node->Kind == core::Name("surface-capture") ||
						node->Kind == core::Name("mirror-capture") ||
						node->Kind == core::Name("portal-capture") || node->Kind == core::Name("gbuffer") ||
						node->Kind == core::Name("depth-peel") || node->Kind == core::Name("forward") ||
						node->Kind == core::Name("portal-overlay") ||
						node->Kind == core::Name("mirror-overlay") ||
						node->Kind == core::Name("transparent") ||
						node->Kind == core::Name("transparent-layer");
					if (consumesInstanceUploads &&
						!DependsOnKind(pipeline, scheduled.Node, core::Name("delta-upload"))) {
						offender = node->Name;
						reason = "draw work has no graph dependency on delta-upload";
						return false;
					}
					if (node->Kind == core::Name("raster") || node->Kind == core::Name("dispatch")) {
						const std::string *attachment = node->Parameter(core::Name("attachment"));
						if (attachment != nullptr && *attachment == "visual" &&
							(node->Reads.size() != 1 || node->Writes.size() != 1)) {
							offender = node->Name;
							reason = "a visual attachment needs one source and one target for inactive "
									 "pass-through";
							return false;
						}
					}
					graph::NodeRequirements needs = spec->Needs;
					if (node->Kind == core::Name("ambient-correct")) {
						size_t shadowInputs = 0;
						for (const char *port :
							 {"directional-response", "room-depth", "room-normal", "shadow"})
							shadowInputs +=
								std::find(node->ReadPorts.begin(), node->ReadPorts.end(), core::Name(port)) !=
								node->ReadPorts.end();
						if (shadowInputs != 0 && shadowInputs != 4) {
							offender = node->Name;
							reason = "directional correction requires response, room depth, room normal and "
									 "shadow together";
							return false;
						}
					}
					if (node->Kind == core::Name("deferred-lighting")) {
						for (size_t output = 0; output < node->Writes.size(); ++output) {
							const auto later = node->Writes.begin() + static_cast<std::ptrdiff_t>(output + 1);
							if (std::find(later, node->Writes.end(), node->Writes[output]) !=
								node->Writes.end()) {
								offender = node->Name;
								reason = "deferred-lighting outputs require distinct resources";
								return false;
							}
						}
						const auto hasOutput = [&](const char *name) {
							return std::find(
									   node->WritePorts.begin(), node->WritePorts.end(), core::Name(name)
								   ) != node->WritePorts.end();
						};
						const bool baseline = hasOutput("lighting-baseline");
						if (hasOutput("directional-response") && !baseline) {
							offender = node->Name;
							reason = "directional-response requires lighting-baseline";
							return false;
						}
						if (baseline) needs.Formats.push_back(graph::ResourceFormat::RGBA32F);
					}
					if (node->Kind == core::Name("pack-channels")) {
						const std::array ports{"r", "g", "b", "a"};
						if (node->Reads.size() != ports.size() || node->Writes.size() != 1 ||
							node->ReadPorts.size() != ports.size() ||
							node->WritePorts != std::vector{core::Name("packed")}) {
							offender = node->Name;
							reason = "pack-channels needs r, g, b and a inputs plus one packed output";
							return false;
						}
						const auto sampleable = [](graph::ResourceFormat format) {
							return format != graph::ResourceFormat::R32U &&
								   format != graph::ResourceFormat::D24S8 &&
								   format != graph::ResourceFormat::D32F &&
								   format != graph::ResourceFormat::BC1_SRGB &&
								   format != graph::ResourceFormat::BC3 &&
								   format != graph::ResourceFormat::BC5 &&
								   format != graph::ResourceFormat::BC7_SRGB;
						};
						for (size_t index = 0; index < ports.size(); ++index) {
							if (node->ReadPorts[index] != core::Name(ports[index]) ||
								std::find(node->Reads.begin(), node->Reads.end(), node->Writes.front()) !=
									node->Reads.end()) {
								offender = node->Name;
								reason = "pack-channels input cannot alias its output";
								return false;
							}
							const graph::ResourceDesc *source = pipeline.FindResource(node->Reads[index]);
							const std::string *component =
								node->Parameter(core::Name(std::string(ports[index]) + "-component"));
							if (component != nullptr &&
								(component->size() != 1 || (*component)[0] < '0' || (*component)[0] > '3')) {
								offender = node->Name;
								reason = "pack-channels component must be an integer from 0 through 3";
								return false;
							}
							const uint32_t selected =
								component == nullptr ? 0 : uint32_t((*component)[0] - '0');
							if (source == nullptr || !sampleable(source->Format) ||
								selected >= graph::ChannelCount(source->Format)) {
								offender = node->Name;
								reason = "pack-channels selector exceeds source components or source is not "
										 "float sampled";
								return false;
							}
						}
						const graph::ResourceDesc *target = pipeline.FindResource(node->Writes.front());
						if (target == nullptr || target->Format != graph::ResourceFormat::RGBA32F) {
							offender = node->Name;
							reason = "pack-channels output must be RGBA32F";
							return false;
						}
						needs.Formats.push_back(graph::ResourceFormat::RGBA32F);
					}
					if (node->Kind == core::Name("blit")) {
						if (node->Reads.size() != 1 || node->Writes.size() != 1) {
							offender = node->Name;
							reason = "blit needs exactly one source and one target";
							return false;
						}
						graph::ResourceFormat targetFormat = graph::ResourceFormat::RGBA16F;
						if (const std::string *format = node->Parameter(core::Name("format"));
							format != nullptr && !graph::ParseResourceFormat(*format, targetFormat)) {
							offender = node->Name;
							reason = "blit target format is not recognised";
							return false;
						}
						const graph::ResourceDesc *target = pipeline.FindResource(node->Writes.front());
						if (target == nullptr || target->Format != targetFormat) {
							offender = node->Name;
							reason = "blit target resource does not use its selected format";
							return false;
						}
						needs.Formats = {targetFormat};
					}
					if (caps != nullptr) {
						const CapabilityCheck capability = CheckCapabilities(*caps, needs);
						if (!capability.Accepted()) {
							stage = PipelineAdmissionStage::Capability;
							offender = node->Name;
							reason = Describe(capability.Status);
							if (capability.Status == CapabilityStatus::MissingFormat) {
								reason += ": ";
								reason += graph::Describe(capability.Format);
							}
							return false;
						}
					}
					const std::string *background = node->Parameter(core::Name("background"));
					const bool separateDepth = node->Kind == core::Name("depth-linearise") &&
											   background != nullptr && *background == "zero";
					if (seen.contains(node->Kind.Id()) && !spec->Repeatable && !separateDepth) {
						offender = node->Name;
						reason = "this render node kind may appear only once";
						return false;
					}
					if (node->Scope != spec->Scope && !spec->FlexibleScope) {
						offender = node->Name;
						reason = "this backend node cannot run at the authored scope";
						return false;
					}
					// Zero-background exports own graph targets; lighting depth remains a singleton.
					if (!separateDepth) {
						seen.insert(node->Kind.Id());
					}
					if (const std::string *queue = node->Parameter(core::Name("queue"));
						queue != nullptr && *queue != "auto" && *queue != graph::Describe(spec->Queue)) {
						offender = node->Name;
						reason = "this backend node requires the " +
								 std::string(graph::Describe(spec->Queue)) + " queue";
						return false;
					}
					if (const std::string *culling = node->Parameter(core::Name("culling"));
						culling != nullptr) {
						if (*culling != "inherit" && node->Kind != core::Name("cull-frustum")) {
							offender = node->Name;
							reason = "culling belongs on an entity filter node, not this backend node";
							return false;
						}
						// Occlusion is accepted since the backend grew its depth
						// pyramid and indirect draw path, but it composes behind
						// the gbuffer pass: that pass's early phase is what
						// seeds the pyramid the cull tests against, so a
						// document without it authored a cull nothing can feed.
						if (*culling == "occlusion") {
							bool depthWriter = false;
							for (uint32_t value = 1; value <= pipeline.Count() && !depthWriter; value++) {
								const graph::Node *writer = pipeline.Find(graph::NodeId{value});
								depthWriter = writer != nullptr && writer->Enabled &&
											  writer->Kind == core::Name("gbuffer");
							}
							if (!depthWriter) {
								offender = node->Name;
								reason = "occlusion culling needs the gbuffer pass to seed its "
										 "depth pyramid";
								return false;
							}
						}
					}
				}
			}
			return true;
		}

		std::vector<graph::NodeId>
		EntityNodesOf(const graph::RenderGraph &pipeline, const graph::CompiledGraph &compiled) {
			std::vector<graph::NodeId> nodes;
			nodes.reserve(compiled.PerView.size());
			for (const graph::NodeId id : compiled.PerView) {
				const graph::Node *node = pipeline.Find(id);
				if (node == nullptr) {
					continue;
				}
				const bool producesEntityFlow =
					std::any_of(node->Writes.begin(), node->Writes.end(), [&](graph::ResourceId resource) {
						const graph::ResourceDesc *desc = pipeline.FindResource(resource);
						return desc != nullptr && (desc->Kind == graph::ResourceKind::Entities ||
												   desc->Kind == graph::ResourceKind::Camera);
					});
				if (producesEntityFlow) {
					nodes.push_back(id);
				}
			}
			return nodes;
		}

		struct RetainedNodePlan {
			std::vector<uint8_t> Nodes;
			uint16_t Families = 0;
		};

		template <size_t Size> bool KindIn(core::Name kind, const std::array<std::string_view, Size> &kinds) {
			return std::find(kinds.begin(), kinds.end(), kind.Text()) != kinds.end();
		}

		uint16_t RetainedFamiliesOf(const graph::RenderGraph &pipeline, std::span<const uint8_t> retained) {
			static constexpr std::array uploadKinds{
				std::string_view("world"),
				std::string_view("camera"),
				std::string_view("entities"),
				std::string_view("cull-frustum"),
				std::string_view("cull-distance"),
				std::string_view("filter-tag"),
				std::string_view("order-draw"),
				std::string_view("mesh-residency"),
				std::string_view("delta-upload"),
				std::string_view("select-lod")
			};
			static constexpr std::array shadowKinds{std::string_view("shadow")};
			static constexpr std::array mirrorKinds{
				std::string_view("mirror-capture"), std::string_view("mirror-overlay")
			};
			static constexpr std::array portalKinds{
				std::string_view("portal-capture"),
				std::string_view("portal-tonemap"),
				std::string_view("portal-overlay")
			};
			static constexpr std::array surfaceKinds{std::string_view("surface-capture")};
			static constexpr std::array authoredKinds{
				std::string_view("raster"),
				std::string_view("fxaa"),
				std::string_view("taa"),
				std::string_view("smaa-edges"),
				std::string_view("smaa-blend"),
				std::string_view("smaa-resolve"),
				std::string_view("exposure-grade"),
				std::string_view("hsv"),
				std::string_view("mix"),
				std::string_view("transform-crop"),
				std::string_view("blur"),
				std::string_view("dispatch"),
				std::string_view("tessellate"),
				std::string_view("global-illumination"),
				std::string_view("raytrace"),
				std::string_view("pathtrace")
			};
			static constexpr std::array shadingKinds{
				std::string_view("ambient-response"),
				std::string_view("ambient-merge"),
				std::string_view("ambient-correct"),
				std::string_view("colour-compose"),
				std::string_view("depth-compose"),
				std::string_view("last-frame"),
				std::string_view("blit"),
				std::string_view("depth-linearise"),
				std::string_view("depth-validity"),
				std::string_view("camera-motion"),
				std::string_view("hzb"),
				std::string_view("ssao"),
				std::string_view("deferred-lighting"),
				std::string_view("skybox-compute"),
				std::string_view("clouds-compute"),
				std::string_view("sky"),
				std::string_view("fog"),
				std::string_view("shader-lenses"),
				std::string_view("tonemap")
			};
			static constexpr std::array geometryKinds{
				std::string_view("tessellate"),
				std::string_view("tessellated-draw"),
				std::string_view("forward"),
				std::string_view("gbuffer"),
				std::string_view("depth-peel"),
				std::string_view("transparent-layer"),
				std::string_view("transparent")
			};

			uint16_t families = 0;
			for (uint32_t value = 1; value <= pipeline.Count(); ++value) {
				if (value > retained.size() || retained[value - 1] == 0) continue;
				const graph::Node *node = pipeline.Find(graph::NodeId{value});
				if (node == nullptr) continue;
				if (KindIn(node->Kind, uploadKinds)) families |= RetainedUpload;
				if (KindIn(node->Kind, shadowKinds)) families |= RetainedShadow;
				if (KindIn(node->Kind, mirrorKinds)) families |= RetainedMirror;
				if (KindIn(node->Kind, portalKinds)) families |= RetainedPortal;
				if (KindIn(node->Kind, surfaceKinds)) families |= RetainedSurface;
				if (KindIn(node->Kind, authoredKinds)) families |= RetainedAuthored;
				if (KindIn(node->Kind, shadingKinds)) families |= RetainedShading;
				if (KindIn(node->Kind, geometryKinds)) families |= RetainedGeometry;
			}
			return families;
		}

		RetainedNodePlan
		RetainedNodesOf(const graph::RenderGraph &pipeline, std::span<const core::Name> customKinds) {
			std::vector<uint8_t> retained(pipeline.Count(), 0);
			std::vector<uint8_t> liveResources(pipeline.ResourceCount() + 1, 0);

			bool changed = true;
			while (changed) {
				changed = false;
				for (uint32_t value = 1; value <= pipeline.Count(); ++value) {
					const graph::Node *node = pipeline.Find(graph::NodeId{value});
					if (node == nullptr || !node->Enabled || retained[value - 1] != 0) continue;
					const graph::NodeKindSpec *kind = graph::NodeCatalogue::Find(node->Kind);
					const bool output = kind != nullptr && kind->Category == graph::NodeCategory::Output;
					const bool custom =
						std::find(customKinds.begin(), customKinds.end(), node->Kind) != customKinds.end();
					// Path tracing advances its accumulation even when the scene is unchanged.
					// Other history resources are caches and do not make their writers temporal.
					const bool temporal = node->Kind == core::Name("pathtrace");
					const bool consumesLive =
						std::any_of(node->Reads.begin(), node->Reads.end(), [&](graph::ResourceId resource) {
							return resource.Value < liveResources.size() &&
								   liveResources[resource.Value] != 0;
						});
					if (!output && !custom && !temporal && !consumesLive) continue;
					retained[value - 1] = 1;
					changed = true;
					for (const graph::ResourceId resource : node->Writes) {
						if (resource.Value < liveResources.size()) liveResources[resource.Value] = 1;
					}
				}
			}
			return {retained, RetainedFamiliesOf(pipeline, retained)};
		}

	}

	Renderer::Impl::PipelineCompilation Renderer::Impl::CompilePipeline(
		core::Name name,
		const graph::RenderGraph &pipeline,
		const DeviceCaps *caps,
		std::span<const core::Name> customKinds
	) {
		graph::CompiledGraph compiled;
		graph::ExecutionSchedule schedule;
		PipelineCompilation result;
		if (!ValidatePipeline(
				pipeline,
				compiled,
				schedule,
				result.Failure.Offender,
				result.Failure.Reason,
				result.Failure.Stage,
				caps,
				customKinds
			)) {
			return result;
		}

		RetainedNodePlan retained = RetainedNodesOf(pipeline, customKinds);
		std::vector<graph::NodeId> entityNodes = EntityNodesOf(pipeline, compiled);
		graph::ResourceAliasPlan aliases = graph::BuildResourceAliases(pipeline, compiled);
		std::vector<graph::PlannedCommandBuffer> buffers = graph::PlanCommandBuffers(schedule);
		result.Package = InstalledPipeline{
			.Name = name,
			.Graph = pipeline,
			.Compiled = std::move(compiled),
			.EntityNodes = std::move(entityNodes),
			.RetainedNodes = std::move(retained.Nodes),
			.RetainedFamilies = retained.Families,
			.Schedule = std::move(schedule),
			.Aliases = std::move(aliases),
			.Buffers = std::move(buffers),
		};
		return result;
	}

}
