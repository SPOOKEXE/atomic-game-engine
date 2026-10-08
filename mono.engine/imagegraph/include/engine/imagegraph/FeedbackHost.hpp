#pragma once

#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/FeedbackReplay.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/imagegraph/SourceFrameCacheProject.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>

namespace engine::imagegraph {
	namespace feedback_detail {
		// Scope admission precedes memo reads and follows the same compiled dependency union as replay.
		inline bool CheckScopeCone(
			const Document &document,
			const Plan &plan,
			std::span<const std::string> outputs,
			std::string_view selectedNode,
			const EvaluationRequest &request,
			Diagnostic &diagnostic
		) {
			const auto fail = [&](Status status, const char *message) {
				diagnostic = {status, {}, {}, message};
				return false;
			};
			if (request.Scope != ComposerScope::Unrestricted && request.Scope != ComposerScope::ImageOnly)
				return fail(Status::InvalidValue, "Composer scope is invalid");
			if (request.Scope == ComposerScope::Unrestricted) return true;
			if (document.Nodes.size() > Limits::MaximumNodes ||
				document.Outputs.size() > Limits::MaximumOutputs || outputs.size() > Limits::MaximumOutputs ||
				request.SimulationCacheCaptures.size() > Limits::MaximumNodes)
				return fail(Status::LimitExceeded, "feedback scope cone exceeds bounds");
			std::array<bool, Limits::MaximumNodes> visited{};
			std::array<size_t, Limits::MaximumNodes> pending{};
			size_t count = 0;
			bool valid = true;
			bool limited = false;
			uint64_t work = 64ull * 1024 * 1024;
			const auto addIndex = [&](size_t index) {
				if (index >= document.Nodes.size()) {
					valid = false;
					return;
				}
				if (!visited[index]) {
					visited[index] = true;
					pending[count++] = index;
				}
			};
			const auto addNode = [&](std::string_view id) {
				for (size_t index = 0; index < document.Nodes.size(); ++index) {
					if (!work) {
						valid = false;
						limited = true;
						return;
					}
					--work;
					if (document.Nodes[index].Id == id) {
						addIndex(index);
						return;
					}
				}
				valid = false;
			};
			if (!selectedNode.empty()) addNode(selectedNode);
			for (const auto id : request.SimulationCacheCaptures)
				addNode(id);
			for (const auto &output : document.Outputs)
				if ((outputs.empty() && selectedNode.empty()) ||
					std::find(outputs.begin(), outputs.end(), output.Id) != outputs.end())
					addNode(output.NodeId);
			for (const auto &id : outputs)
				if (std::none_of(document.Outputs.begin(), document.Outputs.end(), [&](const auto &out) {
						return out.Id == id;
					}))
					valid = false;
			while (count && valid) {
				const size_t index = pending[--count];
				const auto &node = document.Nodes[index];
				if (CheckComposerNodeScope(node, request.Scope, diagnostic) != Status::Ok) return false;
				const uint64_t cost = plan.EffectiveLinks.size() + plan.GroupSurfaceDependencies.size() +
									  plan.InlineOwnerDependencies.size() +
									  plan.InlineControlDependencies.size() +
									  plan.PcxNamedDependencies.size();
				if (cost > work)
					return fail(Status::LimitExceeded, "feedback scope cone work exceeds bounds");
				work -= cost;
				for (const auto &link : plan.EffectiveLinks)
					if (link.ToNode == node.Id) addNode(link.FromNode);
				for (const auto &dependency : plan.GroupSurfaceDependencies)
					if (dependency.Consumer == index) addIndex(dependency.Producer);
				for (const auto &dependency : plan.InlineOwnerDependencies)
					if (dependency.Consumer == index && !dependency.ControlsOnly) addIndex(dependency.Owner);
				for (const auto &dependency : plan.InlineControlDependencies)
					if (dependency.Consumer == index) addIndex(dependency.Producer);
				for (const auto &dependency : plan.PcxNamedDependencies)
					if (dependency.Consumer == index) addIndex(dependency.Producer);
			}
			if (limited) return fail(Status::LimitExceeded, "feedback scope cone work exceeds bounds");
			return valid || fail(Status::InvalidOutput, "feedback scope output cone is invalid");
		}
		struct GroupPurityLifecycle {
			std::array<std::string, 5> Categories;
			uint64_t RetainedBytes() const {
				uint64_t bytes = sizeof(*this);
				for (const auto &category : Categories)
					bytes += category.capacity();
				return bytes;
			}
		};
		// grug compare callback-relevant authored facts, never ordinary values, key
		// samples or canvas positions.
		inline bool PrepareGroupPurityLifecycle(
			const Document &document,
			GroupPurityLifecycle &result,
			uint64_t maximumBytes,
			Diagnostic &diagnostic
		) {
			using KeyPort = std::pair<std::string_view, std::string_view>;
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
				diagnostic = {Status::LimitExceeded, {}, {}, "group callback signature cap exceeds bounds"};
				return false;
			}
			uint64_t work = 64ull * 1024 * 1024;
			std::vector<KeyPort> implicitModes;
			const uint64_t modeCount = uint64_t(document.Keyframes.size()) + document.Tracks.size();
			const uint64_t scratch = modeCount * sizeof(KeyPort);
			if (scratch >= maximumBytes) {
				diagnostic = {Status::LimitExceeded, {}, {}, "group callback modes exceed byte bounds"};
				return false;
			}
			implicitModes.reserve(modeCount);
			const auto appendModes = [&](const auto &records) {
				for (const auto &key : records) {
					const Node *owner = nullptr;
					for (const auto &node : document.Nodes) {
						const auto bytes = std::max(node.Id.size(), key.NodeId.size()) + 1;
						if (bytes > work) return false;
						work -= bytes;
						if (node.Id == key.NodeId) {
							owner = &node;
							break;
						}
					}
					if (!owner) return false;
					bool explicitMode = false;
					for (const auto *ports : {&owner->SourceStaticInputs, &owner->SourceAnimatedInputs})
						for (const auto &port : *ports) {
							const auto bytes = std::max(port.size(), key.Port.size()) + 1;
							if (bytes > work) return false;
							work -= bytes;
							explicitMode = explicitMode || port == key.Port;
						}
					if (!explicitMode) implicitModes.emplace_back(key.NodeId, key.Port);
				}
				return true;
			};
			if (!appendModes(document.Keyframes) || !appendModes(document.Tracks)) goto refused;
			{
				bool comparisonRefused = false;
				std::sort(implicitModes.begin(), implicitModes.end(), [&](const auto &a, const auto &b) {
					const auto bytes = std::max(a.first.size(), b.first.size()) +
									   std::max(a.second.size(), b.second.size()) + 2;
					if (bytes > work)
						comparisonRefused = true;
					else
						work -= bytes;
					return a < b;
				});
				if (comparisonRefused) goto refused;
				implicitModes.erase(
					std::unique(
						implicitModes.begin(),
						implicitModes.end(),
						[&](const auto &a, const auto &b) {
							const auto bytes = std::max(a.first.size(), b.first.size()) +
											   std::max(a.second.size(), b.second.size()) + 2;
							if (bytes > work)
								comparisonRefused = true;
							else
								work -= bytes;
							return a == b;
						}
					),
					implicitModes.end()
				);
				if (comparisonRefused) goto refused;
			}
			{
				std::array<uint64_t, 5> sizes{};
				GroupPurityLifecycle candidate;
				bool admitted = true;
				const auto encode = [&](bool write) {
					const auto number = [&](size_t category, uint64_t value) {
						if (!write) {
							sizes[category] += sizeof(value);
							return;
						}
						for (unsigned byte = 0; byte < 8; ++byte)
							candidate.Categories[category].push_back(static_cast<char>(value >> (byte * 8)));
					};
					const auto text = [&](size_t category, std::string_view value) {
						number(category, value.size());
						if (!write)
							sizes[category] += value.size();
						else
							candidate.Categories[category].append(value);
					};
					number(0, document.Nodes.size());
					for (const auto &node : document.Nodes) {
						text(0, node.Id);
						text(0, node.Type);
						text(0, node.InstanceBase);
						text(0, node.SourceParentInputBase);
						text(1, node.Id);
						text(1, node.GroupId);
						text(2, node.Id);
						for (const auto *ports : {
								 &node.InstanceOverrides, &node.SourceStaticInputs, &node.SourceAnimatedInputs
							 }) {
							number(2, ports->size());
							for (const auto &port : *ports)
								text(2, port);
						}
						number(2, node.SourceInputExpressions.size());
						for (const auto &expression : node.SourceInputExpressions) {
							text(2, expression.Port);
							number(2, expression.Enabled);
						}
						text(4, node.Id);
						number(4, node.DynamicInputs.size());
						for (const auto &port : node.DynamicInputs) {
							text(4, port.Id);
							number(4, uint64_t(port.Type));
						}
						number(4, node.DynamicOutputs.size());
						for (const auto &port : node.DynamicOutputs) {
							text(4, port.Id);
							number(4, uint64_t(port.Type));
						}
						if (node.Type == "pc.group_input")
							for (const auto &value : node.Values)
								if (value.Port == "input_type" || value.Port == "subtype" ||
									value.Port == "vector_size") {
									text(4, value.Port);
									number(4, value.Data.index());
									if (const auto *choice = std::get_if<EnumValue>(&value.Data))
										number(4, uint64_t(choice->Value));
									else if (const auto *integer = std::get_if<int64_t>(&value.Data))
										number(4, uint64_t(*integer));
									else if (const auto *scalar = std::get_if<double>(&value.Data))
										number(4, std::bit_cast<uint64_t>(*scalar));
									else
										admitted = false;
								}
					}
					number(2, implicitModes.size());
					for (const auto &[node, port] : implicitModes) {
						text(2, node);
						text(2, port);
					}
					number(0, document.Groups.size());
					for (const auto &group : document.Groups) {
						text(0, group.Id);
						text(0, group.InstanceBase);
						text(0, group.OwnerNodeId);
						text(1, group.Id);
						text(1, group.ParentId);
						text(3, group.Id);
						number(3, group.PureFunction);
						text(4, group.Id);
						number(4, group.Ports.size());
						for (const auto &port : group.Ports) {
							text(4, port.Id);
							text(4, port.JunctionId);
							text(4, port.ControlNodeId);
							number(4, uint64_t(port.Direction));
						}
					}
					number(0, document.Links.size());
					for (const auto &link : document.Links) {
						text(0, link.FromNode);
						text(0, link.FromPort);
						text(0, link.ToNode);
						text(0, link.ToPort);
					}
					number(4, document.Junctions.size());
					for (const auto &junction : document.Junctions) {
						text(4, junction.Id);
						text(4, junction.GroupId);
						number(4, uint64_t(junction.Type));
					}
				};
				encode(false);
				uint64_t bytes = scratch + candidate.RetainedBytes();
				for (const auto size : sizes) {
					if (size > maximumBytes - std::min(bytes, maximumBytes)) goto refused;
					bytes += size;
				}
				if (!admitted || bytes > maximumBytes) goto refused;
				for (size_t index = 0; index < sizes.size(); ++index)
					candidate.Categories[index].reserve(sizes[index]);
				encode(true);
				if (!admitted || scratch + candidate.RetainedBytes() > maximumBytes) goto refused;
				result = std::move(candidate);
				return true;
			}
		refused:
			diagnostic = {
				Status::LimitExceeded, {}, {}, "group callback signature exceeds work or byte bounds"
			};
			return false;
		}
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
	enum class FeedbackSamplingProfile : uint8_t { LegacyFixedTicks, NativeFractional };
	// One host owns feedback generations and source processor state. All
	// selected/bound outputs share one evaluated closure per tick, so a simulation
	// or cached processor executes once.
	class CapturedFeedbackHost {
		FeedbackSamplingProfile SamplingProfile = FeedbackSamplingProfile::LegacyFixedTicks;
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
					Status::LimitExceeded, {}, {}, "Source cache clear overlay exceeds work bounds"
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
			uint64_t identities = 0;
			for (const auto &node : document.Nodes) {
				if (node.SourceProperties.size() > Limits::MaximumPropertiesPerNode) return failWork();
				const uint64_t metadataWork =
					ClearNameScanWork(node.SourceProperties, [](const auto &property) -> const auto & {
						return property.Port;
					});
				if (!AdmitClearWork(work, matching * 2, metadataWork)) return failWork();
				const auto saved = SourceFrameCacheIdentity(node);
				if (saved.size() > CLEAR_WORK_LIMIT - identities) return failWork();
				identities += saved.size();
			}
			if (!AdmitClearWork(work, matching * 2, identities)) return failWork();
			const auto retainedNode = [&](const auto &entry) {
				return tracked(entry) &&
					   std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
						   if (node.Id != entry.NodeId) return false;
						   const auto type = SourceFrameCacheRowType(entry);
						   return type.empty() ? node.Type == "pc.cache_results"
											   : node.Type == type &&
													 entry.LoadedCacheData == SourceFrameCacheIdentity(node);
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
					Status::LimitExceeded, {}, {}, "Source cache clear overlay exceeds byte bounds"
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
		GroupRenderSession Groups;
		feedback_detail::GroupPurityLifecycle GroupLifecycle;
		uint64_t GroupSeed = 0;
		uint32_t GroupImageDimension = 0;
		// The immutable journal before the current frame; current pixels are never
		// duplicated here.
		StatefulOutputEvaluationResult FrameStart;
		std::vector<RequestImageSource> FrameStartInputs;
		bool FrameStartValid = false, FrameStartResetSurfaces = false;
		EvaluationSnapshot InputSnapshot;
		std::string InputNode;
		uint64_t DocumentRevision = 0, InputRevision = 0, Tick = 0;
		ComposerScope GenerationScope = ComposerScope::Unrestricted;
		double Subframe = 0;
		bool NegativeFrame = false;
		bool RigidPlaying = false, RigidFrameProgress = false;
		std::optional<SourceCachePlaybackObservation> CacheObservation;
		std::optional<SourceFrameCacheProjectObservation> CacheProjectObservation;
		bool Configured = false, Initialized = false, Stateful = false, HaveExternalSources = false;
		FrameTime SourceCommonFrame{};
		uint64_t SourceCommonDocumentRevision = 0, SourceCommonInputRevision = 0;
		bool SourceCommonReady = false;
		bool SourceCommonMemoInvalidated = false;

		static bool RefreshGroupLifecycle(
			const Document &document,
			const Plan &plan,
			const EvaluationRequest &request,
			const feedback_detail::GroupPurityLifecycle &previous,
			const feedback_detail::GroupPurityLifecycle &current,
			GroupRenderSession &groups,
			Diagnostic &diagnostic,
			uint64_t maximumBytes
		) {
			constexpr std::array events{
				SourcePurityRefreshEvent::LoadTopology,
				SourcePurityRefreshEvent::Membership,
				SourcePurityRefreshEvent::AnimationMode,
				SourcePurityRefreshEvent::PureFunction,
				SourcePurityRefreshEvent::InputOutput
			};
			for (size_t index = 0; index < events.size(); ++index)
				if (groups.Purities.empty() || previous.Categories[index] != current.Categories[index])
					return RefreshSourceGroupPurity(
							   document, plan, request, {events[index]}, groups, diagnostic, maximumBytes
						   ) == Status::Ok;
			return true;
		}
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
		// grug call after loaded nodes exist, before preparing the revised document.
		// no input edit is invented.
		[[nodiscard]] bool RefreshLoadedSourceCacheGroups(
			const Document &document,
			std::span<const std::string_view> owners,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			return RefreshLoadedSourceCacheGroups(
				document, owners, diagnostic, CacheGroupLoadAdmission{}, maximumBytes
			);
		}
		// grug source/history admission runs after both loaded journals are ready.
		// refusal preserves current-frame and frame-start cache ownership and frozen
		// producer outputs.
		[[nodiscard]] bool RefreshLoadedSourceCacheGroups(
			const Document &document,
			std::span<const std::string_view> owners,
			Diagnostic &diagnostic,
			const CacheGroupLoadAdmission &admit,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
				diagnostic = {Status::LimitExceeded, {}, {}, "loaded cache group host cap is outside bounds"};
				return false;
			}
			if (owners.empty()) {
				if (!admit) {
					diagnostic = {};
					return true;
				}
				const auto resident = RetainedBytes();
				if (resident > maximumBytes) {
					diagnostic = {
						Status::LimitExceeded, {}, {}, "loaded cache group admission exceeds live bytes"
					};
					return false;
				}
				return RefreshLoadedCacheGroupReplay(
						   document, owners, {}, maximumBytes - resident, diagnostic, admit
					   ) == Status::Ok;
			}
			const auto resident = RetainedBytes();
			const auto groups = RetainedCacheGroupReplayBytes(State.Data.CacheGroups) +
								RetainedCacheGroupReplayBytes(FrameStart.Data.CacheGroups);
			if (resident >= maximumBytes || groups > resident) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "loaded cache group host exceeds live byte bounds"
				};
				return false;
			}
			const std::array journals{&State.Data.CacheGroups, &FrameStart.Data.CacheGroups};
			return RefreshLoadedCacheGroupReplay(
					   document, owners, journals, maximumBytes - resident + groups, diagnostic, admit
				   ) == Status::Ok;
		}
		// grug admit history while candidates stay private. false or bad_alloc must
		// preserve admission state. callback borrows documents; successful admission
		// is followed only by no-throw moves.
		template <class Admission>
			requires std::is_invocable_r_v<bool, const Admission &, const Document &, const Document &>
		[[nodiscard]] bool ToggleSourceCacheGroupMember(
			Document &document,
			std::string_view ownerId,
			std::string_view memberId,
			Diagnostic &diagnostic,
			const Admission &admit,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) try {
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
				diagnostic = {Status::LimitExceeded, {}, {}, "cache membership host cap is outside bounds"};
				return false;
			}
			const auto resident = RetainedBytes();
			const auto groups = RetainedCacheGroupReplayBytes(State.Data.CacheGroups) +
								RetainedCacheGroupReplayBytes(FrameStart.Data.CacheGroups);
			if (resident >= maximumBytes || groups > resident) {
				diagnostic = {Status::LimitExceeded, {}, {}, "cache membership host exceeds live bytes"};
				return false;
			}
			const std::array<const CacheGroupReplayState *, 2> journals{
				&State.Data.CacheGroups, &FrameStart.Data.CacheGroups
			};
			auto prepared = PrepareAuthoredCacheGroupMember(
				document, journals, ownerId, memberId, maximumBytes - resident + groups, diagnostic
			);
			if (!prepared) return false;
			if (!admit(document, prepared->Authored)) {
				diagnostic = {Status::LimitExceeded, {}, {}, "cache membership authoring admission refused"};
				return false;
			}
			static_assert(std::is_nothrow_move_assignable_v<Document>);
			static_assert(std::is_nothrow_move_assignable_v<CacheGroupReplayState>);
			document = std::move(prepared->Authored);
			State.Data.CacheGroups = std::move(prepared->Journals[0]);
			FrameStart.Data.CacheGroups = std::move(prepared->Journals[1]);
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "cache membership authoring allocation refused"};
			return false;
		}
		[[nodiscard]] bool ToggleSourceCacheGroupMember(
			Document &document,
			std::string_view ownerId,
			std::string_view memberId,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			return ToggleSourceCacheGroupMember(
				document,
				ownerId,
				memberId,
				diagnostic,
				[](const Document &, const Document &) { return true; },
				maximumBytes
			);
		}
		const CacheGroupReplayState &SourceCacheGroups() const {
			return State.Data.CacheGroups;
		}
		// grug synchronize native authored metadata and input edits before revision
		// replay. both complete data journals publish together; metadata alone keeps
		// rows and latest pixels.
		[[nodiscard]] bool NotifySourceAuthoredEdits(
			const Document &document,
			std::span<const std::string_view> editedNodes,
			std::span<const std::string_view> membershipOwners,
			std::span<const std::string_view> serializeOwners,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes,
			uint64_t maximumWork = SOURCE_FRAME_CACHE_EDIT_WORK_BYTES
		) {
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
				maximumWork > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES) {
				diagnostic = {Status::LimitExceeded, {}, {}, "Source authored edit cap is outside bounds"};
				return false;
			}
			// grug unconfigured cold journals have no captured rows or frozen members
			// to wake.
			if (!Configured) editedNodes = {};
			if (editedNodes.empty() && membershipOwners.empty() && serializeOwners.empty()) {
				diagnostic = {};
				return true;
			}
			const auto resident = RetainedBytes();
			const auto data = RetainedDataReplayBytes(State.Data) + RetainedDataReplayBytes(FrameStart.Data);
			if (resident >= maximumBytes || data > resident) {
				diagnostic = {Status::LimitExceeded, {}, {}, "Source authored edit host exceeds live bytes"};
				return false;
			}
			const std::array journals{&State.Data, &FrameStart.Data};
			return SynchronizeSourceFrameCacheEdits(
					   document,
					   editedNodes,
					   membershipOwners,
					   serializeOwners,
					   journals,
					   CacheProjectObservation,
					   diagnostic,
					   maximumBytes - resident + data,
					   maximumWork
				   ) == Status::Ok;
		}
		// Notify accepted input/connection edits before preparing the revised
		// document.
		[[nodiscard]] bool NotifySourceInputEdits(
			const Document &document,
			std::span<const std::string_view> editedNodes,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			return NotifySourceAuthoredEdits(document, editedNodes, {}, {}, diagnostic, maximumBytes);
		}
		// The observed source button frees slots without evaluating the graph. A
		// fresh observation owns the next update; same-clock dependent previews are
		// unavailable.
		bool ClearSourceCache(
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
					Status::InvalidValue, "Source cache clear requires the current prepared document"
				);
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
				document.Nodes.size() > Limits::MaximumNodes ||
				document.Outputs.size() > Limits::MaximumOutputs ||
				plan.EffectiveLinks.size() > Limits::MaximumLinks)
				return fail(Status::LimitExceeded, "Source cache clear graph or byte bound exceeded");
			for (const auto &item : document.Nodes)
				if (item.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
					return fail(Status::LimitExceeded, "source cache Clear metadata count exceeds bounds");
			const uint64_t edges = plan.GroupSurfaceDependencies.size() +
								   plan.InlineOwnerDependencies.size() +
								   plan.InlineControlDependencies.size() + plan.PcxNamedDependencies.size();
			if (edges > Limits::MaximumLinks || CacheClearNodes.size() > Limits::MaximumNodes ||
				CacheInvalidInputs.size() > Limits::MaximumNodes ||
				CacheInvalidOutputs.size() > Limits::MaximumOutputs)
				return fail(
					Status::LimitExceeded, "Source cache clear dependency or name count exceeds bounds"
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
			// Each discovered link may perform a complete ID search. Charge the
			// conservative node-times-link bound, including string lengths, before
			// entering the walk.
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
				return fail(Status::LimitExceeded, "Source cache clear dependency/name work exceeds bounds");
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &item) {
					return item.Id == nodeId;
				});
			if (node == document.Nodes.end() || (node->Type != "pc.cache_results" &&
												 node->Type != "pc.cache" && node->Type != "pc.cache_array"))
				return fail(Status::UnknownNode, "Clear cache requires an authored source cache node");
			const bool frameCache = node->Type != "pc.cache_results";
			bool serialize = true, seenSerialize = false;
			if (frameCache) {
				for (const auto &property : node->SourceProperties) {
					if (property.Port != "serialize") continue;
					const auto *value = std::get_if<bool>(&property.Data);
					if (!value || seenSerialize)
						return fail(
							Status::InvalidValue, "frame-cache Clear requires one typed Serialize flag"
						);
					serialize = *value;
					seenSerialize = true;
				}
				// Cache Array's nonforced clear is inert; admitted group enabling forces
				// it.
				if (node->Type == "pc.cache_array" &&
					(!serialize ||
					 (CacheProjectObservation && (CacheProjectObservation->ProjectLoading ||
												  CacheProjectObservation->ProjectAppending)))) {
					// Source clears input captures before its nonforced frame-clear guards.
					if (InputNode == nodeId) {
						InputSnapshot = {};
						InputNode.clear();
					}
					diagnostic = {};
					return true;
				}
			}
			const bool enableMembers =
				frameCache && serialize &&
				(!CacheProjectObservation ||
				 (!CacheProjectObservation->ProjectLoading && !CacheProjectObservation->ProjectAppending));
			const auto memberOwner = [&](const DataReplayState &source) {
				return std::find_if(
					source.CacheGroups.Owners.begin(),
					source.CacheGroups.Owners.end(),
					[&](const auto &owner) { return owner.NodeId == nodeId; }
				);
			};
			if (enableMembers) {
				for (const auto *source : {&State.Data, &FrameStart.Data}) {
					if (source->CacheGroups.Owners.size() > Limits::MaximumNodes)
						return fail(Status::LimitExceeded, "frame-cache button owner count exceeds bounds");
					if (!AdmitClearWork(
							work,
							2,
							ClearNameScanWork(
								source->CacheGroups.Owners,
								[](const auto &owner) -> const auto & { return owner.NodeId; }
							)
						))
						return fail(
							Status::LimitExceeded, "frame-cache button owner-name work exceeds bounds"
						);
					const auto owner = memberOwner(*source);
					if (owner == source->CacheGroups.Owners.end()) continue;
					if (owner->Members.size() > Limits::MaximumLinks ||
						!AdmitClearWork(work, owner->Members.size(), nodeNames))
						return fail(Status::LimitExceeded, "frame-cache button member work exceeds bounds");
					if (!owner->Members.empty() && !CacheProjectObservation)
						return fail(
							Status::UnsupportedExecution,
							"frame-cache group enabling requires project observations"
						);
				}
			}
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
			if (enableMembers)
				for (const auto *source : {&State.Data, &FrameStart.Data}) {
					const auto owner = memberOwner(*source);
					if (owner == source->CacheGroups.Owners.end()) continue;
					for (const auto &id : owner->Members)
						addId(id);
				}
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
			if (invalid) return fail(Status::InvalidValue, "Source cache clear dependencies are invalid");
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
							ClearMetadataBytes() + InputNode.capacity() +
							RetainedGroupRenderSessionBytes(Groups);
			live += Bindings.capacity() * sizeof(FeedbackBinding);
			for (const auto &binding : Bindings)
				live += binding.SourceId.capacity() + binding.OutputId.capacity();
			const auto clearedBytes = [&](const DataReplayState &source) {
				return frameCache ? ClearedSourceFrameCacheReplayBytes(*node, source)
								  : ClearedCacheResultsReplayBytes(source, nodeId);
			};
			const uint64_t stateCopy = clearedBytes(State.Data);
			const uint64_t startCopy = clearedBytes(FrameStart.Data);
			const auto validationBytes = [&](const DataReplayState &source) {
				const bool additionalRow =
					frameCache &&
					std::none_of(source.Entries.begin(), source.Entries.end(), [&](const auto &row) {
						return row.NodeId == nodeId;
					});
				return DataReplayValidationWorkspaceBytes(source, additionalRow ? 1 : 0);
			};
			const uint64_t validation =
				std::max(validationBytes(State.Data), validationBytes(FrameStart.Data));
			if (stateCopy == UINT64_MAX || startCopy == UINT64_MAX)
				return fail(Status::InvalidValue, "Source cache clear slot ledger is invalid");
			if (live > maximumBytes || names > maximumBytes - live ||
				stateCopy > maximumBytes - live - names ||
				startCopy > maximumBytes - live - names - stateCopy ||
				validation > maximumBytes - live - names - stateCopy - startCopy)
				return fail(Status::LimitExceeded, "Source cache clear transaction exceeds live byte bounds");
			DataReplayState cleared, start;
			const auto clear = [&](const DataReplayState &source, DataReplayState &output) {
				const uint64_t bytes =
					RetainedDataReplayBytes(source) + clearedBytes(source) + validationBytes(source);
				return (frameCache ? ClearSourceFrameCacheButtonReplay(
										 *node, source, output, CacheProjectObservation, diagnostic, bytes
									 )
								   : ClearCacheResultsReplay(source, nodeId, output, diagnostic, bytes)) ==
					   Status::Ok;
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
			diagnostic = {Status::LimitExceeded, {}, {}, "Source cache clear allocation refused"};
			return false;
		}
		// Export continuation can resume only a published clock from matching
		// authored inputs.
		std::optional<FrameTime> PreparedFrame(uint64_t revision, uint64_t externalRevision) const noexcept {
			if (SourceCommonMemoInvalidated || !Configured || !Initialized || DocumentRevision != revision ||
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
			if (!add(RetainedGroupRenderSessionBytes(Groups)) || !add(GroupLifecycle.RetainedBytes()))
				return UINT64_MAX;
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

		[[nodiscard]] bool HasSourceFrameCacheLoading() const noexcept {
			return std::any_of(State.Data.Entries.begin(), State.Data.Entries.end(), [](const auto &row) {
				return row.SourceFrameCacheLoading && row.SourceFrameCacheLoading->Loading;
			});
		}
		// Borrow only for an immediate admitted copy while the matching published
		// revision is ready.
		const DataReplayState *PreparedData(uint64_t revision, uint64_t externalRevision) const noexcept {
			if (SourceCommonMemoInvalidated || !Configured || !Initialized || DocumentRevision != revision ||
				InputRevision != externalRevision)
				return nullptr;
			return &State.Data;
		}
		// grug thumbnail and inspector observers borrow the same completed source
		// processor pulse.
		const GroupRenderSession *
		PreparedGroups(uint64_t revision, uint64_t externalRevision, FrameTime frame) const noexcept {
			const bool prepared =
				PreparedFrame(revision, externalRevision) == std::optional<FrameTime>{frame} &&
				!Groups.Outputs.Nodes.empty();
			const bool common = SourceCommonReady && SourceCommonDocumentRevision == revision &&
								SourceCommonInputRevision == externalRevision && SourceCommonFrame == frame &&
								!Groups.Common.Owners.empty();
			return prepared || common ? &Groups : nullptr;
		}
		[[nodiscard]] Status InitializeSourceCommonRuntime(
			const Document &document,
			const Plan &plan,
			const EvaluationRequest &request,
			SourceNodeInitialState initialState,
			Diagnostic &diagnostic,
			uint64_t revision,
			uint64_t externalRevision,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			if (initialState != SourceNodeInitialState::Loaded &&
				initialState != SourceNodeInitialState::Constructed) {
				diagnostic = {Status::InvalidValue, {}, {}, "source common constructor policy is invalid"};
				return diagnostic.Code;
			}
			if (document.SourceCommonOwners.empty())
				return ReconcileSourceCommonRuntime(
					document,
					plan,
					request,
					{initialState},
					diagnostic,
					revision,
					externalRevision,
					maximumBytes
				);
			const auto allowance = SourceCommonAllowance(request, maximumBytes, diagnostic);
			if (!allowance) return diagnostic.Code;
			auto current = SourceCommonRequest(request);
			const auto status = engine::imagegraph::InitializeNativeSourceCommonRuntime(
				document, plan, current, initialState, Groups, diagnostic, *allowance
			);
			if (status != Status::Ok) return status;
			CommitSourceCommonReplay();
			SourceCommonFrame = GetFrameTime(current);
			SourceCommonDocumentRevision = revision;
			SourceCommonInputRevision = externalRevision;
			SourceCommonReady = true;
			return Status::Ok;
		}
		[[nodiscard]] Status ReconcileSourceCommonRuntime(
			const Document &document,
			const Plan &plan,
			const EvaluationRequest &request,
			const SourceCommonRuntimeReconcile &operation,
			Diagnostic &diagnostic,
			uint64_t revision,
			uint64_t externalRevision,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			const auto allowance = SourceCommonAllowance(request, maximumBytes, diagnostic);
			if (!allowance) return diagnostic.Code;
			auto current = SourceCommonRequest(request);
			const auto status = engine::imagegraph::ReconcileNativeSourceCommonRuntime(
				document, plan, current, operation, Groups, diagnostic, *allowance
			);
			if (status != Status::Ok) return status;
			CommitSourceCommonReplay(!document.SourceCommonOwners.empty());
			SourceCommonDocumentRevision = revision;
			SourceCommonInputRevision = externalRevision;
			SourceCommonReady = false;
			return Status::Ok;
		}
		[[nodiscard]] Status StepSourceCommonRuntime(
			const Document &document,
			const Plan &plan,
			const EvaluationRequest &request,
			const SourceCommonRuntimeObservations &observations,
			Diagnostic &diagnostic,
			uint64_t revision,
			uint64_t externalRevision,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) {
			if (document.SourceCommonOwners.empty()) {
				if (!Groups.SourceCommonBindings.empty()) {
					diagnostic = {
						Status::InvalidValue,
						{},
						{},
						"source common owner retirement requires explicit reconciliation"
					};
					return diagnostic.Code;
				}
				diagnostic = {};
				return Status::Ok;
			}
			const auto allowance = SourceCommonAllowance(request, maximumBytes, diagnostic);
			if (!allowance) return diagnostic.Code;
			auto current = SourceCommonRequest(request);
			const auto status = engine::imagegraph::NativeSourceStepBounded(
				document, plan, current, observations, Groups, diagnostic, *allowance
			);
			if (status != Status::Ok) return status;
			CommitSourceCommonReplay();
			SourceCommonFrame = GetFrameTime(current);
			SourceCommonDocumentRevision = revision;
			SourceCommonInputRevision = externalRevision;
			SourceCommonReady = true;
			return Status::Ok;
		}
		[[nodiscard]] Status ReadSourceCommonGetter(
			const Document &document,
			const Plan &plan,
			std::string_view ownerId,
			SourceCommonSelector selector,
			const EvaluationRequest &request,
			EvaluatedValue &result,
			Diagnostic &diagnostic,
			uint64_t revision,
			uint64_t externalRevision,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) const {
			if (!SourceCommonReady || SourceCommonDocumentRevision != revision ||
				SourceCommonInputRevision != externalRevision || SourceCommonFrame != GetFrameTime(request)) {
				diagnostic = {
					Status::InvalidValue, std::string(ownerId), {}, "source common getter state is stale"
				};
				return diagnostic.Code;
			}
			const auto allowance = SourceCommonAllowance(request, maximumBytes, diagnostic);
			if (!allowance) return diagnostic.Code;
			const auto current = SourceCommonRequest(request);
			return engine::imagegraph::ReadNativeSourceCommonGetter(
				document, plan, ownerId, selector, current, Groups, result, diagnostic, *allowance
			);
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
			Groups = {};
			GroupLifecycle = {};
			FrameStart = {};
			FrameStartInputs = {};
			FrameStartValid = false;
			InputSnapshot = {};
			InputNode.clear();
			DocumentRevision = InputRevision = Tick = 0;
			GenerationScope = ComposerScope::Unrestricted;
			Subframe = 0;
			NegativeFrame = RigidPlaying = RigidFrameProgress = false;
			CacheObservation.reset();
			Configured = Initialized = Stateful = HaveExternalSources = false;
			SourceCommonFrame = {};
			SourceCommonDocumentRevision = SourceCommonInputRevision = 0;
			SourceCommonReady = false;
			SourceCommonMemoInvalidated = false;
		}

	  private:
		// The engine charges Groups and the borrowed canonical journals. Admit all other host storage
		// beside that shared operation, including retained preceding feedback pixels, without cloning it.
		std::optional<uint64_t> SourceCommonAllowance(
			const EvaluationRequest &request, uint64_t maximumBytes, Diagnostic &diagnostic
		) const {
			const auto held = RetainedBytes(), groups = RetainedGroupRenderSessionBytes(Groups),
					   ledgers = LedgerBytes(State);
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || held == UINT64_MAX ||
				groups > held || ledgers > held - groups) {
				diagnostic = {Status::LimitExceeded, {}, {}, "source common host residency exceeds bounds"};
				return {};
			}
			uint64_t outside = held - groups - ledgers;
			for (const auto *sources : {&Inputs, &Seeds, &FrameStartInputs})
				if (!request.ImageSources.empty() && request.ImageSources.data() == sources->data() &&
					request.ImageSources.size() == sources->size()) {
					const auto borrowed = CaptureBytes(request.ImageSources);
					if (borrowed > outside) {
						diagnostic = {
							Status::LimitExceeded, {}, {}, "source common image residency exceeds bounds"
						};
						return {};
					}
					outside -= borrowed;
					break;
				}
			if (outside >= maximumBytes) {
				diagnostic = {Status::LimitExceeded, {}, {}, "source common host residency exceeds bounds"};
				return {};
			}
			return maximumBytes - outside;
		}
		EvaluationRequest SourceCommonRequest(const EvaluationRequest &request) const {
			auto current = request;
			current.GroupRender = &Groups;
			current.SourceCommon = &Groups.Common;
			current.SourceCommonAnimators = &Groups.CommonAnimators;
			current.SimulationReplay = &State.Simulation;
			current.SurfaceReplay = &State.Surfaces;
			current.RandomReplay = &State.Random;
			current.DataReplay = &State.Data;
			current.RigidReplay = &State.Rigid;
			return current;
		}
		void CommitSourceCommonReplay(bool invalidateMemo = true) noexcept {
			State.Simulation = std::move(Groups.Replay.Simulation);
			State.Surfaces = std::move(Groups.Replay.Surfaces);
			State.Random = std::move(Groups.Replay.Random);
			State.Data = std::move(Groups.Replay.Data);
			State.Rigid = std::move(Groups.Replay.Rigid);
			Groups.Replay = {};
			// Empty retirement validates and reconciles native membership while ordinary memo remains valid.
			if (!invalidateMemo) return;
			// Retain the preceding image generation for feedback bindings. Public memo reads stay hidden
			// until Prepare publishes the next named outputs; the common held sockets remain readable.
			InputSnapshot = {};
			InputNode.clear();
			SourceCommonMemoInvalidated = true;
		}
		void ClearPreservingSourceCommon() {
			auto groups = std::move(Groups);
			auto simulation = std::move(State.Simulation);
			auto surfaces = std::move(State.Surfaces);
			auto random = std::move(State.Random);
			auto data = std::move(State.Data);
			auto rigid = std::move(State.Rigid);
			const auto sourceFrame = SourceCommonFrame;
			const auto sourceDocumentRevision = SourceCommonDocumentRevision;
			const auto sourceInputRevision = SourceCommonInputRevision;
			const bool sourceReady = SourceCommonReady;
			Clear();
			Groups = std::move(groups);
			State.Simulation = std::move(simulation);
			State.Surfaces = std::move(surfaces);
			State.Random = std::move(random);
			State.Data = std::move(data);
			State.Rigid = std::move(rigid);
			SourceCommonFrame = sourceFrame;
			SourceCommonDocumentRevision = sourceDocumentRevision;
			SourceCommonInputRevision = sourceInputRevision;
			SourceCommonReady = sourceReady;
			SourceCommonMemoInvalidated = true;
		}

	  public:
		// Native fractional previews sample a canonical integer-prefix generation. Source group feedback
		// instead retains every rendered observation; inline feedback requires an exact one-frame step.
		// Changing the native profile retires generations rather than reinterpreting existing pixels.
		bool SetFeedbackSamplingProfile(FeedbackSamplingProfile profile) {
			if (profile != FeedbackSamplingProfile::LegacyFixedTicks &&
				profile != FeedbackSamplingProfile::NativeFractional)
				return false;
			if (profile != SamplingProfile) {
				Clear();
				SamplingProfile = profile;
			}
			return true;
		}
		FeedbackSamplingProfile FeedbackProfile() const noexcept {
			return SamplingProfile;
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
			const bool changed = !Configured || DocumentRevision != revision ||
								 InputRevision != externalRevision || GenerationScope != request.Scope;
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
			if (!feedback_detail::CheckScopeCone(document, plan, outputs, selectedNode, request, diagnostic))
				return false;
			const auto temporal = AnalyzeStatefulTemporalCone(
				document, plan, outputs, selectedNode, request.SimulationCacheCaptures
			);
			if (!temporal.Valid) return fail(Status::InvalidOutput, "stateful output cone is invalid");
			const bool authoredGroups =
				std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
					if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") ||
						node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
						return false;
					return std::any_of(
						node.SourceProperties.begin(), node.SourceProperties.end(), [](const auto &property) {
							const auto *group = std::get_if<ArrayValue>(&property.Data);
							return property.Port == "cache_group" && group &&
								   group->ElementType == ValueType::Text && !group->Elements.empty();
						}
					);
				});
			// Common source owners retain constructor sockets even for stateless native kernels. Their
			// ordinary render pass must publish current sockets before the final held-output observation.
			const bool groupedProcessing = document.FormatVersion >= 10 &&
										   (!document.Groups.empty() || !Groups.SourceCommonBindings.empty());
			const auto *borrowedGroups =
				groupedProcessing && request.GroupRender != &Groups ? request.GroupRender : nullptr;
			stateful = groupedProcessing || authoredGroups || temporal.Simulation ||
					   temporal.SurfaceCaches != 0 || temporal.RandomGenerators != 0 ||
					   temporal.DataProcessors != 0 || !State.Simulation.Entries.empty() ||
					   !State.Surfaces.Entries.empty() || !State.Random.Entries.empty() ||
					   !State.Data.Entries.empty() || !State.Data.CacheGroups.Nodes.empty() ||
					   temporal.RigidActors != 0 || !State.Rigid.Owners.empty();
			const bool directData = (groupedProcessing || temporal.DataProcessors != 0) &&
									!temporal.FirstFrameData && !temporal.Simulation &&
									!temporal.SurfaceCaches && !temporal.RandomGenerators &&
									!temporal.RigidActors && bindings.empty() &&
									(!temporal.SourceFrameCaches ||
									 (request.SourceCachePlayback && request.SourceCachePlayback->Sampling ==
																		 SourceCacheSampling::ObservedFrame));
			if (!stateful && bindings.empty()) {
				if (changed || Stateful) {
					if (SourceCommonReady || !Groups.Common.Owners.empty())
						ClearPreservingSourceCommon();
					else
						Clear();
					Configured = true;
					DocumentRevision = revision;
					InputRevision = externalRevision;
					GenerationScope = request.Scope;
				}
				if (SourceCommonReady) {
					request.GroupRender = &Groups;
					request.SourceCommon = &Groups.Common;
					request.SourceCommonAnimators = &Groups.CommonAnimators;
					request.SimulationReplay = &State.Simulation;
					request.SurfaceReplay = &State.Surfaces;
					request.RandomReplay = &State.Random;
					request.DataReplay = &State.Data;
					request.RigidReplay = &State.Rigid;
				}
				return true;
			}
			const bool fractionalFeedback =
				SamplingProfile == FeedbackSamplingProfile::NativeFractional && !bindings.empty();
			if ((request.NegativeFrame && !directData) ||
				((temporal.FixedSimulationSteps || temporal.SurfaceCaches ||
				  (!bindings.empty() && !fractionalFeedback)) &&
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
			const uint64_t currentBytes =
				OutputBytes(State) + InputSnapshot.RetainedBytes() + InputNode.capacity() +
				SourceBytes(Inputs) + configBytes + oldConfigBytes + externalBytes + LedgerBytes(FrameStart) +
				SourceBytes(FrameStartInputs) + ClearMetadataBytes() +
				RetainedGroupRenderSessionBytes(Groups) + GroupLifecycle.RetainedBytes();
			if (currentBytes >= maximumBytes)
				return fail(Status::LimitExceeded, "stateful host residency exceeds byte bounds");
			if (!CacheClearNodes.empty() || !CacheInvalidOutputs.empty() || !CacheInvalidInputs.empty()) {
				uint64_t work = CLEAR_WORK_LIMIT;
				const uint64_t nodeNames =
					ClearNameScanWork(document.Nodes, [](const auto &n) -> const auto & { return n.Id; });
				const uint64_t outputNames = ClearNamesScanWork(outputs);
				const uint64_t replayNames =
					ClearNameScanWork(State.Data.Entries, [](const auto &row) -> const auto & {
						return row.NodeId;
					});
				uint64_t identities = 0;
				for (const auto &node : document.Nodes) {
					if (node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
						return fail(
							Status::LimitExceeded, "source cache clear metadata count exceeds bounds"
						);
					if (!AdmitClearWork(
							work,
							CacheClearNodes.size(),
							ClearNameScanWork(
								node.SourceProperties,
								[](const auto &property) -> const auto & { return property.Port; }
							)
						))
						return fail(Status::LimitExceeded, "source cache clear metadata work exceeds bounds");
					const auto saved = SourceFrameCacheIdentity(node);
					if (saved.size() > CLEAR_WORK_LIMIT - identities)
						return fail(Status::LimitExceeded, "source cache clear identity work exceeds bounds");
					identities += saved.size();
				}
				if (!AdmitClearWork(work, CacheClearNodes.size(), replayNames) ||
					!AdmitClearWork(work, CacheClearNodes.size() * State.Data.Entries.size(), identities) ||
					!AdmitClearWork(work, CacheClearNodes.size(), nodeNames) ||
					!AdmitClearWork(work, CacheClearNodes.size(), sizeof("pc.cache_results")) ||
					!AdmitClearWork(work, CacheInvalidOutputs.size(), outputNames) ||
					!AdmitClearWork(work, outputs.size() + 1, ClearNamesScanWork(CacheInvalidOutputs)) ||
					!AdmitClearWork(work, 1, ClearNamesScanWork(CacheInvalidInputs)))
					return fail(
						Status::LimitExceeded, "Source cache clear retirement/name work exceeds bounds"
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
					Status::UnsupportedExecution, "Source cache was cleared; await a fresh source observation"
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
				((fractionalFeedback && Subframe != request.Subframe) || cacheObservationChanged ||
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
			if (!SourceCommonMemoInvalidated && !changed && Initialized && !rigidObservationChanged &&
				!cacheObservationChanged && Tick == request.Tick && Subframe == request.Subframe &&
				NegativeFrame == request.NegativeFrame && sameSelection &&
				(!groupedProcessing ||
				 (GroupSeed == request.Seed && GroupImageDimension == request.MaximumImageDimension &&
				  CacheProjectObservation == request.SourceCacheProject &&
				  CacheObservation == request.SourceCachePlayback && RigidPlaying == request.RigidPlaying &&
				  RigidFrameProgress == request.RigidFrameProgress)) &&
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
				request.GroupRender = borrowedGroups							 ? borrowedGroups
									  : (groupedProcessing || SourceCommonReady) ? &Groups
																				 : nullptr;
				if (SourceCommonReady) {
					request.SourceCommon = &Groups.Common;
					request.SourceCommonAnimators = &Groups.CommonAnimators;
				}
				return true;
			}
			const bool reuseGroupPulse =
				!SourceCommonMemoInvalidated && groupedProcessing && !borrowedGroups && !changed &&
				Initialized && Tick == request.Tick && Subframe == request.Subframe &&
				NegativeFrame == request.NegativeFrame && GroupSeed == request.Seed &&
				GroupImageDimension == request.MaximumImageDimension &&
				RigidPlaying == request.RigidPlaying && RigidFrameProgress == request.RigidFrameProgress &&
				CacheObservation == request.SourceCachePlayback &&
				CacheProjectObservation == request.SourceCacheProject &&
				request.SimulationCacheCaptures.empty();
			const bool commonFrameReady =
				SourceCommonReady && SourceCommonDocumentRevision == revision &&
				SourceCommonInputRevision == externalRevision &&
				SourceCommonFrame == FrameTime{request.Tick, request.Subframe, request.NegativeFrame};
			const bool contiguous =
				reuseGroupPulse || directData || refreshFrame || commonFrameReady ||
				(!changed && sameSelection && Initialized &&
				 ((Tick < Limits::MaximumTick && Tick + 1 == request.Tick &&
				   (!fractionalFeedback || Subframe == 0)) ||
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
					"mixed observed frame-cache seek needs a recorded scheduler "
					"or explicit native played "
					"prefix"
				);
			if (!contiguous && request.Tick > 4096)
				return fail(Status::LimitExceeded, "stateful seek exceeds the 4096-step bound");
			std::string candidateNode(selectedNode);
			GroupRenderSession candidateGroups;
			feedback_detail::GroupPurityLifecycle candidateLifecycle;
			const bool ownGroupCandidate = !borrowedGroups && (groupedProcessing || SourceCommonReady);
			const bool lifecycleChanged = groupedProcessing && !borrowedGroups &&
										  (!Configured || DocumentRevision != revision ||
										   (!document.Groups.empty() && Groups.Purities.empty()));
			if (ownGroupCandidate) {
				const auto groupBytes = RetainedGroupRenderSessionBytes(Groups);
				if (groupBytes >= maximumBytes - currentBytes)
					return fail(Status::LimitExceeded, "group process history copy exceeds byte bounds");
				candidateGroups = Groups;
				if (lifecycleChanged) {
					const auto signatureAllowance = maximumBytes - currentBytes - groupBytes;
					if (!feedback_detail::PrepareGroupPurityLifecycle(
							document, candidateLifecycle, signatureAllowance, diagnostic
						))
						return false;
				}
			}
			StatefulOutputEvaluationResult candidate;
			StatefulOutputEvaluationResult candidateStart;
			std::vector<RequestImageSource> candidateStartInputs;
			bool candidateStartValid = false, candidateStartResetSurfaces = false;
			EvaluationSnapshot candidateSnapshot;
			// Interlace caches survive edits and seeks; Time Remap clears on native
			// revision changes.
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
			const uint64_t seedCopyBytes = RetainedSurfaceFrameReplayBytes(State.Surfaces) + cacheCopyBytes +
										   RetainedCacheGroupReplayBytes(State.Data.CacheGroups);
			if (!contiguous && seedCopyBytes > maximumBytes - currentBytes)
				return fail(Status::LimitExceeded, "stateful seek cache copy exceeds bounds");
			if (!contiguous) {
				// grug keep loaded owner order and frozen getters when frame history
				// starts again.
				candidate.Data.CacheGroups = State.Data.CacheGroups;
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
			const bool retainedSourceFrames =
				std::any_of(State.Data.Entries.begin(), State.Data.Entries.end(), [](const auto &row) {
					return !SourceFrameCacheRowType(row).empty();
				});
			if (!contiguous && (temporal.SourceFrameCaches || retainedSourceFrames)) {
				const uint64_t resident = currentBytes + OutputBytes(candidate);
				if (resident >= maximumBytes)
					return fail(Status::LimitExceeded, "frame-cache seek overlay residency exceeds bounds");
				if (OverlaySourceFrameCacheRows(
						document,
						State.Data,
						candidate.Data,
						temporal.SourceFrameCaches ? FrameCacheOutputPolicy::Constructor
												   : FrameCacheOutputPolicy::RetainedObservation,
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
				// Retire old typed cache rows before another node with that ID can
				// consume their state.
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
				if (ownGroupCandidate) {
					clock.GroupRender = &candidateGroups;
					clock.SourceCommon = &candidateGroups.Common;
					clock.SourceCommonAnimators = &candidateGroups.CommonAnimators;
				}
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
							Status::LimitExceeded, "Source cache clear overlay residency exceeds bounds"
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
					const bool sameFrameAction =
						Tick == request.Tick && Subframe == request.Subframe &&
						NegativeFrame == request.NegativeFrame &&
						(SourceCommonMemoInvalidated || !request.SimulationCacheCaptures.empty() ||
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
				if (tick == request.Tick &&
					(temporal.RigidActors || temporal.SourceFrameCaches || fractionalFeedback) &&
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
				const uint64_t held =
					LedgerBytes(retiredPrior) + LedgerBytes(FrameStart) + SourceBytes(FrameStartInputs) +
					LedgerBytes(candidateStart) + SourceBytes(candidateStartInputs) + configBytes +
					oldConfigBytes + externalBytes + RetainedGroupRenderSessionBytes(Groups) +
					GroupLifecycle.RetainedBytes() + OutputBytes(State) + InputSnapshot.RetainedBytes() +
					candidateSnapshot.RetainedBytes() + SourceBytes(Inputs) + SourceBytes(previous) +
					SourceBytes(captures) + SourceBytes(inputs) +
					(ownGroupCandidate ? RetainedGroupRenderSessionBytes(candidateGroups) +
											 candidateLifecycle.RetainedBytes()
									   : 0);
				if (held >= maximumBytes)
					return fail(Status::LimitExceeded, "stateful generation overlap exceeds byte bounds");
				if (groupedProcessing && !borrowedGroups && !reuseGroupPulse) {
					if (!RefreshGroupLifecycle(
							document,
							plan,
							clock,
							GroupLifecycle,
							lifecycleChanged ? candidateLifecycle : GroupLifecycle,
							candidateGroups,
							diagnostic,
							maximumBytes - held
						))
						return false;
					if (ProcessGroupRender(
							document,
							plan,
							clock,
							{GroupRenderMode::AutomaticFull},
							candidateGroups,
							diagnostic,
							maximumBytes - held
						) != Status::Ok)
						return false;
					clock.GroupRender = &candidateGroups;
					clock.SimulationReplay = &candidateGroups.Replay.Simulation;
					clock.SurfaceReplay = &candidateGroups.Replay.Surfaces;
					clock.RandomReplay = &candidateGroups.Replay.Random;
					clock.DataReplay = &candidateGroups.Replay.Data;
					clock.RigidReplay = &candidateGroups.Replay.Rigid;
				}
				if (reuseGroupPulse) clock.GroupRender = &candidateGroups;
				if (clock.GroupRender) {
					clock.ReuseSimulationFrame = true;
					clock.ResetSurfaceReplay = false;
				}
				const auto groupHeld =
					ownGroupCandidate ? RetainedGroupRenderSessionBytes(candidateGroups) : 0;
				if (groupHeld >= maximumBytes - held)
					return fail(Status::LimitExceeded, "group process observation exceeds byte bounds");
				const auto observationAllowance = maximumBytes - held - groupHeld;
				if (selectedNode.empty()) {
					if (EvaluateStatefulOutputs(
							document, plan, outputs, clock, candidate, diagnostic, observationAllowance
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
							observationAllowance,
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
				// grug one ledger owns processor history; the group journal keeps socket
				// values and readiness.
				candidateGroups.Replay = {};
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
					const auto node =
						std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
							return item.Id == id;
						});
					if (node == document.Nodes.end()) return true;
					return std::none_of(
						State.Data.Entries.begin(), State.Data.Entries.end(), [&](const auto &row) {
							if (row.NodeId != id) return false;
							const auto type = SourceFrameCacheRowType(row);
							return type.empty() ? node->Type == "pc.cache_results"
												: node->Type == type &&
													  row.LoadedCacheData == SourceFrameCacheIdentity(*node);
						}
					);
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

			GroupSeed = request.Seed;
			GroupImageDimension = request.MaximumImageDimension;
			if (lifecycleChanged) GroupLifecycle = std::move(candidateLifecycle);
			if (!groupedProcessing) GroupLifecycle = {};
			if (ownGroupCandidate)
				Groups = std::move(candidateGroups);
			else if (!SourceCommonReady && Groups.SourceCommonBindings.empty() &&
					 Groups.Common.Owners.empty())
				Groups = {};
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
			GenerationScope = request.Scope;
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
			request.GroupRender = borrowedGroups							 ? borrowedGroups
								  : (groupedProcessing || SourceCommonReady) ? &Groups
																			 : nullptr;
			if (SourceCommonReady) {
				request.SourceCommon = &Groups.Common;
				request.SourceCommonAnimators = &Groups.CommonAnimators;
			}
			SourceCommonMemoInvalidated = false;
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "stateful host allocation was refused"};
			return false;
		}
		// grug force the selected wrapper without turning its automatic render flag
		// on.
		bool ForceGroup(
			const Document &document,
			const Plan &plan,
			std::string_view groupId,
			EvaluationRequest request,
			Diagnostic &diagnostic,
			uint64_t maximumBytes = Limits::MaximumEvaluationBytes
		) try {
			const auto groupBytes = RetainedGroupRenderSessionBytes(Groups);
			const auto held = RetainedBytes();
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || held >= maximumBytes ||
				groupBytes >= maximumBytes - held) {
				diagnostic = {Status::LimitExceeded, {}, {}, "forced group host residency exceeds bounds"};
				return false;
			}
			feedback_detail::GroupPurityLifecycle lifecycle;
			if (!feedback_detail::PrepareGroupPurityLifecycle(
					document, lifecycle, maximumBytes - held - groupBytes, diagnostic
				))
				return false;
			const auto lifecycleBytes = lifecycle.RetainedBytes();
			if (lifecycleBytes >= maximumBytes - held - groupBytes) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "forced group callback residency exceeds bounds"
				};
				return false;
			}
			GroupRenderSession prepared = Groups;
			request.SimulationReplay = &State.Simulation;
			request.SurfaceReplay = &State.Surfaces;
			request.RandomReplay = &State.Random;
			request.DataReplay = &State.Data;
			request.RigidReplay = &State.Rigid;
			const auto allowance = maximumBytes - held - lifecycleBytes;
			if (!RefreshGroupLifecycle(
					document, plan, request, GroupLifecycle, lifecycle, prepared, diagnostic, allowance
				) ||
				ProcessGroupRender(
					document,
					plan,
					request,
					{GroupRenderMode::ForceGroup, groupId},
					prepared,
					diagnostic,
					allowance
				) != Status::Ok)
				return false;
			State.Simulation = std::move(prepared.Replay.Simulation);
			State.Surfaces = std::move(prepared.Replay.Surfaces);
			State.Random = std::move(prepared.Replay.Random);
			State.Data = std::move(prepared.Replay.Data);
			State.Rigid = std::move(prepared.Replay.Rigid);
			prepared.Replay = {};
			Groups = std::move(prepared);
			GroupLifecycle = std::move(lifecycle);
			Initialized = false;
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, {}, "forced group callback allocation refused"};
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
		std::span<const std::string> CacheInvalidatedOutputs() const {
			return CacheInvalidOutputs;
		}
		const EvaluationSnapshot &Snapshot() const {
			return InputSnapshot;
		}
		const StatefulNamedOutput *Value(std::string_view outputId) const {
			if (SourceCommonMemoInvalidated) return nullptr;
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
