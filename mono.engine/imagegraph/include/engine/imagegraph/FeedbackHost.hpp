#pragma once

#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/FeedbackReplay.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/SourceFrameCacheProject.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace engine::imagegraph {
	namespace feedback_detail {
		// Lua VM state belongs to its provider session, outside the replay checkpoint.
		inline bool RefreshTouchesLua(
			const Document &document,
			const Plan &plan,
			std::span<const std::string> outputs,
			std::string_view selectedNode,
			std::span<const std::string_view> actions
		) {
			if (document.Nodes.size() > Limits::MaximumNodes || outputs.size() > Limits::MaximumOutputs ||
				actions.size() > Limits::MaximumNodes)
				return true;
			std::array<bool, Limits::MaximumNodes> visited{};
			std::array<size_t, Limits::MaximumNodes> pending{};
			size_t count = 0;
			bool invalid = false;
			const auto add = [&](size_t index) {
				if (index >= document.Nodes.size()) {
					invalid = true;
					return;
				}
				if (!visited[index]) {
					visited[index] = true;
					pending[count++] = index;
				}
			};
			const auto addNode = [&](std::string_view id) {
				for (size_t i = 0; i < document.Nodes.size(); ++i)
					if (document.Nodes[i].Id == id) {
						add(i);
						return;
					}
				invalid = true;
			};
			if (!selectedNode.empty()) addNode(selectedNode);
			for (auto id : actions)
				addNode(id);
			for (const auto &id : outputs) {
				const auto output =
					std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &item) {
						return item.Id == id;
					});
				if (output == document.Outputs.end())
					invalid = true;
				else
					addNode(output->NodeId);
			}
			while (count && !invalid) {
				const auto current = pending[--count];
				const auto &node = document.Nodes[current];
				if (node.Type.starts_with("pc.lua_")) return true;
				for (const auto &link : plan.EffectiveLinks)
					if (link.ToNode == node.Id) addNode(link.FromNode);
				for (const auto &edge : plan.GroupSurfaceDependencies)
					if (edge.Consumer == current) add(edge.Producer);
				for (const auto &edge : plan.InlineOwnerDependencies)
					if (edge.Consumer == current) add(edge.Owner);
				for (const auto &edge : plan.InlineControlDependencies)
					if (edge.Consumer == current) add(edge.Producer);
				for (const auto &edge : plan.PcxNamedDependencies)
					if (edge.Consumer == current) add(edge.Producer);
			}
			return invalid;
		}

	} // namespace feedback_detail
	// One host owns feedback generations and source processor state. All
	// selected/bound outputs share one evaluated closure per tick, so a simulation
	// or cached processor executes once.
	class CapturedFeedbackHost {
		std::vector<FeedbackBinding> Bindings;
		std::vector<std::string> CacheClearNodes, CacheInvalidOutputs, CacheInvalidInputs;
		FrameTime CacheClearClock{};
		static uint64_t NamesBytes(const std::vector<std::string> &names) {
			uint64_t bytes = names.capacity() * sizeof(std::string);
			for (const auto &name : names)
				bytes += name.capacity();
			return bytes;
		}
		uint64_t ClearMetadataBytes() const {
			return NamesBytes(CacheClearNodes) + NamesBytes(CacheInvalidOutputs) +
				   NamesBytes(CacheInvalidInputs);
		}
		static constexpr uint64_t CLEAR_WORK_LIMIT = 64ull * 1024 * 1024;
		static bool AdmitClearWork(uint64_t &remaining, uint64_t count, uint64_t perItem) {
			if (count && perItem > remaining / count) return false;
			remaining -= count * perItem;
			return true;
		}
		template <class Items, class Name> static uint64_t ClearNameScanWork(const Items &items, Name name) {
			uint64_t remaining = CLEAR_WORK_LIMIT;
			for (const auto &item : items) {
				const auto &text = name(item);
				if (text.size() >= remaining) return UINT64_MAX;
				remaining -= text.size() + 1;
			}
			return CLEAR_WORK_LIMIT - remaining;
		}
		static uint64_t ClearNamesScanWork(const std::vector<std::string> &names) {
			return ClearNameScanWork(names, [](const auto &name) -> const auto & { return name; });
		}
		bool OverlayClearedRows(
			const Document &document, DataReplayState &target, uint64_t maximumBytes, Diagnostic &diagnostic
		) const {
			const auto failWork = [&] {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "Cache Results clear overlay exceeds work bounds"
				};
				return false;
			};
			uint64_t work = CLEAR_WORK_LIMIT;
			const uint64_t names = ClearNamesScanWork(CacheClearNodes);
			// Admit membership scans before the first tracked-row comparison.
			if (!AdmitClearWork(work, State.Data.Entries.size(), names)) return failWork();
			const auto tracked = [&](const auto &entry) {
				return std::find(CacheClearNodes.begin(), CacheClearNodes.end(), entry.NodeId) !=
					   CacheClearNodes.end();
			};
			const uint64_t matching =
				std::count_if(State.Data.Entries.begin(), State.Data.Entries.end(), tracked);
			const uint64_t documentNames =
				ClearNameScanWork(document.Nodes, [](const auto &node) -> const auto & { return node.Id; });
			const uint64_t targetNames =
				ClearNameScanWork(target.Entries, [](const auto &entry) -> const auto & {
					return entry.NodeId;
				});
			const uint64_t sourceNames =
				ClearNameScanWork(State.Data.Entries, [](const auto &entry) -> const auto & {
					return entry.NodeId;
				});
			if (!AdmitClearWork(work, State.Data.Entries.size() * 2, names) ||
				!AdmitClearWork(work, matching * 2, documentNames) ||
				!AdmitClearWork(work, matching * 2, sizeof("pc.cache_results")) ||
				!AdmitClearWork(work, matching * 2, targetNames) ||
				!AdmitClearWork(work, matching * 2, sourceNames))
				return failWork();
			const auto retainedNode = [&](const auto &entry) {
				return tracked(entry) &&
					   std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
						   return node.Id == entry.NodeId && node.Type == "pc.cache_results";
					   });
			};
			size_t count = 0;
			uint64_t copies = 0;
			for (const auto &entry : State.Data.Entries)
				if (retainedNode(entry)) {
					if (std::none_of(target.Entries.begin(), target.Entries.end(), [&](const auto &item) {
							return item.NodeId == entry.NodeId && item.ProcessorRow == entry.ProcessorRow;
						}))
						++count;
					copies += RetainedDataReplayEntryBytes(entry);
				}
			const uint64_t retained = RetainedDataReplayBytes(target);
			const uint64_t slots = (target.Entries.size() + count) * sizeof(DataReplayEntry);
			if (count + target.Entries.size() > Limits::MaximumArrayElements || retained > maximumBytes ||
				copies > maximumBytes - retained || slots > maximumBytes - retained - copies) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "Cache Results clear overlay exceeds byte bounds"
				};
				return false;
			}
			target.Entries.reserve(target.Entries.size() + count);
			for (const auto &entry : State.Data.Entries) {
				if (!retainedNode(entry)) continue;
				const auto old =
					std::find_if(target.Entries.begin(), target.Entries.end(), [&](const auto &item) {
						return item.NodeId == entry.NodeId && item.ProcessorRow == entry.ProcessorRow;
					});
				if (old == target.Entries.end())
					target.Entries.push_back(entry);
				else
					*old = entry;
			}
			return true;
		}
		std::vector<RequestImageSource> Seeds, Inputs;
		StatefulOutputEvaluationResult State;
		// The immutable journal before the current frame; current pixels are never
		// duplicated here.
		StatefulOutputEvaluationResult FrameStart;
		std::vector<RequestImageSource> FrameStartInputs;
		bool FrameStartValid = false, FrameStartResetSurfaces = false;
		EvaluationSnapshot InputSnapshot;
		std::string InputNode;
		uint64_t DocumentRevision = 0, InputRevision = 0, Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		bool RigidPlaying = false, RigidFrameProgress = false;
		std::optional<SourceCachePlaybackObservation> CacheObservation;
		std::optional<SourceFrameCacheProjectObservation> CacheProjectObservation;
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
		static uint64_t LedgerBytes(const StatefulOutputEvaluationResult &state) {
			const std::array<uint64_t, 5> records{
				RetainedSimulationReplayBytes(state.Simulation),
				RetainedSurfaceFrameReplayBytes(state.Surfaces),
				RetainedRandomReplayBytes(state.Random),
				RetainedDataReplayBytes(state.Data),
				RetainedRigidReplayBytes(state.Rigid)
			};
			uint64_t bytes = 0;
			for (const auto record : records) {
				if (record > std::numeric_limits<uint64_t>::max() - bytes)
					return std::numeric_limits<uint64_t>::max();
				bytes += record;
			}
			return bytes;
		}
		static void
		CopyLedgers(const StatefulOutputEvaluationResult &source, StatefulOutputEvaluationResult &target) {
			target.Simulation = source.Simulation;
			target.Surfaces = source.Surfaces;
			target.Random = source.Random;
			target.Data = source.Data;
			target.Rigid = source.Rigid;
		}
		static uint64_t CaptureBytes(std::span<const RequestImageSource> sources) {
			uint64_t bytes = sources.size() * sizeof(RequestImageSource);
			for (const auto &source : sources)
				bytes += source.SourceId.capacity() + source.Data.Pixels.capacity();
			return bytes;
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
				   node.Type.starts_with("pc.strand_") || node.Type.starts_with("pc.verlet_") ||
				   node.Type.starts_with("pc.flip_");
		}

	  public:
		// The observed source button frees slots without evaluating the graph. A fresh
		// observation owns the next update; same-clock dependent previews are unavailable.
		bool ClearCacheResults(
			const Document &document,
			const Plan &plan,
			std::string_view nodeId,
			uint64_t revision,
			uint64_t externalRevision,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) try {
			const auto fail = [&](Status status, const char *message) {
				diagnostic = {status, std::string(nodeId), {}, message};
				return false;
			};
			if (!Configured || DocumentRevision != revision || InputRevision != externalRevision)
				return fail(
					Status::InvalidValue, "Cache Results clear requires the current prepared document"
				);
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
				document.Nodes.size() > Limits::MaximumNodes ||
				document.Outputs.size() > Limits::MaximumOutputs ||
				plan.EffectiveLinks.size() > Limits::MaximumLinks)
				return fail(Status::LimitExceeded, "Cache Results clear graph or byte bound exceeded");
			const uint64_t edges = plan.GroupSurfaceDependencies.size() +
								   plan.InlineOwnerDependencies.size() +
								   plan.InlineControlDependencies.size() + plan.PcxNamedDependencies.size();
			if (edges > Limits::MaximumLinks || CacheClearNodes.size() > Limits::MaximumNodes ||
				CacheInvalidInputs.size() > Limits::MaximumNodes ||
				CacheInvalidOutputs.size() > Limits::MaximumOutputs)
				return fail(
					Status::LimitExceeded, "Cache Results clear dependency or name count exceeds bounds"
				);
			const uint64_t nodeNames =
				ClearNameScanWork(document.Nodes, [](const auto &n) -> const auto & { return n.Id; });
			const uint64_t outputNames =
				ClearNameScanWork(document.Outputs, [](const auto &o) -> const auto & { return o.Id; });
			const uint64_t linkNames =
				ClearNameScanWork(plan.EffectiveLinks, [](const auto &l) -> const auto & {
					return l.FromNode;
				});
			uint64_t work = CLEAR_WORK_LIMIT;
			// Each discovered link may perform a complete ID search. Charge the conservative
			// node-times-link bound, including string lengths, before entering the walk.
			if (!AdmitClearWork(work, 1, nodeNames) ||
				!AdmitClearWork(work, document.Nodes.size(), linkNames) ||
				!AdmitClearWork(work, document.Nodes.size() * plan.EffectiveLinks.size(), nodeNames) ||
				!AdmitClearWork(work, document.Nodes.size(), edges) ||
				!AdmitClearWork(work, 1, ClearNamesScanWork(CacheClearNodes)) ||
				!AdmitClearWork(work, document.Nodes.size() + 1, ClearNamesScanWork(CacheInvalidInputs)) ||
				!AdmitClearWork(work, document.Nodes.size() + 1, nodeNames) ||
				!AdmitClearWork(work, document.Outputs.size(), ClearNamesScanWork(CacheInvalidOutputs)) ||
				!AdmitClearWork(work, document.Outputs.size(), outputNames) ||
				!AdmitClearWork(work, document.Outputs.size(), ClearNamesScanWork(CacheInvalidInputs)) ||
				!AdmitClearWork(work, document.Outputs.size(), nodeNames))
				return fail(Status::LimitExceeded, "Cache Results clear dependency/name work exceeds bounds");
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &item) {
					return item.Id == nodeId;
				});
			if (node == document.Nodes.end() || node->Type != "pc.cache_results")
				return fail(Status::UnknownNode, "Clear cache requires an authored Cache Results node");
			std::array<bool, Limits::MaximumNodes> affected{};
			std::array<size_t, Limits::MaximumNodes> pending{};
			size_t count = 0;
			bool invalid = false;
			const auto add = [&](size_t index) {
				if (index >= document.Nodes.size()) {
					invalid = true;
					return;
				}
				if (!affected[index]) {
					affected[index] = true;
					pending[count++] = index;
				}
			};
			const auto addId = [&](std::string_view id) {
				const auto item =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &n) {
						return n.Id == id;
					});
				if (item == document.Nodes.end())
					invalid = true;
				else
					add(size_t(item - document.Nodes.begin()));
			};
			add(size_t(node - document.Nodes.begin()));
			while (count && !invalid) {
				const size_t current = pending[--count];
				for (const auto &link : plan.EffectiveLinks)
					if (link.FromNode == document.Nodes[current].Id) addId(link.ToNode);
				for (const auto &edge : plan.GroupSurfaceDependencies)
					if (edge.Producer == current) add(edge.Consumer);
				for (const auto &edge : plan.InlineOwnerDependencies)
					if (!edge.ControlsOnly && edge.Owner == current) add(edge.Consumer);
				for (const auto &edge : plan.InlineControlDependencies)
					if (edge.Producer == current) add(edge.Consumer);
				for (const auto &edge : plan.PcxNamedDependencies)
					if (edge.Producer == current) add(edge.Consumer);
			}
			if (invalid) return fail(Status::InvalidValue, "Cache Results clear dependencies are invalid");
			uint64_t names = (CacheClearNodes.size() + document.Nodes.size() + CacheInvalidInputs.size() +
							  CacheInvalidOutputs.size() + document.Outputs.size() + 1) *
							 sizeof(std::string);
			for (const auto &n : document.Nodes)
				names += std::max(n.Id.size(), std::string{}.capacity());
			for (const auto &out : document.Outputs)
				names += std::max(out.Id.size(), std::string{}.capacity());
			names += ClearMetadataBytes() + std::max(nodeId.size(), std::string{}.capacity());
			uint64_t live = OutputBytes(State) + LedgerBytes(FrameStart) + SourceBytes(FrameStartInputs) +
							SourceBytes(Seeds) + SourceBytes(Inputs) + InputSnapshot.RetainedBytes() +
							ClearMetadataBytes() + InputNode.capacity();
			live += Bindings.capacity() * sizeof(FeedbackBinding);
			for (const auto &binding : Bindings)
				live += binding.SourceId.capacity() + binding.OutputId.capacity();
			const uint64_t stateCopy = ClearedCacheResultsReplayBytes(State.Data, nodeId);
			const uint64_t startCopy = ClearedCacheResultsReplayBytes(FrameStart.Data, nodeId);
			const uint64_t validation =
				std::max(State.Data.Entries.size(), FrameStart.Data.Entries.size()) * sizeof(size_t);
			if (stateCopy == UINT64_MAX || startCopy == UINT64_MAX)
				return fail(Status::InvalidValue, "Cache Results clear slot ledger is invalid");
			if (live > maximumBytes || names > maximumBytes - live ||
				stateCopy > maximumBytes - live - names ||
				startCopy > maximumBytes - live - names - stateCopy ||
				validation > maximumBytes - live - names - stateCopy - startCopy)
				return fail(
					Status::LimitExceeded, "Cache Results clear transaction exceeds live byte bounds"
				);
			DataReplayState cleared, start;
			const auto clear = [&](const DataReplayState &source, DataReplayState &output) {
				const uint64_t bytes = RetainedDataReplayBytes(source) +
									   ClearedCacheResultsReplayBytes(source, nodeId) +
									   source.Entries.size() * sizeof(size_t);
				return ClearCacheResultsReplay(source, nodeId, output, diagnostic, bytes) == Status::Ok;
			};
			if (!clear(State.Data, cleared) || !clear(FrameStart.Data, start)) return false;
			auto nodes = CacheClearNodes, outputs = CacheInvalidOutputs, inputs = CacheInvalidInputs;
			nodes.reserve(nodes.size() + 1);
			outputs.reserve(outputs.size() + document.Outputs.size());
			inputs.reserve(inputs.size() + document.Nodes.size());
			const auto unique = [](auto &list, std::string_view id) {
				if (std::find(list.begin(), list.end(), id) == list.end()) list.emplace_back(id);
			};
			unique(nodes, nodeId);
			for (size_t i = 0; i < document.Nodes.size(); ++i)
				if (affected[i]) unique(inputs, document.Nodes[i].Id);
			for (const auto &out : document.Outputs)
				if (std::find(inputs.begin(), inputs.end(), out.NodeId) != inputs.end())
					unique(outputs, out.Id);
			State.Data = std::move(cleared);
			FrameStart.Data = std::move(start);
			CacheClearNodes = std::move(nodes);
			CacheInvalidOutputs = std::move(outputs);
			CacheInvalidInputs = std::move(inputs);
			CacheClearClock = {Tick, Subframe, NegativeFrame};
			if (std::find(CacheInvalidInputs.begin(), CacheInvalidInputs.end(), InputNode) !=
				CacheInvalidInputs.end())
				InputSnapshot = {};
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "Cache Results clear allocation refused"};
			return false;
		}
		// Export continuation can resume only a published clock from matching authored inputs.
		std::optional<FrameTime> PreparedFrame(uint64_t revision, uint64_t externalRevision) const noexcept {
			if (!Configured || !Initialized || DocumentRevision != revision ||
				InputRevision != externalRevision)
				return {};
			return FrameTime{Tick, Subframe, NegativeFrame};
		}
		// Owned capacities, including the immutable preframe journal. This excludes
		// borrowed request captures and provider sessions charged by their owners.
		uint64_t RetainedBytes() const noexcept {
			uint64_t bytes = sizeof(*this);
			const auto add = [&](uint64_t value) {
				if (value > UINT64_MAX - bytes) return false;
				bytes += value;
				return true;
			};
			const auto rows = [&](size_t count, size_t stride) {
				return !count || (stride <= UINT64_MAX / count && add(count * stride));
			};
			const auto sources = [&](const auto &values) {
				if (!rows(values.capacity(), sizeof(RequestImageSource))) return false;
				for (const auto &source : values)
					if (!add(source.SourceId.capacity()) || !add(source.Data.Pixels.capacity())) return false;
				return true;
			};
			for (const auto *state : {&State, &FrameStart}) {
				if (!add(RetainedStatefulOutputBytes(*state)) || !add(LedgerBytes(*state))) return UINT64_MAX;
			}
			if (!sources(Seeds) || !sources(Inputs) || !sources(FrameStartInputs) ||
				!add(InputSnapshot.RetainedBytes()) || !add(InputNode.capacity()) ||
				!rows(Bindings.capacity(), sizeof(FeedbackBinding)))
				return UINT64_MAX;
			for (const auto &binding : Bindings)
				if (!add(binding.SourceId.capacity()) || !add(binding.OutputId.capacity())) return UINT64_MAX;
			for (const auto *names : {&CacheClearNodes, &CacheInvalidOutputs, &CacheInvalidInputs}) {
				if (!rows(names->capacity(), sizeof(std::string))) return UINT64_MAX;
				for (const auto &name : *names)
					if (!add(name.capacity())) return UINT64_MAX;
			}
			return bytes;
		}

		// Borrow only for an immediate admitted copy while the matching published revision is ready.
		const DataReplayState *PreparedData(uint64_t revision, uint64_t externalRevision) const noexcept {
			if (!Configured || !Initialized || DocumentRevision != revision ||
				InputRevision != externalRevision)
				return nullptr;
			return &State.Data;
		}
		bool Active() const {
			return Stateful || !Bindings.empty();
		}
		// Native playback restart reconstructs tick zero while retaining source cache
		// controls. Provider sessions and authored revisions remain owned by their
		// existing callers.
		void RestartCycle() {
			Initialized = false;
		}
		void Clear() {
			CacheClearNodes = CacheInvalidOutputs = CacheInvalidInputs = {};
			CacheClearClock = {};
			Bindings = {};
			Seeds = {};
			Inputs = {};
			State = {};
			FrameStart = {};
			FrameStartInputs = {};
			FrameStartValid = false;
			InputSnapshot = {};
			InputNode.clear();
			DocumentRevision = InputRevision = Tick = 0;
			Subframe = 0;
			NegativeFrame = RigidPlaying = RigidFrameProgress = false;
			CacheObservation.reset();
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
			const bool directData = temporal.DataProcessors != 0 && !temporal.FirstFrameData &&
									!temporal.Simulation && !temporal.SurfaceCaches &&
									!temporal.RandomGenerators && !temporal.RigidActors && bindings.empty() &&
									(!temporal.SourceFrameCaches ||
									 (request.SourceCachePlayback && request.SourceCachePlayback->Sampling ==
																		 SourceCacheSampling::ObservedFrame));
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
										  oldConfigBytes + externalBytes + LedgerBytes(FrameStart) +
										  SourceBytes(FrameStartInputs) + ClearMetadataBytes();
			if (currentBytes >= maximumBytes)
				return fail(Status::LimitExceeded, "stateful host residency exceeds byte bounds");
			if (!CacheClearNodes.empty() || !CacheInvalidOutputs.empty() || !CacheInvalidInputs.empty()) {
				uint64_t work = CLEAR_WORK_LIMIT;
				const uint64_t nodeNames =
					ClearNameScanWork(document.Nodes, [](const auto &n) -> const auto & { return n.Id; });
				const uint64_t outputNames = ClearNamesScanWork(outputs);
				if (!AdmitClearWork(work, CacheClearNodes.size(), nodeNames) ||
					!AdmitClearWork(work, CacheClearNodes.size(), sizeof("pc.cache_results")) ||
					!AdmitClearWork(work, CacheInvalidOutputs.size(), outputNames) ||
					!AdmitClearWork(work, outputs.size() + 1, ClearNamesScanWork(CacheInvalidOutputs)) ||
					!AdmitClearWork(work, 1, ClearNamesScanWork(CacheInvalidInputs)))
					return fail(
						Status::LimitExceeded, "Cache Results clear retirement/name work exceeds bounds"
					);
			}
			const auto clearedOutput = [&](std::string_view id) {
				return std::find(CacheInvalidOutputs.begin(), CacheInvalidOutputs.end(), id) !=
					   CacheInvalidOutputs.end();
			};
			const bool clearSelection =
				!selectedOutput.empty() ? clearedOutput(selectedOutput)
				: !selectedNode.empty()
					? std::find(CacheInvalidInputs.begin(), CacheInvalidInputs.end(), selectedNode) !=
						  CacheInvalidInputs.end()
					: std::any_of(outputs.begin(), outputs.end(), clearedOutput);
			if (!changed &&
				FrameTime{request.Tick, request.Subframe, request.NegativeFrame} == CacheClearClock &&
				clearSelection)
				return fail(
					Status::UnsupportedExecution,
					"Cache Results was cleared; await a fresh source observation"
				);
			const bool cacheObservationChanged = temporal.SourceFrameCaches && Initialized &&
												 (CacheObservation != request.SourceCachePlayback ||
												  CacheProjectObservation != request.SourceCacheProject);
			const bool rigidObservationChanged =
				temporal.RigidActors && Initialized &&
				(RigidPlaying != request.RigidPlaying || RigidFrameProgress != request.RigidFrameProgress);
			const bool refreshFrame =
				!changed && Initialized && sameSelection && Tick == request.Tick &&
				NegativeFrame == request.NegativeFrame &&
				(cacheObservationChanged ||
				 (temporal.RigidActors && (rigidObservationChanged || Subframe != request.Subframe ||
										   !request.SimulationCacheCaptures.empty())));
			if (refreshFrame && !FrameStartValid)
				return fail(Status::InvalidValue, "playback refresh has no frame-start checkpoint");
			if (refreshFrame && feedback_detail::RefreshTouchesLua(
									document, plan, outputs, selectedNode, request.SimulationCacheCaptures
								))
				return fail(
					Status::UnsupportedExecution,
					"playback frame refresh cannot replay Lua session side effects"
				);
			if (!changed && Initialized && !rigidObservationChanged && !cacheObservationChanged &&
				Tick == request.Tick && Subframe == request.Subframe &&
				NegativeFrame == request.NegativeFrame && sameSelection &&
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
				directData || refreshFrame ||
				(!changed && sameSelection && Initialized &&
				 ((Tick < Limits::MaximumTick && Tick + 1 == request.Tick) ||
				  (Tick == request.Tick && Subframe == request.Subframe &&
				   NegativeFrame == request.NegativeFrame &&
				   (!request.SimulationCacheCaptures.empty() || rigidObservationChanged ||
					cacheObservationChanged)) ||
				  (temporal.RandomGenerators && !temporal.Simulation && !temporal.SurfaceCaches &&
				   bindings.empty() && Tick == request.Tick && request.Subframe > Subframe)));
			if (!contiguous && request.Tick && temporal.SourceFrameCaches && request.SourceCachePlayback &&
				request.SourceCachePlayback->Sampling == SourceCacheSampling::ObservedFrame)
				return fail(
					Status::UnsupportedExecution,
					"mixed observed frame-cache seek needs a recorded scheduler or explicit native played "
					"prefix"
				);
			if (!contiguous && request.Tick > 4096)
				return fail(Status::LimitExceeded, "stateful seek exceeds the 4096-step bound");
			std::string candidateNode(selectedNode);
			StatefulOutputEvaluationResult candidate;
			StatefulOutputEvaluationResult candidateStart;
			std::vector<RequestImageSource> candidateStartInputs;
			bool candidateStartValid = false, candidateStartResetSurfaces = false;
			EvaluationSnapshot candidateSnapshot;
			// Interlace caches survive edits and seeks; Time Remap clears on native revision changes.
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
				if (changed)
					std::erase_if(candidate.Surfaces.Entries, [&](const SurfaceFrameReplayEntry &entry) {
						return std::any_of(
							document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
								return node.Id == entry.NodeId && node.Type == "pc.time_remap";
							}
						);
					});
				candidate.Simulation.Entries.reserve(retainedCaches);
				for (const auto &entry : State.Simulation.Entries)
					if (retainCache(entry)) candidate.Simulation.Entries.push_back(entry);
			}
			if (!contiguous && temporal.SourceFrameCaches) {
				const uint64_t resident = currentBytes + OutputBytes(candidate);
				if (resident >= maximumBytes)
					return fail(Status::LimitExceeded, "frame-cache seek overlay residency exceeds bounds");
				if (OverlaySourceFrameCacheRows(
						document,
						State.Data,
						candidate.Data,
						FrameCacheOutputPolicy::Constructor,
						diagnostic,
						maximumBytes - resident
					) != Status::Ok)
					return false;
			}
			StatefulOutputEvaluationResult retiredPrior;
			const bool retireBeforeEvaluation =
				DocumentRevision != revision && contiguous && !refreshFrame &&
				std::any_of(State.Data.Entries.begin(), State.Data.Entries.end(), [](const auto &row) {
					return !SourceFrameCacheRowType(row).empty();
				});
			if (retireBeforeEvaluation) {
				// Retire old typed cache rows before another node with that ID can consume their state.
				const uint64_t copyBytes = LedgerBytes(State);
				const uint64_t resident = currentBytes + OutputBytes(candidate);
				if (resident >= maximumBytes || copyBytes >= maximumBytes - resident)
					return fail(Status::LimitExceeded, "frame-cache prior retirement copy exceeds bounds");
				CopyLedgers(State, retiredPrior);
				const DataReplayState none;
				if (OverlaySourceFrameCacheRows(
						document,
						none,
						retiredPrior.Data,
						FrameCacheOutputPolicy::RetainedObservation,
						diagnostic,
						maximumBytes - resident - copyBytes
					) != Status::Ok)
					return false;
			}
			std::vector<RequestImageSource> previous, inputs;
			const uint64_t first = contiguous ? request.Tick : 0;
			for (uint64_t tick = first; tick <= request.Tick; tick++) {
				EvaluationRequest clock = request;
				clock.Tick = tick;
				clock.ReuseSimulationFrame = false;
				clock.Subframe = tick == request.Tick ? request.Subframe : 0;
				BindNativeSourceFrameCacheProjectPrefix(clock);
				if (tick != request.Tick) clock.SimulationCacheCaptures = {};
				clock.SimulationAuthoringRevision = revision;
				clock.ResetSurfaceReplay = refreshFrame ? FrameStartResetSurfaces : !contiguous && tick == 0;
				if (refreshFrame) {
					uint64_t overlayBytes = 0;
					size_t overlayCount = 0;
					for (const auto &entry : State.Simulation.Entries)
						if (retainCache(entry)) {
							overlayBytes += RetainedSimulationEntryBytes(entry);
							++overlayCount;
						}
					const uint64_t copyBytes =
						LedgerBytes(FrameStart) + SourceBytes(FrameStartInputs) + overlayBytes;
					if (copyBytes > maximumBytes - currentBytes)
						return fail(Status::LimitExceeded, "frame-start checkpoint overlay exceeds bounds");
					CopyLedgers(FrameStart, candidateStart);
					if (temporal.SourceFrameCaches) {
						const uint64_t resident = currentBytes + LedgerBytes(candidateStart);
						if (resident >= maximumBytes)
							return fail(
								Status::LimitExceeded,
								"frame-cache checkpoint overlay residency exceeds bounds"
							);
						const auto policy =
							request.SourceCachePlayback && request.SourceCachePlayback->Sampling ==
															   SourceCacheSampling::ObservedFrame
								? FrameCacheOutputPolicy::RetainedObservation
								: FrameCacheOutputPolicy::PreObservation;
						if (OverlaySourceFrameCacheRows(
								document,
								State.Data,
								candidateStart.Data,
								policy,
								diagnostic,
								maximumBytes - resident
							) != Status::Ok)
							return false;
					}
					candidateStartInputs = FrameStartInputs;
					candidateStart.Simulation.Entries.reserve(
						candidateStart.Simulation.Entries.size() + overlayCount
					);
					for (const auto &entry : State.Simulation.Entries)
						if (retainCache(entry)) {
							const auto found = std::find_if(
								candidateStart.Simulation.Entries.begin(),
								candidateStart.Simulation.Entries.end(),
								[&](const auto &old) {
									return old.NodeId == entry.NodeId &&
										   old.ProcessorRow == entry.ProcessorRow && old.Cache;
								}
							);
							if (found == candidateStart.Simulation.Entries.end())
								candidateStart.Simulation.Entries.push_back(entry);
							else
								*found = entry;
						}
					candidateStartValid = true;
					candidateStartResetSurfaces = FrameStartResetSurfaces;
				}
				if (((!contiguous && tick == first) || refreshFrame) && !CacheClearNodes.empty()) {
					auto &data = refreshFrame ? candidateStart.Data : candidate.Data;
					const uint64_t resident = currentBytes + OutputBytes(candidate) +
											  LedgerBytes(candidateStart) + SourceBytes(candidateStartInputs);
					if (resident >= maximumBytes)
						return fail(
							Status::LimitExceeded, "Cache Results clear overlay residency exceeds bounds"
						);
					if (!OverlayClearedRows(document, data, maximumBytes - resident, diagnostic))
						return false;
				}
				const auto &prior = refreshFrame			 ? candidateStart
									: retireBeforeEvaluation ? retiredPrior
									: contiguous			 ? State
															 : candidate;
				clock.SimulationReplay = &prior.Simulation;
				clock.SurfaceReplay = &prior.Surfaces;
				clock.RandomReplay = &prior.Random;
				clock.DataReplay = &prior.Data;
				clock.RigidReplay = &prior.Rigid;
				clock.RigidAuthoringRevision = revision;
				const auto &generation = refreshFrame ? candidateStartInputs : tick == 0 ? seeds : previous;
				if (contiguous && !refreshFrame) {
					const bool sameFrameAction = Tick == request.Tick && Subframe == request.Subframe &&
												 NegativeFrame == request.NegativeFrame &&
												 (!request.SimulationCacheCaptures.empty() ||
												  rigidObservationChanged || cacheObservationChanged);
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
				if (refreshFrame) clock.ImageSources = candidateStartInputs;
				if (tick == request.Tick && (temporal.RigidActors || temporal.SourceFrameCaches) &&
					!refreshFrame) {
					const uint64_t copyBytes = LedgerBytes(prior) + CaptureBytes(clock.ImageSources);
					const uint64_t resident = currentBytes + OutputBytes(candidate) +
											  candidateSnapshot.RetainedBytes() + LedgerBytes(retiredPrior) +
											  SourceBytes(previous) + SourceBytes(captures) +
											  SourceBytes(inputs);
					if (resident >= maximumBytes || copyBytes > maximumBytes - resident)
						return fail(Status::LimitExceeded, "frame-start checkpoint copy exceeds bounds");
					CopyLedgers(prior, candidateStart);
					candidateStartInputs.reserve(clock.ImageSources.size());
					candidateStartInputs.insert(
						candidateStartInputs.end(), clock.ImageSources.begin(), clock.ImageSources.end()
					);
					candidateStartValid = true;
					candidateStartResetSurfaces = clock.ResetSurfaceReplay;
				}
				const uint64_t held = LedgerBytes(retiredPrior) + LedgerBytes(FrameStart) +
									  SourceBytes(FrameStartInputs) + LedgerBytes(candidateStart) +
									  SourceBytes(candidateStartInputs) + configBytes + oldConfigBytes +
									  externalBytes + OutputBytes(State) + InputSnapshot.RetainedBytes() +
									  candidateSnapshot.RetainedBytes() + SourceBytes(Inputs) +
									  SourceBytes(previous) + SourceBytes(captures) + SourceBytes(inputs);
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
				if (refreshFrame) {
					const uint64_t copyBytes = SourceBytes(candidateStartInputs);
					if (held >= maximumBytes ||
						OutputBytes(candidate) + candidateSnapshot.RetainedBytes() > maximumBytes - held ||
						copyBytes >
							maximumBytes - held - OutputBytes(candidate) - candidateSnapshot.RetainedBytes())
						return fail(Status::LimitExceeded, "frame-start feedback publication exceeds bounds");
					inputs = candidateStartInputs;
				} else
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
			// Admit the retained preceding generation before its final
			// copy/publication.
			if (request.Tick == 0 && !externalCount) inputs.clear();
			if (DocumentRevision != revision &&
				std::any_of(State.Data.Entries.begin(), State.Data.Entries.end(), [](const auto &row) {
					return !SourceFrameCacheRowType(row).empty();
				})) {
				const uint64_t resident = currentBytes + OutputBytes(candidate) +
										  LedgerBytes(candidateStart) + SourceBytes(candidateStartInputs);
				if (resident >= maximumBytes)
					return fail(Status::LimitExceeded, "frame-cache retirement residency exceeds bounds");
				const DataReplayState none;
				if (OverlaySourceFrameCacheRows(
						document,
						none,
						candidate.Data,
						FrameCacheOutputPolicy::RetainedObservation,
						diagnostic,
						maximumBytes - resident
					) != Status::Ok)
					return false;
			}
			if (changed) {
				Bindings = std::move(declarations);
				Seeds = std::move(blanks);
			}
			if (DocumentRevision != revision) {
				std::erase_if(CacheClearNodes, [&](const auto &id) {
					return std::none_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
						return node.Id == id && node.Type == "pc.cache_results";
					});
				});
				CacheInvalidOutputs = CacheInvalidInputs = {};
			} else {
				std::erase_if(CacheInvalidOutputs, [&](const auto &id) {
					return std::any_of(
						candidate.Outputs.begin(), candidate.Outputs.end(), [&](const auto &out) {
							return out.Id == id;
						}
					);
				});
				if (FrameTime{request.Tick, request.Subframe, request.NegativeFrame} != CacheClearClock)
					CacheInvalidInputs.clear();
			}

			State = std::move(candidate);
			FrameStart = std::move(candidateStart);
			FrameStartInputs = std::move(candidateStartInputs);
			FrameStartValid = candidateStartValid;
			FrameStartResetSurfaces = candidateStartResetSurfaces;
			InputSnapshot = std::move(candidateSnapshot);
			InputNode = std::move(candidateNode);
			Inputs = std::move(inputs);
			DocumentRevision = revision;
			InputRevision = externalRevision;
			Tick = request.Tick;
			Subframe = request.Subframe;
			NegativeFrame = request.NegativeFrame;
			CacheObservation = request.SourceCachePlayback;
			CacheProjectObservation = request.SourceCacheProject;
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
		std::span<const std::string> CacheResultsInvalidatedOutputs() const {
			return CacheInvalidOutputs;
		}
		const EvaluationSnapshot &Snapshot() const {
			return InputSnapshot;
		}
		const StatefulNamedOutput *Value(std::string_view outputId) const {
			if (std::find(CacheInvalidOutputs.begin(), CacheInvalidOutputs.end(), outputId) !=
				CacheInvalidOutputs.end())
				return nullptr;
			for (const auto &output : State.Outputs)
				if (output.Id == outputId) return &output;
			return nullptr;
		}
		const Image *Output(std::string_view outputId) const {
			const auto *value = Value(outputId);
			return value ? ImageOutput(*value) : nullptr;
		}
	};
} // namespace engine::imagegraph
