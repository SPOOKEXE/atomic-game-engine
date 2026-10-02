#pragma once

#include <engine/imagegraph/FeedbackReplay.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>

#include <algorithm>
#include <array>
#include <new>

namespace engine::imagegraph {
	namespace feedback_detail {
		// Only an upstream rigid dependency can change a simulation's inputs during
		// observation refresh. A simulation feeding a rigid texture remains reusable.
		inline bool RigidFeedsSimulation(
			const Document &document,
			const Plan &plan,
			std::span<const std::string> outputs,
			std::string_view selectedNode,
			std::span<const std::string_view> actions
		) {
			struct Visit {
				size_t Index;
				bool Simulation;
			};
			std::array<std::array<bool, 2>, Limits::MaximumNodes> visited{};
			std::array<Visit, 2 * Limits::MaximumNodes> pending{};
			size_t count = 0;
			bool invalid = document.Nodes.size() > Limits::MaximumNodes ||
						   outputs.size() > Limits::MaximumOutputs || actions.size() > Limits::MaximumNodes;
			if (invalid) return true;
			const auto add = [&](size_t index, bool simulation) {
				if (index >= document.Nodes.size() || index >= Limits::MaximumNodes) {
					invalid = true;
					return;
				}
				if (!visited[index][simulation]) {
					visited[index][simulation] = true;
					pending[count++] = {index, simulation};
				}
			};
			const auto addNode = [&](std::string_view id, bool simulation) {
				for (size_t i = 0; i < document.Nodes.size(); ++i)
					if (document.Nodes[i].Id == id) {
						add(i, simulation);
						return;
					}
				invalid = true;
			};
			if (!selectedNode.empty()) addNode(selectedNode, false);
			for (const auto id : actions)
				addNode(id, false);
			for (const auto &id : outputs) {
				const auto found =
					std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &output) {
						return output.Id == id;
					});
				if (found == document.Outputs.end()) {
					invalid = true;
					continue;
				}
				addNode(found->NodeId, false);
			}
			while (count && !invalid) {
				const auto current = pending[--count];
				const auto &node = document.Nodes[current.Index];
				if (current.Simulation && node.Type.starts_with("pc.rigid_")) return true;
				const bool simulation = current.Simulation || node.Type == "image.verlet_simple" ||
										node.Type.starts_with("pc.verlet_") ||
										node.Type.starts_with("pc.flip_");
				for (const auto &link : plan.EffectiveLinks)
					if (link.ToNode == node.Id) addNode(link.FromNode, simulation);
				for (const auto &edge : plan.GroupSurfaceDependencies)
					if (edge.Consumer == current.Index) add(edge.Producer, simulation);
				for (const auto &edge : plan.InlineOwnerDependencies)
					if (edge.Consumer == current.Index && !edge.ControlsOnly) add(edge.Owner, simulation);
				for (const auto &edge : plan.InlineControlDependencies)
					if (edge.Consumer == current.Index) add(edge.Producer, simulation);
				for (const auto &edge : plan.PcxNamedDependencies)
					if (edge.Consumer == current.Index) add(edge.Producer, simulation);
			}
			return invalid;
		}
	}
	// One host owns feedback generations and source processor state. All selected/bound outputs share
	// one evaluated closure per tick, so a simulation or cached processor executes once.
	class CapturedFeedbackHost {
		std::vector<FeedbackBinding> Bindings;
		std::vector<RequestImageSource> Seeds, Inputs;
		StatefulOutputEvaluationResult State;
		EvaluationSnapshot InputSnapshot;
		std::string InputNode;
		uint64_t DocumentRevision = 0, InputRevision = 0, Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		bool RigidPlaying = false, RigidFrameProgress = false;
		bool Configured = false, Initialized = false, Stateful = false, HaveExternalSources = false;

		static uint64_t SourceBytes(const std::vector<RequestImageSource> &sources) {
			uint64_t bytes = sources.capacity() * sizeof(RequestImageSource);
			for (const auto &source : sources)
				bytes += source.SourceId.capacity() + source.Data.Pixels.capacity();
			return bytes;
		}
		static uint64_t OutputBytes(const StatefulOutputEvaluationResult &state) {
			return RetainedStatefulOutputBytes(state) + RetainedSimulationReplayBytes(state.Simulation) +
				   RetainedSurfaceFrameReplayBytes(state.Surfaces) + RetainedRandomReplayBytes(state.Random) +
				   RetainedDataReplayBytes(state.Data) + RetainedRigidReplayBytes(state.Rigid);
		}
		static const Image *ImageOutput(const StatefulNamedOutput &output) {
			if (const auto *image = std::get_if<Image>(&output.Output)) return image;
			if (const auto *array = std::get_if<ImageArray>(&output.Output)) {
				if (array->Items.size() == 1)
					if (const auto *index = std::get_if<size_t>(&array->Items.front().Data);
						index && *index < array->Images.size())
						return &array->Images[*index];
			}
			return nullptr;
		}
		static bool StateNode(const Node &node) {
			return node.Type == "pc.interlaced" || node.Type == "image.verlet_simple" ||
				   node.Type.starts_with("pc.verlet_") || node.Type.starts_with("pc.flip_");
		}

	  public:
		bool Active() const {
			return Stateful || !Bindings.empty();
		}
		// Native playback restart reconstructs tick zero while retaining source cache controls.
		// Provider sessions and authored revisions remain owned by their existing callers.
		void RestartCycle() {
			Initialized = false;
		}
		void Clear() {
			Bindings = {};
			Seeds = {};
			Inputs = {};
			State = {};
			InputSnapshot = {};
			InputNode.clear();
			DocumentRevision = InputRevision = Tick = 0;
			Subframe = 0;
			NegativeFrame = RigidPlaying = RigidFrameProgress = false;
			Configured = Initialized = Stateful = HaveExternalSources = false;
		}
		bool Prepare(
			const Document &document,
			const Plan &plan,
			uint64_t revision,
			uint64_t externalRevision,
			EvaluationRequest &request,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes,
			std::string_view selectedOutput = {},
			std::string_view selectedNode = {}
		) try {
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, {}, {}, message};
				return false;
			};
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
				return fail(Status::LimitExceeded, "feedback host byte cap is outside bounds");
			const bool changed =
				!Configured || DocumentRevision != revision || InputRevision != externalRevision;
			std::vector<FeedbackBinding> declarations;
			std::vector<RequestImageSource> blanks;
			bool stateful = Stateful;
			if (changed) {
				stateful = std::any_of(document.Nodes.begin(), document.Nodes.end(), StateNode);
				for (const auto &node : document.Nodes)
					if (node.Type == "image.captured")
						for (const auto &value : node.Values) {
							const auto *source = std::get_if<std::string>(&value.Data);
							if (value.Port != "source_id" || !source || !source->starts_with("feedback:"))
								continue;
							if (std::none_of(
									declarations.begin(), declarations.end(), [&](const auto &binding) {
										return binding.SourceId == *source;
									}
								))
								declarations.push_back({*source, source->substr(9)});
						}
				const uint32_t width = document.Project ? document.Project->SurfaceWidth : 32,
							   height = document.Project ? document.Project->SurfaceHeight : 32;
				const auto layout = CheckedSurfaceLayout(
					width,
					height,
					SurfaceFormat::RGBA8Unorm,
					std::min<uint64_t>(16 * 1024 * 1024, maximumBytes)
				);
				if (!declarations.empty() &&
					(!layout || declarations.size() > Limits::MaximumOutputs ||
					 declarations.size() > std::min<uint64_t>(16 * 1024 * 1024, maximumBytes) /
											   (layout->Bytes + sizeof(RequestImageSource))))
					return fail(Status::LimitExceeded, "feedback blank seeds exceed byte bounds");
				blanks.reserve(declarations.size());
				for (const auto &binding : declarations) {
					Image image;
					image.Width = width;
					image.Height = height;
					image.Pixels.resize(layout->Bytes);
					image.Hash = SurfaceHash(image);
					blanks.push_back({binding.SourceId, std::move(image)});
				}
			}
			const auto &bindings = changed ? declarations : Bindings;
			const auto &seeds = changed ? blanks : Seeds;
			std::vector<std::string> outputs;
			for (const auto &binding : bindings)
				if (std::find(outputs.begin(), outputs.end(), binding.OutputId) == outputs.end())
					outputs.push_back(binding.OutputId);
			if (!selectedOutput.empty() &&
				std::find(outputs.begin(), outputs.end(), selectedOutput) == outputs.end())
				outputs.emplace_back(selectedOutput);
			const auto temporal = AnalyzeStatefulTemporalCone(
				document, plan, outputs, selectedNode, request.SimulationCacheCaptures
			);
			if (!temporal.Valid) return fail(Status::InvalidOutput, "stateful output cone is invalid");
			stateful = temporal.Simulation || temporal.SurfaceCaches != 0 || temporal.RandomGenerators != 0 ||
					   temporal.DataProcessors != 0 || !State.Simulation.Entries.empty() ||
					   !State.Surfaces.Entries.empty() || !State.Random.Entries.empty() ||
					   !State.Data.Entries.empty() || temporal.RigidActors != 0 ||
					   !State.Rigid.Owners.empty();
			const bool directData = temporal.DataProcessors != 0 && !temporal.Simulation &&
									!temporal.SurfaceCaches && !temporal.RandomGenerators &&
									!temporal.RigidActors && bindings.empty();
			if (!stateful && bindings.empty()) {
				if (changed || Stateful) {
					Clear();
					Configured = true;
					DocumentRevision = revision;
					InputRevision = externalRevision;
				}
				return true;
			}
			if ((request.NegativeFrame && !directData) ||
				((temporal.FixedSimulationSteps || temporal.SurfaceCaches || !bindings.empty()) &&
				 request.Subframe != 0))
				return fail(Status::InvalidValue, "stateful replay requires an integer nonnegative frame");
			if (outputs.empty() && selectedNode.empty())
				return fail(Status::InvalidOutput, "stateful replay requires a selected output");
			if (request.ImageSources.size() > Limits::MaximumNodes)
				return fail(Status::LimitExceeded, "too many external image captures");
			uint64_t externalBytes = 0;
			size_t externalCount = 0;
			for (const auto &source : request.ImageSources) {
				if (std::any_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
						return binding.SourceId == source.SourceId;
					}))
					continue;
				externalBytes +=
					sizeof(RequestImageSource) + source.SourceId.capacity() + source.Data.Pixels.capacity();
				++externalCount;
			}
			const bool sameSelection = InputNode == selectedNode && outputs.size() == State.Outputs.size() &&
									   std::equal(
										   outputs.begin(),
										   outputs.end(),
										   State.Outputs.begin(),
										   [](const auto &id, const auto &value) { return id == value.Id; }
									   );
			uint64_t configBytes = SourceBytes(seeds) + bindings.capacity() * sizeof(FeedbackBinding) +
								   outputs.capacity() * sizeof(std::string) +
								   std::max(selectedNode.size(), std::string{}.capacity());
			for (const auto &binding : bindings)
				configBytes += binding.SourceId.capacity() + binding.OutputId.capacity();
			for (const auto &id : outputs)
				configBytes += id.capacity();
			uint64_t oldConfigBytes =
				changed ? SourceBytes(Seeds) + Bindings.capacity() * sizeof(FeedbackBinding) : 0;
			if (changed)
				for (const auto &binding : Bindings)
					oldConfigBytes += binding.SourceId.capacity() + binding.OutputId.capacity();
			const uint64_t currentBytes = OutputBytes(State) + InputSnapshot.RetainedBytes() +
										  InputNode.capacity() + SourceBytes(Inputs) + configBytes +
										  oldConfigBytes + externalBytes;
			if (currentBytes >= maximumBytes)
				return fail(Status::LimitExceeded, "stateful host residency exceeds byte bounds");
			const bool rigidObservationChanged =
				temporal.RigidActors && Initialized &&
				(RigidPlaying != request.RigidPlaying || RigidFrameProgress != request.RigidFrameProgress);
			const bool refreshRigidObservations = !changed && Initialized && sameSelection &&
												  Tick == request.Tick && Subframe == request.Subframe &&
												  NegativeFrame == request.NegativeFrame &&
												  rigidObservationChanged;
			if (refreshRigidObservations && temporal.Simulation &&
				feedback_detail::RigidFeedsSimulation(
					document, plan, outputs, selectedNode, request.SimulationCacheCaptures
				))
				return fail(
					Status::UnsupportedExecution,
					"rigid observation refresh cannot reuse simulation inputs that depend on rigid output"
				);
			if (!changed && Initialized && !rigidObservationChanged && Tick == request.Tick &&
				Subframe == request.Subframe && NegativeFrame == request.NegativeFrame && sameSelection &&
				request.SimulationCacheCaptures.empty()) {
				if (!bindings.empty())
					request.ImageSources = Tick || HaveExternalSources
											   ? std::span<const RequestImageSource>(Inputs)
											   : std::span<const RequestImageSource>(Seeds);
				request.SimulationReplay = &State.Simulation;
				request.SurfaceReplay = &State.Surfaces;
				request.RandomReplay = &State.Random;
				request.DataReplay = &State.Data;
				request.RigidReplay = &State.Rigid;
				request.RigidAuthoringRevision = revision;
				return true;
			}
			const bool contiguous =
				directData ||
				(!changed && sameSelection && Initialized &&
				 ((Tick < Limits::MaximumTick && Tick + 1 == request.Tick) ||
				  (Tick == request.Tick && Subframe == request.Subframe &&
				   NegativeFrame == request.NegativeFrame &&
				   (!request.SimulationCacheCaptures.empty() || rigidObservationChanged)) ||
				  (temporal.RandomGenerators && !temporal.Simulation && !temporal.SurfaceCaches &&
				   bindings.empty() && Tick == request.Tick && request.Subframe > Subframe)));
			if (!contiguous && request.Tick > 4096)
				return fail(Status::LimitExceeded, "stateful seek exceeds the 4096-step bound");
			std::string candidateNode(selectedNode);
			StatefulOutputEvaluationResult candidate;
			EvaluationSnapshot candidateSnapshot;
			// Source interlace caches deliberately survive authored edits and backward seeks.
			const auto retainCache = [&](const SimulationReplayEntry &entry) {
				return entry.Cache && entry.State.AuthoringRevision == revision &&
					   std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
						   return node.Id == entry.NodeId && node.Type == "pc.verlet_sim_mesh_cache";
					   });
			};
			size_t retainedCaches = 0;
			uint64_t cacheCopyBytes = 0;
			if (!contiguous)
				for (const auto &entry : State.Simulation.Entries)
					if (retainCache(entry)) {
						++retainedCaches;
						cacheCopyBytes += RetainedSimulationEntryBytes(entry);
					}
			const uint64_t seedCopyBytes = RetainedSurfaceFrameReplayBytes(State.Surfaces) + cacheCopyBytes;
			if (!contiguous && seedCopyBytes > maximumBytes - currentBytes)
				return fail(Status::LimitExceeded, "stateful seek cache copy exceeds bounds");
			if (!contiguous) {
				candidate.Surfaces = State.Surfaces;
				candidate.Simulation.Entries.reserve(retainedCaches);
				for (const auto &entry : State.Simulation.Entries)
					if (retainCache(entry)) candidate.Simulation.Entries.push_back(entry);
			}
			std::vector<RequestImageSource> previous, inputs;
			const uint64_t first = contiguous ? request.Tick : 0;
			for (uint64_t tick = first; tick <= request.Tick; tick++) {
				EvaluationRequest clock = request;
				clock.Tick = tick;
				clock.ReuseSimulationFrame = refreshRigidObservations && temporal.Simulation;
				clock.Subframe = tick == request.Tick ? request.Subframe : 0;
				if (tick != request.Tick) clock.SimulationCacheCaptures = {};
				clock.SimulationAuthoringRevision = revision;
				clock.ResetSurfaceReplay = !contiguous && tick == 0;
				const auto &prior = contiguous ? State : candidate;
				clock.SimulationReplay = &prior.Simulation;
				clock.SurfaceReplay = &prior.Surfaces;
				clock.RandomReplay = &prior.Random;
				clock.DataReplay = &prior.Data;
				clock.RigidReplay = &prior.Rigid;
				clock.RigidAuthoringRevision = revision;
				const auto &generation = tick == 0 ? seeds : previous;
				if (contiguous) {
					const bool sameFrameAction =
						Tick == request.Tick && Subframe == request.Subframe &&
						NegativeFrame == request.NegativeFrame &&
						(!request.SimulationCacheCaptures.empty() || rigidObservationChanged);
					const auto sourceImage = [&](const FeedbackBinding &binding) -> const Image * {
						if (sameFrameAction) {
							const auto source =
								std::find_if(Inputs.begin(), Inputs.end(), [&](const auto &input) {
									return input.SourceId == binding.SourceId;
								});
							if (source != Inputs.end()) return &source->Data;
							if (Tick != 0) return nullptr;
							const auto seed =
								std::find_if(seeds.begin(), seeds.end(), [&](const auto &input) {
									return input.SourceId == binding.SourceId;
								});
							return seed == seeds.end() ? nullptr : &seed->Data;
						}
						const auto found =
							std::find_if(State.Outputs.begin(), State.Outputs.end(), [&](const auto &output) {
								return output.Id == binding.OutputId;
							});
						return found == State.Outputs.end() ? nullptr : ImageOutput(*found);
					};
					uint64_t copyBytes = bindings.size() * sizeof(RequestImageSource);
					for (const auto &binding : bindings) {
						const Image *image = sourceImage(binding);
						if (!image)
							return fail(Status::InvalidOutput, "feedback output must contain one image");
						copyBytes += binding.SourceId.capacity() + image->Pixels.capacity();
					}
					if (copyBytes > maximumBytes - currentBytes)
						return fail(Status::LimitExceeded, "feedback generation copy exceeds bounds");
					previous.reserve(bindings.size());
					for (const auto &binding : bindings)
						previous.push_back({binding.SourceId, *sourceImage(binding)});
				}
				std::vector<RequestImageSource> captures;
				const uint64_t captureBytes =
					externalCount && !bindings.empty() ? externalBytes + SourceBytes(generation) : 0;
				if (OutputBytes(candidate) + SourceBytes(previous) + SourceBytes(inputs) + captureBytes >
					maximumBytes - currentBytes)
					return fail(Status::LimitExceeded, "combined capture copy exceeds bounds");
				if (!bindings.empty()) {
					if (externalCount) {
						captures.reserve(generation.size() + externalCount);
						captures.insert(captures.end(), generation.begin(), generation.end());
						for (const auto &source : request.ImageSources)
							if (std::none_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
									return binding.SourceId == source.SourceId;
								}))
								captures.push_back(source);
						clock.ImageSources = captures;
					} else
						clock.ImageSources = generation;
				}
				const uint64_t held = configBytes + oldConfigBytes + externalBytes + OutputBytes(State) +
									  InputSnapshot.RetainedBytes() + candidateSnapshot.RetainedBytes() +
									  SourceBytes(Inputs) + SourceBytes(previous) + SourceBytes(captures) +
									  SourceBytes(inputs);
				if (held >= maximumBytes)
					return fail(Status::LimitExceeded, "stateful generation overlap exceeds byte bounds");
				if (selectedNode.empty()) {
					if (EvaluateStatefulOutputs(
							document, plan, outputs, clock, candidate, diagnostic, maximumBytes - held
						) != Status::Ok)
						return false;
				} else {
					StatefulInputEvaluationResult captured;
					if (EvaluateStatefulNodeInputs(
							document,
							plan,
							selectedNode,
							clock,
							captured,
							diagnostic,
							maximumBytes - held,
							outputs
						) != Status::Ok)
						return false;
					candidate.Outputs = std::move(captured.Outputs);
					candidate.Simulation = std::move(captured.Simulation);
					candidate.Surfaces = std::move(captured.Surfaces);
					candidate.Random = std::move(captured.Random);
					candidate.Data = std::move(captured.Data);
					candidate.Rigid = std::move(captured.Rigid);
					candidateSnapshot = std::move(captured.Inputs);
				}
				inputs = externalCount && !bindings.empty() ? std::move(captures) : std::move(previous);
				previous.clear();
				if (tick != request.Tick) {
					inputs.clear();
					const uint64_t resident = currentBytes + OutputBytes(candidate) +
											  candidateSnapshot.RetainedBytes() + SourceBytes(inputs);
					uint64_t copyBytes = bindings.size() * sizeof(RequestImageSource);
					for (const auto &binding : bindings) {
						const auto found = std::find_if(
							candidate.Outputs.begin(), candidate.Outputs.end(), [&](const auto &output) {
								return output.Id == binding.OutputId;
							}
						);
						const Image *image = found == candidate.Outputs.end() ? nullptr : ImageOutput(*found);
						if (!image)
							return fail(Status::InvalidOutput, "feedback output must contain one image");
						copyBytes += binding.SourceId.capacity() + image->Pixels.capacity();
					}
					if (resident >= maximumBytes || copyBytes > maximumBytes - resident)
						return fail(Status::LimitExceeded, "feedback seek generation copy exceeds bounds");
					previous.reserve(bindings.size());
					for (const auto &binding : bindings) {
						const auto found = std::find_if(
							candidate.Outputs.begin(), candidate.Outputs.end(), [&](const auto &output) {
								return output.Id == binding.OutputId;
							}
						);
						const Image *image = found == candidate.Outputs.end() ? nullptr : ImageOutput(*found);
						if (!image)
							return fail(Status::InvalidOutput, "feedback output must contain one image");
						previous.push_back({binding.SourceId, *image});
					}
				}
			}
			// Admit the retained preceding generation before its final copy/publication.
			if (request.Tick == 0 && !externalCount) inputs.clear();
			if (changed) {
				Bindings = std::move(declarations);
				Seeds = std::move(blanks);
			}
			State = std::move(candidate);
			InputSnapshot = std::move(candidateSnapshot);
			InputNode = std::move(candidateNode);
			Inputs = std::move(inputs);
			DocumentRevision = revision;
			InputRevision = externalRevision;
			Tick = request.Tick;
			Subframe = request.Subframe;
			NegativeFrame = request.NegativeFrame;
			RigidPlaying = request.RigidPlaying;
			RigidFrameProgress = request.RigidFrameProgress;
			Stateful = stateful;
			HaveExternalSources = externalCount != 0;
			Configured = Initialized = true;
			if (!Bindings.empty())
				request.ImageSources = Tick || HaveExternalSources
										   ? std::span<const RequestImageSource>(Inputs)
										   : std::span<const RequestImageSource>(Seeds);
			request.SimulationReplay = &State.Simulation;
			request.SurfaceReplay = &State.Surfaces;
			request.RandomReplay = &State.Random;
			request.DataReplay = &State.Data;
			request.RigidReplay = &State.Rigid;
			request.RigidAuthoringRevision = revision;
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "stateful host allocation was refused"};
			return false;
		}
		bool PrepareNodeInputs(
			const Document &document,
			const Plan &plan,
			uint64_t revision,
			uint64_t externalRevision,
			std::string_view nodeId,
			EvaluationRequest &request,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			return Prepare(
				document, plan, revision, externalRevision, request, diagnostic, maximumBytes, {}, nodeId
			);
		}
		const EvaluationSnapshot &Snapshot() const {
			return InputSnapshot;
		}
		const StatefulNamedOutput *Value(std::string_view outputId) const {
			for (const auto &output : State.Outputs)
				if (output.Id == outputId) return &output;
			return nullptr;
		}
		const Image *Output(std::string_view outputId) const {
			const auto *value = Value(outputId);
			return value ? ImageOutput(*value) : nullptr;
		}
	};
}
