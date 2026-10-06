#include "TimelineOverrides.hpp"

#include "EvaluationAllocator.hpp"
#include "NativeSamplerBindings.hpp"
#include "PuppetControl.hpp"
#include "SourceAnimatorIdentity.hpp"
#include "SourceArgumentTransport.hpp"
#include "SourceMirrorAnimator.hpp"
#include "SourceSeparatedVec2.hpp"
#include "Timeline.hpp"
#include "TimelineDrivers.hpp"
#include "TimelineSchedule.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>
#include <new>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		void SetDiagnostic(
			Diagnostic &diagnostic,
			Status code,
			std::string message,
			std::string_view node = {},
			std::string_view port = {}
		) {
			diagnostic = {code, std::string(node), std::string(port), std::move(message)};
		}

		// Copy construction drops spare vector capacity; strings still retain their inline capacity.
		template <class T> uint64_t CloneOwnedBytes(const T &item) {
			if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>)
				return std::max(item.size(), std::string{}.capacity());
			else if constexpr (std::is_same_v<T, Value>)
				return std::visit([](const auto &leaf) { return CloneOwnedBytes(leaf); }, item);
			else if constexpr (std::is_same_v<T, SourceArrayItem>) {
				return std::visit(
					[](const auto &child) -> uint64_t {
						using C = std::decay_t<decltype(child)>;
						if constexpr (std::is_same_v<C, ElementValue>)
							return std::visit([](const auto &leaf) { return CloneOwnedBytes(leaf); }, child);
						else if constexpr (std::is_same_v<C, Image>)
							return child.Pixels.size();
						else {
							uint64_t bytes = child.size() * sizeof(SourceArrayItem);
							for (const auto &entry : child)
								bytes += CloneOwnedBytes(entry);
							return bytes;
						}
					},
					item.Data
				);
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				uint64_t bytes = item.Items.size() * sizeof(SourceArrayItem);
				for (const auto &entry : item.Items)
					bytes += CloneOwnedBytes(entry);
				bytes += item.Elements.size() * sizeof(ElementValue) +
						 item.Nested.size() * sizeof(std::vector<ElementValue>);
				for (const auto &leaf : item.Elements)
					bytes += std::visit([](const auto &value) { return CloneOwnedBytes(value); }, leaf);
				for (const auto &row : item.Nested) {
					bytes += row.size() * sizeof(ElementValue);
					for (const auto &leaf : row)
						bytes += std::visit([](const auto &value) { return CloneOwnedBytes(value); }, leaf);
				}
				return bytes;
			} else
				return PayloadOwnedBytes(item);
		}
	}

	const Node &TimelineOverrides::Find(size_t index, const Node &fallback) const {
		const auto found =
			std::lower_bound(Nodes.begin(), Nodes.end(), index, [](const auto &item, size_t target) {
				return item.NodeIndex < target;
			});
		return found != Nodes.end() && found->NodeIndex == index ? found->Authored : fallback;
	}

	Status ResolveTimelineOverrides(
		const Document &document,
		std::span<const uint8_t> needed,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		TimelineOverrides &result,
		Diagnostic &diagnostic,
		std::string_view port,
		bool rawSourceQuaternion,
		std::span<const uint8_t> getters,
		std::span<const SourceFrameCacheInputReads> getterReads,
		const TimelineOverrides *previous
	) try {
		ENGINE_PROFILE("imagegraph.timeline");
		if (needed.size() != document.Nodes.size() ||
			(!getters.empty() && getters.size() != document.Nodes.size()) ||
			(!getterReads.empty() && getterReads.size() != document.Nodes.size()) ||
			!ValidFrameTime(GetFrameTime(request))) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"timeline sampling needs a bounded clock and dependency cone"
			);
			return diagnostic.Code;
		}
		TimelineOverrides candidate;
		candidate.Observation = GetFrameTime(request);
		const bool replayKeys =
			request.GroupReplay &&
			(std::any_of(
				 request.GroupReplay->Entries().begin(),
				 request.GroupReplay->Entries().end(),
				 [](const auto &entry) { return !entry.SubtypeKeys.empty() || !entry.ParentKeys.empty(); }
			 ) ||
			 std::any_of(
				 request.GroupReplay->SharedSubtypes().begin(),
				 request.GroupReplay->SharedSubtypes().end(),
				 [](const auto &overlay) { return !overlay.Keys.empty(); }
			 ));
		if (document.Keyframes.empty() && !replayKeys) {
			std::swap(candidate.Charge, result.Charge);
			candidate.Nodes.swap(result.Nodes);
			std::swap(candidate.Observation, result.Observation);
			diagnostic = {};
			return Status::Ok;
		}
		using SourceWriter = std::pair<std::string_view, std::string_view>;
		auto combinedWriters = MakeEvaluationMap<SourceWriter, bool>(budget);
		uint64_t sourceGetterWork = 0;
		const auto consumers = getters.empty() ? needed : getters;
		const auto admitGetterWork = [&](uint64_t count) {
			if (count > 64'000'000 - sourceGetterWork) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "source getter selection exceeds work bounds"
				);
				return false;
			}
			sourceGetterWork += count;
			return true;
		};
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			if (!consumers[index]) continue;
			const auto &consumer = document.Nodes[index];
			const auto selectWriter = [&](std::string_view port) {
				if (!getterReads.empty() && !SourceFrameCacheReadsPort(getterReads[index], port)) return true;
				const auto *getter = SourcePropertyGetterNode(document, consumer, port, &sourceGetterWork);
				if (!getter) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"source getter chain exceeds work bounds",
						consumer.Id,
						port
					);
					return false;
				}
				if (!admitGetterWork(
						document.Links.size() +
						(getter->SourceSeparatedVec2Animators
							 ? getter->SourceSeparatedVec2Animators->Inputs.size()
							 : 0) +
						(request.GroupReplay ? request.GroupReplay->Bindings().size() +
												   request.GroupReplay->SharedSubtypes().size()
											 : 0)
					))
					return false;
				if (std::any_of(document.Links.begin(), document.Links.end(), [&](const auto &link) {
						return link.ToNode == getter->Id && link.ToPort == port;
					}))
					return true;
				const auto *overlay =
					request.GroupReplay ? request.GroupReplay->SharedSubtype(getter->Id, port) : nullptr;
				const auto *axes = overlay && overlay->SeparatedVec2 ? &*overlay->SeparatedVec2
																	 : FindSeparatedVec2(*getter, port);
				if (axes && axes->Separated) return true;
				const auto *binding = request.GroupReplay && request.GroupReplay->InstancesBound()
										  ? request.GroupReplay->Binding(consumer.Id, port)
										  : nullptr;
				const bool shared = binding && !InheritedMovedSourceGetter(consumer, port, binding);
				const SourceWriter writer =
					shared ? SourceWriter{binding->OwnerId, BindingAnimatorPort(*binding)}
						   : SourceWriter{getter->Id, port};
				combinedWriters.emplace(writer, true);
				return true;
			};
			const auto *entry = FindCatalogueEntry(consumer.Type);
			if (entry)
				for (const auto &input : entry->Inputs)
					if (SourceSeparatedVec2Input(consumer, input.Id) && !selectWriter(input.Id))
						return diagnostic.Code;
			for (const auto &input : consumer.DynamicInputs)
				if (SourceSeparatedVec2Input(consumer, input.Id) && !selectWriter(input.Id))
					return diagnostic.Code;
			if (request.GroupReplay) {
				if (!admitGetterWork(request.GroupReplay->Bindings().size())) return diagnostic.Code;
				for (const auto &binding : request.GroupReplay->Bindings())
					if (binding.NodeId == consumer.Id && !selectWriter(binding.Port)) return diagnostic.Code;
			}
		}
		const auto alreadySampled = [&](const Keyframe &key) {
			if (!previous) return false;
			for (const auto &node : previous->Nodes)
				if (node.Authored.Id == key.NodeId)
					return std::find(node.SampledPorts.begin(), node.SampledPorts.end(), key.Port) !=
						   node.SampledPorts.end();
			return false;
		};
		const auto staticSourceKey = [&](const Keyframe &key) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == key.NodeId;
				});
			if (node == document.Nodes.end()) return false;
			const auto *axisOverlay =
				request.GroupReplay ? request.GroupReplay->SharedSubtype(node->Id, key.Port) : nullptr;
			const auto *axes = axisOverlay && axisOverlay->SeparatedVec2 ? &*axisOverlay->SeparatedVec2
																		 : FindSeparatedVec2(*node, key.Port);
			if (axes && !combinedWriters.contains({key.NodeId, key.Port})) return true;
			const auto mirrorMode =
				SourceMirrorGetterAnimated(document, *node, key.Port, request.GroupReplay);
			if (mirrorMode && !*mirrorMode) return true;
			bool originalStatic =
				std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), key.Port) !=
				node->SourceStaticInputs.end();
			if (request.GroupReplay && request.GroupReplay->InstancesBound())
				for (const auto &binding : request.GroupReplay->Bindings())
					if (binding.OwnerId == key.NodeId && BindingAnimatorPort(binding) == key.Port) {
						originalStatic = binding.Writer == GroupSubtypeAnimator::Static;
						break;
					}
			if (!originalStatic) return false;
			size_t keyCount = 0;
			const auto *shared =
				request.GroupReplay ? request.GroupReplay->SharedSubtype(key.NodeId, key.Port) : nullptr;
			if (shared && !shared->Keys.empty())
				keyCount = shared->Keys.size();
			else
				for (const auto &stored : document.Keyframes)
					keyCount += stored.NodeId == key.NodeId && stored.Port == key.Port;
			if (keyCount != 1) return true;
			if (mirrorMode && *mirrorMode) return false;
			// An overridden animated getter still invokes the original shared animator's one-key driver.
			if (request.GroupReplay && request.GroupReplay->InstancesBound())
				for (const auto &binding : request.GroupReplay->Bindings())
					if (binding.OwnerId == key.NodeId && BindingAnimatorPort(binding) == key.Port &&
						binding.Getter == GroupSubtypeAnimator::Animated)
						return false;
			return true;
		};
		const auto sourceTriggerKey = [&](const Keyframe &key) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == key.NodeId;
				});
			if (node == document.Nodes.end()) return false;
			const auto *catalogue = FindCatalogueEntry(node->Type);
			const auto *input = catalogue ? FindCatalogueInput(*catalogue, key.Port) : nullptr;
			if (input && input->SourceKind == "Trigger") return true;
			if (node->Type != "pc.group_input" || key.Port != "parent_value") return false;
			const auto *loaded = request.GroupReplay ? request.GroupReplay->Find(node->Id) : nullptr;
			if (loaded) return loaded->Domain.Kind == SourceSocketKind::Trigger;
			const auto type = std::find_if(node->Values.begin(), node->Values.end(), [](const auto &value) {
				return value.Port == "input_type";
			});
			return type != node->Values.end() && std::holds_alternative<EnumValue>(type->Data) &&
				   std::get<EnumValue>(type->Data).Value == 19;
		};
		const auto suppressed = [&](const Keyframe &key) {
			// Source Trigger maps own key positions. Sampling boolean easing here preempts that getter.
			if (alreadySampled(key) || staticSourceKey(key) || sourceTriggerKey(key)) return true;
			if (request.GroupReplay && request.GroupReplay->Binding(key.NodeId, key.Port)) return true;
			const auto *entry = request.GroupReplay ? request.GroupReplay->Find(key.NodeId) : nullptr;
			const auto *shared =
				request.GroupReplay ? request.GroupReplay->SharedSubtype(key.NodeId, key.Port) : nullptr;
			if (shared && (shared->Fixed || !shared->Keys.empty())) return true;
			return entry &&
				   ((key.Port == "parent_value" && (entry->ParentReset || !entry->ParentKeys.empty())) ||
					(key.Port == "subtype" && (entry->SubtypeStatic || !entry->SubtypeKeys.empty())));
		};
		const auto eachOverlay = [&](auto &&visit) {
			if (!request.GroupReplay) return;
			for (const auto &overlay : request.GroupReplay->SharedSubtypes())
				if (!overlay.Fixed)
					for (const auto &key : overlay.Keys)
						if (!alreadySampled(key) && !staticSourceKey(key) && !sourceTriggerKey(key))
							visit(key);
			for (const auto &entry : request.GroupReplay->Entries()) {
				if (!entry.SubtypeStatic)
					for (const auto &key : entry.SubtypeKeys)
						if (!alreadySampled(key) && !staticSourceKey(key) && !sourceTriggerKey(key))
							visit(key);
				if (!entry.ParentReset)
					for (const auto &key : entry.ParentKeys)
						if (!alreadySampled(key) && !staticSourceKey(key) && !sourceTriggerKey(key))
							visit(key);
			}
		};
		size_t extraKeys = 0;
		eachOverlay([&](const Keyframe &) { ++extraKeys; });
		if (extraKeys > Limits::MaximumKeyframes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "group replay key count exceeds timeline bound");
			return diagnostic.Code;
		}

		const uint64_t tick = request.Tick;
		using PropertyKey = std::pair<size_t, std::string_view>;
		using Track = std::pair<size_t, std::vector<const Keyframe *>>;
		using ConfiguredKey = std::pair<std::string_view, std::string_view>;
		auto workspace = budget.Reserve(
			document.Nodes.size() * sizeof(std::pair<std::string_view, size_t>) +
			(document.Keyframes.size() + extraKeys) * sizeof(const Keyframe *)
		);
		if (!workspace) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "timeline tracks exceed the live byte budget");
			return diagnostic.Code;
		}
		std::vector<std::pair<std::string_view, size_t>> nodeIndices;
		nodeIndices.reserve(document.Nodes.size());
		for (size_t index = 0; index < document.Nodes.size(); ++index)
			nodeIndices.emplace_back(document.Nodes[index].Id, index);
		std::sort(nodeIndices.begin(), nodeIndices.end());
		const auto indexOf = [&](std::string_view id) -> std::optional<size_t> {
			const auto found = std::lower_bound(
				nodeIndices.begin(), nodeIndices.end(), id, [](const auto &item, std::string_view value) {
					return item.first < value;
				}
			);
			return found != nodeIndices.end() && found->first == id ? std::optional<size_t>{found->second}
																	: std::nullopt;
		};
		auto tracks = MakeEvaluationMap<PropertyKey, Track>(budget);
		for (const Keyframe &key : document.Keyframes) {
			if ((!port.empty() && key.Port != port) || suppressed(key)) continue;
			const auto index = indexOf(key.NodeId);
			if (!index) {
				SetDiagnostic(
					diagnostic, Status::UnknownNode, "keyframe node does not exist", key.NodeId, key.Port
				);
				return diagnostic.Code;
			}
			if (!needed[*index]) continue;
			const PropertyKey property{*index, key.Port};
			auto found = tracks.find(property);
			if (found == tracks.end()) found = tracks.emplace(property, Track{}).first;
			++found->second.first;
		}
		eachOverlay([&](const Keyframe &key) {
			if (!port.empty() && key.Port != port) return;
			const auto index = indexOf(key.NodeId);
			if (!index || !needed[*index]) return;
			const PropertyKey property{*index, key.Port};
			auto found = tracks.find(property);
			if (found == tracks.end()) found = tracks.emplace(property, Track{}).first;
			++found->second.first;
		});
		for (auto &[property, track] : tracks)
			track.second.reserve(track.first);
		for (const Keyframe &key : document.Keyframes) {
			if ((!port.empty() && key.Port != port) || suppressed(key)) continue;
			const size_t index = *indexOf(key.NodeId);
			if (needed[index]) tracks.at({index, key.Port}).second.push_back(&key);
		}
		eachOverlay([&](const Keyframe &key) {
			if (!port.empty() && key.Port != port) return;
			const auto index = indexOf(key.NodeId);
			if (index && needed[*index]) tracks.at({*index, key.Port}).second.push_back(&key);
		});
		auto configuredTracks = MakeEvaluationMap<ConfiguredKey, const AnimationTrack *>(budget);
		for (const auto &track : document.Tracks)
			configuredTracks[{track.NodeId, track.Port}] = &track;
		if (request.GroupReplay)
			for (const auto &animator : request.GroupReplay->DetachedAnimators())
				if (animator.Track) configuredTracks[{animator.OwnerId, animator.Id}] = &*animator.Track;
		size_t nodeCount = 0, previousIndex = document.Nodes.size();
		for (const auto &[property, track] : tracks)
			if (property.first != previousIndex) {
				++nodeCount;
				previousIndex = property.first;
			}
		uint64_t cloneBytes =
			nodeCount * sizeof(TimelineNodeOverride) + tracks.size() * sizeof(std::string_view);
		const auto add = [&](uint64_t bytes) {
			if (bytes > budget.Available() || cloneBytes > budget.Available() - bytes) return false;
			cloneBytes += bytes;
			return true;
		};
		previousIndex = document.Nodes.size();
		for (const auto &[property, track] : tracks) {
			const Node &node = document.Nodes[property.first];
			if (property.first != previousIndex) {
				previousIndex = property.first;
				if (!add(
						CloneOwnedBytes(node.Id) + CloneOwnedBytes(node.Type) +
						CloneOwnedBytes(node.GroupId) + CloneOwnedBytes(node.InstanceBase) +
						CloneOwnedBytes(node.SourceDisplayName) + CloneOwnedBytes(node.SourceInternalName) +
						node.InstanceOverrides.size() * sizeof(std::string) +
						node.SourceAnimatedInputs.size() * sizeof(std::string) +
						node.SourceStaticInputs.size() * sizeof(std::string) +
						node.Values.size() * sizeof(AuthoredValue) +
						node.DynamicInputs.size() * sizeof(DynamicInput) +
						node.DynamicOutputs.size() * sizeof(DynamicOutput) +
						node.SourceInputExpressions.size() * sizeof(SourceInputExpression) +
						node.SourceProperties.size() * sizeof(AuthoredValue)
					))
					goto clone_refused;
				const auto samplers = NativeSamplerBindingsPayloadBytes(node, false);
				if (!samplers || !add(*samplers)) goto clone_refused;
				const auto axes = SeparatedVec2Bytes(node, false);
				if (!axes || !add(*axes)) goto clone_refused;
				for (const auto &port : node.InstanceOverrides)
					if (!add(CloneOwnedBytes(port))) goto clone_refused;
				for (const auto &port : node.SourceAnimatedInputs)
					if (!add(CloneOwnedBytes(port))) goto clone_refused;
				for (const auto &port : node.SourceStaticInputs)
					if (!add(CloneOwnedBytes(port))) goto clone_refused;
				for (const auto &value : node.Values)
					if (!add(CloneOwnedBytes(value.Port) + CloneOwnedBytes(value.Data))) goto clone_refused;
				for (const auto &input : node.DynamicInputs)
					if (!add(
							CloneOwnedBytes(input.Id) + CloneOwnedBytes(input.SourceLayerName) +
							CloneOwnedBytes(input.SourceInputId) +
							(input.Default ? CloneOwnedBytes(*input.Default) : 0)
						))
						goto clone_refused;
				for (const auto &output : node.DynamicOutputs)
					if (!add(CloneOwnedBytes(output.Id))) goto clone_refused;
				for (const auto &expression : node.SourceInputExpressions)
					if (!add(CloneOwnedBytes(expression.Port) + CloneOwnedBytes(expression.Code)))
						goto clone_refused;
				for (const auto &value : node.SourceProperties)
					if (!add(CloneOwnedBytes(value.Port) + CloneOwnedBytes(value.Data))) goto clone_refused;
			}
			const bool dynamic =
				std::any_of(node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &input) {
					return input.Id == property.second;
				});
			const bool authored = std::any_of(node.Values.begin(), node.Values.end(), [&](const auto &value) {
				return value.Port == property.second;
			});
			if (!dynamic && !authored &&
				!add(sizeof(AuthoredValue) + CloneOwnedBytes(std::string_view(property.second))))
				goto clone_refused;
		}
		{
			auto charge = budget.Reserve(cloneBytes);
			if (!charge) goto clone_refused;
			candidate.Charge = std::move(*charge);
		}
		candidate.Nodes.reserve(nodeCount);
		previousIndex = document.Nodes.size();
		for (const auto &[property, track] : tracks) {
			if (property.first == previousIndex) continue;
			previousIndex = property.first;
			const Node &original = document.Nodes[property.first];
			size_t fresh = 0, sampledPortCount = 0;
			for (auto iterator = tracks.lower_bound({property.first, {}});
				 iterator != tracks.end() && iterator->first.first == property.first;
				 ++iterator) {
				++sampledPortCount;
				const auto &other = iterator->first;
				const bool dynamic = std::any_of(
					original.DynamicInputs.begin(), original.DynamicInputs.end(), [&](const auto &input) {
						return input.Id == other.second;
					}
				);
				const bool authored =
					std::any_of(original.Values.begin(), original.Values.end(), [&](const auto &value) {
						return value.Port == other.second;
					});
				fresh += !dynamic && !authored;
			}
			Node copy{original.Id, original.Type, original.GroupId, original.Position, {}};
			copy.InstanceBase = std::string(original.InstanceBase);
			copy.InstanceOverrides = original.InstanceOverrides;
			copy.SourceAnimatedInputs = original.SourceAnimatedInputs;
			copy.SourceStaticInputs = original.SourceStaticInputs;
			copy.SourceSeparatedVec2Animators = original.SourceSeparatedVec2Animators;
			copy.DynamicOutputs = original.DynamicOutputs;
			copy.SourceDisplayName = original.SourceDisplayName;
			copy.SourceInternalName = original.SourceInternalName;
			copy.SourceInputExpressions = original.SourceInputExpressions;
			copy.SourceProperties = original.SourceProperties;
			copy.NativeSamplerBindings = original.NativeSamplerBindings;
			copy.Values.reserve(original.Values.size() + fresh);
			copy.DynamicInputs.reserve(original.DynamicInputs.size());
			for (const auto &value : original.Values)
				copy.Values.push_back(value);
			for (const auto &input : original.DynamicInputs)
				copy.DynamicInputs.push_back(input);
			candidate.Nodes.push_back({property.first, std::move(copy), {}});
			candidate.Nodes.back().SampledPorts.reserve(sampledPortCount);
		}
		for (auto &[property, track] : tracks) {
			const auto sampledNode = std::lower_bound(
				candidate.Nodes.begin(),
				candidate.Nodes.end(),
				property.first,
				[](const auto &item, size_t index) { return item.NodeIndex < index; }
			);
			sampledNode->SampledPorts.push_back(property.second);
			auto &keys = track.second;
			Node &sampled = std::lower_bound(
								candidate.Nodes.begin(),
								candidate.Nodes.end(),
								property.first,
								[](const auto &item, size_t index) { return item.NodeIndex < index; }
			)->Authored;
			uint64_t maximumValue = 0;
			for (const auto *key : keys)
				maximumValue = std::max(maximumValue, CloneOwnedBytes(key->Data));
			auto valueCharge = budget.Reserve(3 * maximumValue + keys.size() * sizeof(FrameTime));
			if (!valueCharge) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"timeline value workspace exceeds the live byte budget",
					sampled.Id,
					property.second
				);
				return diagnostic.Code;
			}
			std::sort(keys.begin(), keys.end(), [](const Keyframe *left, const Keyframe *right) {
				return CompareFrameTime(GetFrameTime(*left), GetFrameTime(*right)) < 0;
			});
			Value value;
			const auto configured = configuredTracks.find({sampled.Id, property.second});
			if (keys.size() > 1 && configured != configuredTracks.end() &&
				configured->second->End == "wrap" && !document.Timeline) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"effective shared wrap track needs timeline frame count",
					sampled.Id,
					property.second
				);
				return diagnostic.Code;
			}
			if (configured != configuredTracks.end() && keys.size() > 1 &&
				(configured->second->LoopRange < -1 ||
				 (configured->second->LoopRange >= 0 &&
				  static_cast<uint64_t>(configured->second->LoopRange) >= keys.size()))) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"group animator replacement has an unrepresented source loop-range state",
					sampled.Id,
					property.second
				);
				return diagnostic.Code;
			}
			const Keyframe *left = nullptr;
			const Keyframe *right = nullptr;
			uint64_t numerator = 0;
			uint64_t denominator = 1;
			double fractionalRatio = 0;
			double driverRatio = 0.5;
			long double driverFrame = FrameTimeToReal(GetFrameTime(request));
			const bool extendedTime =
				request.NegativeFrame || std::any_of(keys.begin(), keys.end(), [](const Keyframe *key) {
					return key->NegativeFrame || key->Subframe != 0;
				});
			const bool fractionalTime = extendedTime || request.Subframe != 0;
			bool suppressDriver = false;
			// Node_Keyframe returns its lone source key before consulting end/range metadata.
			if (keys.size() == 1 &&
				(keys.front()->Interpolation == "source" ||
				 std::find(
					 sampled.SourceStaticInputs.begin(), sampled.SourceStaticInputs.end(), property.second
				 ) != sampled.SourceStaticInputs.end()))
				left = keys.front();
			else if (extendedTime) {
				std::vector<FrameTime> times;
				times.reserve(keys.size());
				for (const Keyframe *key : keys)
					times.push_back(GetFrameTime(*key));
				const AnimationTrack *track =
					configured == configuredTracks.end() ? nullptr : configured->second;
				const detail::KeyEnd end = !track || track->End == "hold" ? detail::KeyEnd::Hold
										   : track->End == "loop"		  ? detail::KeyEnd::Loop
										   : track->End == "ping"		  ? detail::KeyEnd::Ping
																		  : detail::KeyEnd::Wrap;
				const size_t loopStart = !track || track->LoopRange < 0
											 ? 0
											 : keys.size() - 1 - static_cast<size_t>(track->LoopRange);
				const uint64_t totalFrames =
					document.Timeline ? document.Timeline->Frames : keys.back()->Tick + 1;
				const bool sourceFastPath =
					keys.front()->Interpolation == "source" &&
					(keys.size() == 1 || (end == detail::KeyEnd::Wrap && driverFrame <= 0));
				const detail::KeyEnd selectionEnd = sourceFastPath ? detail::KeyEnd::Hold : end;
				detail::FractionalKeySelection selection;
				FrameTime mapped;
				if (!detail::SelectFrameTimes(
						times, GetFrameTime(request), totalFrames, selectionEnd, loopStart, selection, mapped
					)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"signed keyframe time is outside the bounded track",
						sampled.Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
				left = keys[selection.From];
				if (selection.To != selection.From) right = keys[selection.To];
				fractionalRatio = selection.Ratio;
				driverFrame = FrameTimeToReal(mapped);
			} else if (configured == configuredTracks.end()) {
				const auto next =
					std::upper_bound(keys.begin(), keys.end(), tick, [](uint64_t value, const Keyframe *key) {
						return value < key->Tick;
					});
				if (next == keys.begin()) {
					left = keys.front();
				} else {
					left = *(next - 1);
					if (next != keys.end() && (left->Tick != tick || request.Subframe != 0)) {
						right = *next;
						numerator = tick - left->Tick;
						denominator = right->Tick - left->Tick;
						if (request.Subframe != 0)
							fractionalRatio = static_cast<double>(
								(static_cast<long double>(numerator) + request.Subframe) / denominator
							);
					}
				}
			} else {
				std::vector<uint64_t> keyTicks;
				keyTicks.reserve(keys.size());
				for (const Keyframe *key : keys)
					keyTicks.push_back(key->Tick);
				const AnimationTrack &track = *configured->second;
				const detail::KeyEnd end = track.End == "loop"	 ? detail::KeyEnd::Loop
										   : track.End == "ping" ? detail::KeyEnd::Ping
										   : track.End == "wrap" ? detail::KeyEnd::Wrap
																 : detail::KeyEnd::Hold;
				const size_t loopStart =
					track.LoopRange < 0 ? 0 : keys.size() - 1 - static_cast<size_t>(track.LoopRange);
				const uint64_t totalFrames =
					document.Timeline ? document.Timeline->Frames : keys.back()->Tick + 1;
				const long double sampleFrame = driverFrame;
				const long double firstFrame = static_cast<long double>(keyTicks[loopStart]);
				const long double lastFrame = static_cast<long double>(keyTicks.back());
				// The source bypasses range remapping for its single-key fast path.
				if (keys.size() > 1 && track.End == "loop" && sampleFrame > lastFrame) {
					const long double period = lastFrame - firstFrame + 1.0L;
					driverFrame = firstFrame + std::fmod(sampleFrame - lastFrame, period);
				} else if (keys.size() > 1 && track.End == "ping" && sampleFrame > lastFrame) {
					const long double duration = lastFrame - firstFrame;
					if (duration == 0.0L) {
						driverFrame = firstFrame;
					} else {
						const long double phase = std::fmod(sampleFrame - firstFrame, duration * 2.0L);
						driverFrame =
							phase < duration ? firstFrame + phase : firstFrame + duration * 2.0L - phase;
					}
				}
				if (request.Subframe == 0) {
					detail::KeySelection selection;
					if (!detail::SelectKeys(keyTicks, tick, totalFrames, end, loopStart, selection)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"keyframe time is outside the bounded track",
							sampled.Id,
							std::string(property.second)
						);
						return diagnostic.Code;
					}
					left = keys[selection.From];
					if (selection.To != selection.From) right = keys[selection.To];
					numerator = selection.Numerator;
					denominator = selection.Denominator;
				} else {
					detail::FractionalKeySelection selection;
					if (!detail::SelectKeysFractional(
							keyTicks, tick, request.Subframe, totalFrames, end, loopStart, selection
						)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"fractional keyframe time is outside the bounded track",
							sampled.Id,
							std::string(property.second)
						);
						return diagnostic.Code;
					}
					left = keys[selection.From];
					if (selection.To != selection.From) right = keys[selection.To];
					fractionalRatio = selection.Ratio;
				}
			}
			if (right) {
				driverRatio = !fractionalTime
								  ? static_cast<double>(numerator) / static_cast<double>(denominator)
								  : fractionalRatio;
			} else if (keys.size() > 1 && left != keys.back() &&
					   driverFrame == FrameTimeToReal(GetFrameTime(*left))) {
				driverRatio = 0.0;
			}
			// The source's multi-key getter returns its first key at every nonpositive frame.
			if (keys.size() > 1 && keys.front()->Interpolation == "source" && driverFrame <= 0 &&
				!(keys.front()->SourceDriver &&
				  std::holds_alternative<KeyframeAudioDriver>(*keys.front()->SourceDriver)) &&
				(configured == configuredTracks.end() || configured->second->End != "wrap")) {
				left = keys.front();
				right = nullptr;
				suppressDriver = true;
			}
			if (keys.size() > 1 && keys.front()->Interpolation == "source" && driverFrame <= 0 &&
				configured != configuredTracks.end() && configured->second->End == "wrap") {
				left = keys.back();
				right = keys.front();
				const long double totalFrames = document.Timeline->Frames;
				const long double last = FrameTimeToReal(GetFrameTime(*left));
				const long double span = totalFrames - last + FrameTimeToReal(GetFrameTime(*right));
				if (span == 0 || !std::isfinite(span)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"source wrap interval has no finite duration",
						left->NodeId,
						left->Port
					);
					return diagnostic.Code;
				}
				driverRatio = static_cast<double>((totalFrames - last + driverFrame) / span);
				fractionalRatio = driverRatio;
			}
			// Fractional source key-map array indexing has no verified coercion contract.
			// Nonpositive source hold bypasses lookup. Exact positive endpoints remain native policy
			// pending coercion proof.
			if (right && left->Interpolation == "source" && driverFrame > 0 &&
				!(left->SourceDriver && std::holds_alternative<KeyframeAudioDriver>(*left->SourceDriver)) &&
				std::any_of(
					keys.begin(), keys.end(), [](const Keyframe *key) { return key->Subframe != 0; }
				) &&
				driverFrame != FrameTimeToReal(GetFrameTime(*left)) &&
				driverFrame != FrameTimeToReal(GetFrameTime(*right))) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"source fractional key-map index coercion is unverified",
					left->NodeId,
					left->Port
				);
				return diagnostic.Code;
			}
			const bool wrappingSegment = configured != configuredTracks.end() &&
										 configured->second->End == "wrap" && left == keys.back() &&
										 right == keys.front();
			const bool beforeFirst = !wrappingSegment && driverFrame < FrameTimeToReal(GetFrameTime(*left));
			if (!wrappingSegment && keys.size() > 1 && left == keys.front() && driverFrame == 0.0L &&
				!(left->SourceDriver && std::holds_alternative<KeyframeAudioDriver>(*left->SourceDriver)))
				suppressDriver = true;
			if (document.FormatVersion >= 8 && left->Interpolation == "source") {
				// Source returns an undriven lone raw key before numeric interpolation.
				if (keys.size() == 1 && !left->SourceDriver && !left->SineDriver &&
					SourceArgumentDefault(sampled, property.second)) {
					value = left->Data;
					goto resolved_track_value;
				}
				const auto *sourceCatalogue = FindCatalogueEntry(sampled.Type);
				const auto *sourceInput =
					sourceCatalogue ? FindCatalogueInput(*sourceCatalogue, property.second) : nullptr;
				const bool sourceEnum = sourceInput && sourceInput->Type == ValueType::Enum &&
										sourceInput->SourceBehavior &&
										sourceInput->SourceBehavior->FractionalInterpolation == true;
				const bool sourceDomain =
					sourceEnum || left->SourceDriver ||
					(configured != configuredTracks.end() && configured->second->QuaternionMode) ||
					std::holds_alternative<ArrayValue>(left->Data) ||
					std::holds_alternative<Gradient>(left->Data) ||
					std::holds_alternative<int64_t>(left->Data) ||
					std::holds_alternative<Vector3>(left->Data) ||
					std::holds_alternative<Vector4>(left->Data) || std::holds_alternative<Area>(left->Data);
				if (sourceDomain) {
					// Source exact interior keys still interpolate with their outgoing interval (notably raw
					// Slerp).
					if (!right && left != keys.back() &&
						driverFrame == FrameTimeToReal(GetFrameTime(*left)) && !suppressDriver) {
						const auto found = std::find(keys.begin(), keys.end(), left);
						right = *(found + 1);
						driverRatio = 0;
					}
					double ease = driverRatio;
					if (right) {
						const auto side = [](const std::string &type) {
							return type == "bezier" ? detail::CurveSide::Bezier
								   : type == "cut"	? detail::CurveSide::Cut
													: detail::CurveSide::Linear;
						};
						detail::KeyBlend blend;
						if (!detail::EaseKeys(
								{side(left->Ease->OutType),
								 side(right->Ease->InType),
								 left->Ease->Out.X,
								 left->Ease->Out.Y,
								 right->Ease->In.X,
								 right->Ease->In.Y},
								driverRatio,
								blend,
								wrappingSegment
							)) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidValue,
								"source driver easing could not be evaluated",
								left->NodeId,
								left->Port
							);
							return diagnostic.Code;
						}
						if (blend.Choice != detail::KeyChoice::Blend) {
							const auto *chosen = blend.Choice == detail::KeyChoice::From ? left : right;
							suppressDriver = true;
							ease = 0;
							left = chosen;
							right = nullptr;
						} else
							ease = blend.Ratio;
					}
					const auto mode = configured != configuredTracks.end()
										  ? configured->second->QuaternionMode
										  : std::nullopt;
					Value enumFirst, enumLast;
					if (sourceEnum &&
						(!detail::SourceEnumNumericPayload(left->Data, enumFirst) ||
						 !detail::SourceEnumNumericPayload(right ? right->Data : left->Data, enumLast))) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"source choice animation requires a flat numeric domain",
							left->NodeId,
							left->Port
						);
						return diagnostic.Code;
					}
					if (const auto *gradient = std::get_if<Gradient>(&left->Data);
						gradient && right && (suppressDriver || beforeFirst || !left->SourceDriver)) {
						const auto *target = std::get_if<Gradient>(&right->Data);
						size_t count = 0;
						const Status admitted =
							target ? detail::SourceGradientLerpCount(*gradient, *target, ease, count)
								   : Status::TypeMismatch;
						if (admitted != Status::Ok) {
							SetDiagnostic(
								diagnostic,
								admitted,
								"source gradient interpolation is undefined or "
								"exceeds its key limit",
								left->NodeId,
								left->Port
							);
							return diagnostic.Code;
						}
						const uint64_t workspace =
							3 * std::max(maximumValue, uint64_t(count) * sizeof(GradientKey)) +
							keys.size() * sizeof(FrameTime);
						if (!valueCharge->Resize(workspace)) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"source gradient workspace exceeds the live byte budget",
								left->NodeId,
								left->Port
							);
							return diagnostic.Code;
						}
					}
					const Status status = detail::ApplySourceDriver(
						!suppressDriver && !beforeFirst && left->SourceDriver ? &*left->SourceDriver
																			  : nullptr,
						sourceEnum ? enumFirst : left->Data,
						sourceEnum ? enumLast
						: right	   ? right->Data
								   : left->Data,
						ease,
						driverRatio,
						static_cast<double>(driverFrame),
						right != nullptr,
						mode.value_or(-1),
						value,
						static_cast<double>(
							document.Timeline ? document.Timeline->Frames : keys.back()->Tick + 1
						),
						rawSourceQuaternion && (!port.empty() ||
												[&] {
													const auto *entry = FindCatalogueEntry(sampled.Type);
													const auto *input =
														entry ? FindCatalogueInput(*entry, property.second)
															  : nullptr;
													return input && input->SourceIndex >= 0 &&
														   input->Type == ValueType::Quaternion;
												}()),
						&request
					);
					if (status != Status::Ok) {
						SetDiagnostic(
							diagnostic,
							status,
							"source driver value domain or numeric result cannot be represented",
							left->NodeId,
							left->Port
						);
						return diagnostic.Code;
					}
					goto resolved_track_value;
				}
			}
			if (right && left->Interpolation == "linear" &&
				PuppetControlValue(sampled, property.second, ValueType::Struct, left->Data) &&
				PuppetControlValue(sampled, property.second, ValueType::Struct, right->Data)) {
				const auto status = ApplySourceDriver(
					nullptr,
					left->Data,
					right->Data,
					driverRatio,
					driverRatio,
					static_cast<double>(driverFrame),
					true,
					-1,
					value
				);
				if (status != Status::Ok) {
					SetDiagnostic(
						diagnostic,
						status,
						"Puppet numeric control interpolation cannot be represented",
						left->NodeId,
						left->Port
					);
					return diagnostic.Code;
				}
				goto resolved_track_value;
			}
			if (!right || left->Interpolation == "step" ||
				(left->Interpolation != "source" && ((document.FormatVersion < 9 && !extendedTime)
														 ? left->Tick == tick
														 : GetFrameTime(*left) == GetFrameTime(request)))) {
				value = left->Data;
			} else if (left->Interpolation == "cubic") {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"cubic keyframe needs authored tangent controls",
					sampled.Id,
					std::string(property.second)
				);
				return diagnostic.Code;
			} else if (left->Interpolation == "source") {
				const auto side = [](const std::string &type) {
					return type == "bezier" ? detail::CurveSide::Bezier
						   : type == "cut"	? detail::CurveSide::Cut
											: detail::CurveSide::Linear;
				};
				const detail::KeyEase ease{
					side(left->Ease->OutType),
					side(right->Ease->InType),
					left->Ease->Out.X,
					left->Ease->Out.Y,
					right->Ease->In.X,
					right->Ease->In.Y
				};
				detail::KeyBlend blend;
				const double ratio =
					wrappingSegment && driverFrame <= 0 && left->Interpolation == "source"
						? driverRatio
						: (!fractionalTime ? static_cast<double>(numerator) / denominator : fractionalRatio);
				if (!detail::EaseKeys(ease, ratio, blend, wrappingSegment)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"keyframe easing could not be evaluated",
						sampled.Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
				if (blend.Choice == detail::KeyChoice::From) {
					value = left->Data;
					suppressDriver = true;
				} else if (blend.Choice == detail::KeyChoice::To) {
					value = right->Data;
					suppressDriver = true;
				} else {
					const Status status =
						detail::InterpolateEased(left->Data, right->Data, blend.Ratio, value);
					if (status != Status::Ok) {
						SetDiagnostic(
							diagnostic,
							status,
							"source easing needs finite scalar, vector or colour values",
							sampled.Id,
							std::string(property.second)
						);
						return diagnostic.Code;
					}
				}
			} else {
				const Status status =
					!fractionalTime
						? detail::Interpolate(left->Data, right->Data, numerator, denominator, value)
						: detail::InterpolateEased(left->Data, right->Data, fractionalRatio, value);
				if (status != Status::Ok) {
					SetDiagnostic(
						diagnostic,
						status,
						"linear interpolation needs finite scalar, vector or colour values",
						sampled.Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
			}
			if (left->SineDriver && !suppressDriver && !beforeFirst) {
				const KeyframeSineDriver &driver = *left->SineDriver;
				const uint64_t totalFrames =
					document.Timeline ? document.Timeline->Frames : keys.back()->Tick + 1;
				const double frameRate = driver.Frequency / static_cast<double>(totalFrames);
				const double wholeFrame = std::floor(static_cast<double>(driverFrame));
				const double subframe = static_cast<double>(driverFrame - wholeFrame);
				const double cycle = std::fmod(
					std::fmod(driver.Phase, 1.0) + std::fmod(frameRate, 1.0) * wholeFrame +
						std::fmod(frameRate * subframe, 1.0),
					1.0
				);
				constexpr double twoPi = 6.2831853071795864769252867665590057683943387987502;
				double envelope = 1.0;
				if (driver.Smooth > 0.0) {
					const double edge = std::clamp(
						std::min(
							{1.0,
							 2.0 * driverRatio / driver.Smooth,
							 2.0 * (1.0 - driverRatio) / driver.Smooth}
						),
						0.0,
						1.0
					);
					envelope = edge * edge * (3.0 - 2.0 * edge);
				}
				const double modulation = std::sin(cycle * twoPi) * driver.Amplitude * envelope;
				const double base = std::get<double>(value);
				const double driven = base + modulation;
				if (!std::isfinite(driven)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"sine keyframe driver result is not finite",
						sampled.Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
				value = driven;
			}
		resolved_track_value:
			Node &node = sampled;
			const auto dynamic = std::find_if(
				node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const DynamicInput &candidate) {
					return candidate.Id == property.second;
				}
			);
			const auto authored =
				std::find_if(node.Values.begin(), node.Values.end(), [&](const AuthoredValue &candidate) {
					return candidate.Port == property.second;
				});
			// The replacement remains admitted alongside the old payload until assignment frees it.
			const uint64_t replacedBytes =
				dynamic != node.DynamicInputs.end()
					? (dynamic->Default ? RetainedPayloadBytes(*dynamic->Default) : 0)
				: authored != node.Values.end() ? RetainedPayloadBytes(authored->Data)
												: 0;
			const uint64_t replacementBytes = RetainedPayloadBytes(value);
			if (!valueCharge->Resize(replacementBytes)) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"timeline replacement exceeds the live byte budget",
					node.Id,
					property.second
				);
				return diagnostic.Code;
			}
			if (dynamic != node.DynamicInputs.end()) {
				dynamic->Default = std::move(value);
			} else if (authored == node.Values.end()) {
				node.Values.push_back({std::string(property.second), std::move(value)});
			} else {
				authored->Data = std::move(value);
			}
			if (!candidate.Charge.Merge(std::move(*valueCharge))) std::terminate();
			if (!candidate.Charge.Resize(candidate.Charge.Bytes() - replacedBytes)) std::terminate();
		}
		std::swap(candidate.Charge, result.Charge);
		candidate.Nodes.swap(result.Nodes);
		std::swap(candidate.Observation, result.Observation);
		diagnostic = {};
		return Status::Ok;
	clone_refused:
		SetDiagnostic(diagnostic, Status::LimitExceeded, "animated node copies exceed the live byte budget");
		return diagnostic.Code;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "timeline allocation was refused");
		return diagnostic.Code;
	}
	Status ExtendTimelineOverrides(
		const Document &document,
		std::span<const uint8_t> needed,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		TimelineOverrides &result,
		Diagnostic &diagnostic,
		bool rawSourceQuaternion,
		std::span<const uint8_t> getters,
		std::span<const SourceFrameCacheInputReads> getterReads
	) try {
		ENGINE_PROFILE("imagegraph.timeline.extend");
		const auto clock = GetFrameTime(request);
		if (needed.size() != document.Nodes.size() ||
			(!getters.empty() && getters.size() != document.Nodes.size()) ||
			(!getterReads.empty() && getterReads.size() != document.Nodes.size()) || !ValidFrameTime(clock) ||
			(result.Observation && *result.Observation != clock) ||
			(!result.Nodes.empty() && !result.Observation)) {
			SetDiagnostic(diagnostic, Status::InvalidValue, "timeline extension needs the same observation");
			return diagnostic.Code;
		}
		EvaluationVector<uint8_t> fresh(needed.begin(), needed.end(), EvaluationAllocator<uint8_t>(budget));
		size_t previous = document.Nodes.size();
		for (const auto &node : result.Nodes) {
			if (node.NodeIndex >= document.Nodes.size() ||
				(previous != document.Nodes.size() && node.NodeIndex <= previous) ||
				node.Authored.Id != document.Nodes[node.NodeIndex].Id ||
				node.Authored.Type != document.Nodes[node.NodeIndex].Type) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "timeline extension has invalid prior nodes");
				return diagnostic.Code;
			}
			previous = node.NodeIndex;
			if (getters.empty()) fresh[node.NodeIndex] = 0;
		}
		if (std::none_of(fresh.begin(), fresh.end(), [](uint8_t value) { return value != 0; })) {
			diagnostic = {};
			return Status::Ok;
		}
		TimelineOverrides additions;
		const auto status = ResolveTimelineOverrides(
			document,
			fresh,
			request,
			budget,
			additions,
			diagnostic,
			{},
			rawSourceQuaternion,
			getters,
			getterReads,
			&result
		);
		if (status != Status::Ok) return status;
		if (additions.Nodes.empty()) {
			result.Observation = clock;
			return Status::Ok;
		}
		const uint64_t oldTables =
			(result.Nodes.size() + additions.Nodes.size()) * sizeof(TimelineNodeOverride);
		uint64_t mergeBytes = 0;
		for (auto &addition : additions.Nodes) {
			const auto old = std::lower_bound(
				result.Nodes.begin(),
				result.Nodes.end(),
				addition.NodeIndex,
				[](const auto &item, size_t index) { return item.NodeIndex < index; }
			);
			if (old == result.Nodes.end() || old->NodeIndex != addition.NodeIndex) continue;
			mergeBytes +=
				(old->Authored.Values.size() + addition.Authored.Values.size()) * sizeof(AuthoredValue) +
				(old->SampledPorts.size() + addition.SampledPorts.size()) * sizeof(std::string_view);
		}
		auto combinedCharge = budget.Reserve(oldTables + mergeBytes);
		if (!combinedCharge) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "timeline extension table exceeds live bytes");
			return diagnostic.Code;
		}
		std::vector<TimelineNodeOverride> combined;
		combined.reserve(result.Nodes.size() + additions.Nodes.size());
		for (auto &addition : additions.Nodes) {
			const auto old = std::lower_bound(
				result.Nodes.begin(),
				result.Nodes.end(),
				addition.NodeIndex,
				[](const auto &item, size_t index) { return item.NodeIndex < index; }
			);
			if (old == result.Nodes.end() || old->NodeIndex != addition.NodeIndex) continue;
			addition.Authored.Values.reserve(old->Authored.Values.size() + addition.Authored.Values.size());
			addition.SampledPorts.reserve(old->SampledPorts.size() + addition.SampledPorts.size());
		}
		// finish all allocations before moving earlier samples into the merged node.
		static_assert(std::is_nothrow_move_constructible_v<TimelineNodeOverride>);
		if (!combinedCharge->Merge(std::move(result.Charge))) {
			SetDiagnostic(diagnostic, Status::InvalidValue, "timeline extension uses another byte ledger");
			return diagnostic.Code;
		}
		if (!combinedCharge->Merge(std::move(additions.Charge))) std::terminate();
		auto old = result.Nodes.begin(), added = additions.Nodes.begin();
		while (old != result.Nodes.end() || added != additions.Nodes.end()) {
			if (added == additions.Nodes.end() ||
				(old != result.Nodes.end() && old->NodeIndex < added->NodeIndex))
				combined.push_back(std::move(*old++));
			else if (old == result.Nodes.end() || added->NodeIndex < old->NodeIndex)
				combined.push_back(std::move(*added++));
			else {
				const auto newlySampled = [&](std::string_view port) {
					return std::find(added->SampledPorts.begin(), added->SampledPorts.end(), port) !=
						   added->SampledPorts.end();
				};
				for (auto &value : old->Authored.Values) {
					if (newlySampled(value.Port)) continue;
					const auto destination = std::find_if(
						added->Authored.Values.begin(), added->Authored.Values.end(), [&](const auto &item) {
							return item.Port == value.Port;
						}
					);
					if (destination == added->Authored.Values.end())
						added->Authored.Values.push_back(std::move(value));
					else
						destination->Data = std::move(value.Data);
				}
				for (auto &input : old->Authored.DynamicInputs) {
					if (newlySampled(input.Id)) continue;
					const auto destination = std::find_if(
						added->Authored.DynamicInputs.begin(),
						added->Authored.DynamicInputs.end(),
						[&](const auto &item) { return item.Id == input.Id; }
					);
					if (destination != added->Authored.DynamicInputs.end())
						destination->Default = std::move(input.Default);
				}
				added->SampledPorts.insert(
					added->SampledPorts.end(), old->SampledPorts.begin(), old->SampledPorts.end()
				);
				combined.push_back(std::move(*added++));
				++old;
			}
		}
		result.Nodes.swap(combined);
		std::vector<TimelineNodeOverride>().swap(combined);
		std::vector<TimelineNodeOverride>().swap(additions.Nodes);
		if (!combinedCharge->Resize(combinedCharge->Bytes() - oldTables)) std::terminate();
		result.Charge = std::move(*combinedCharge);
		result.Observation = clock;
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "timeline extension allocation was refused");
		return diagnostic.Code;
	}

}
