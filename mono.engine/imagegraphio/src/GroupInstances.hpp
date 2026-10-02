#pragma once
#include "ImportBudget.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

namespace engine::imagegraphio::detail {
	// Admit the saved local projection while candidate and prior destinations
	// remain live. Clearing execution aliases selects the source stage before
	// setInstance.
	inline bool CaptureGroupPrebinding(
		const imagegraph::Document &document,
		std::optional<imagegraph::Document> &result,
		uint64_t previousDocumentBytes,
		ImportBudget &budget,
		std::string &failure
	) {
		if (std::none_of(document.Groups.begin(), document.Groups.end(), [](const auto &group) {
				return !group.InstanceBase.empty();
			}))
			return true;
		const auto bytes = imagegraph::DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes > imagegraph::Limits::MaximumDocumentBytes ||
			previousDocumentBytes > budget.Available() ||
			*bytes > (budget.Available() - previousDocumentBytes) / 2) {
			failure = "saved local Group projection exceeds overlapping operation bounds";
			return false;
		}
		if (!budget.Hold(*bytes)) std::terminate();
		try {
			auto snapshot = document;
			for (auto &node : snapshot.Nodes)
				node.InstanceBase.clear();
			for (auto &group : snapshot.Groups)
				group.InstanceBase.clear();
			result = std::move(snapshot);
			return true;
		} catch (const std::bad_alloc &) {
			failure = "saved local Group projection allocation failed";
			return false;
		}
	}
	// Reconciles the native projection only. The checked source archive is never
	// rewritten.
	inline bool ResolveImportedGroupInstances(
		const nlohmann::json &root,
		imagegraph::Document &document,
		std::string &failure,
		uint64_t previousDocumentBytes = 0,
		uint64_t maximumBytes = imagegraph::Limits::MaximumEvaluationBytes,
		ImportBudget *callerBudget = nullptr
	) {
		using namespace imagegraph;
		const auto fail = [&](std::string text) {
			failure = std::move(text);
			return false;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail("instance operation byte limit is invalid");
		ImportBudget localBudget(maximumBytes);
		ImportBudget &budget = callerBudget ? *callerBudget : localBudget;
		const auto candidateBytes = DocumentRetainedPayloadBytes(document);
		if (!candidateBytes || *candidateBytes > Limits::MaximumDocumentBytes ||
			!budget.Hold(previousDocumentBytes) || !budget.Hold(*candidateBytes))
			return fail("instance candidate and previous document exceed operation bounds");
		// Rebound allocators include map and set node storage, not only their value
		// pairs.
		using ClassPair = std::pair<const std::string_view, std::string_view>;
		using IdPair = std::pair<const std::string, std::string>;
		using IdMap = std::map<std::string, std::string, std::less<>, ImportAllocator<IdPair>>;
		using IdSet = std::set<std::string, std::less<>, ImportAllocator<std::string>>;
		using ViewSet = std::set<std::string_view, std::less<>, ImportAllocator<std::string_view>>;
		std::map<std::string_view, std::string_view, std::less<>, ImportAllocator<ClassPair>> sourceClasses(
			std::less<>{}, ImportAllocator<ClassPair>{budget}
		);
		const auto admit = [&](uint64_t bytes) { return budget.Hold(bytes); };
		const auto textBytes = [](std::string_view text) -> uint64_t {
			return std::max<size_t>(text.size(), 15) + 1;
		};
		const auto holdText = [&](std::string_view text) { return admit(textBytes(text)); };
		// Existing vector storage stays live until reserve returns. Charge the
		// replacement first.
		const auto reserve = [&](auto &values, size_t count) {
			using Item = typename std::decay_t<decltype(values)>::value_type;
			if (count <= values.capacity()) return true;
			if (count > UINT64_MAX / sizeof(Item) || !admit(count * sizeof(Item))) return false;
			const auto oldCapacity = values.capacity();
			values.reserve(count);
			budget.Release(oldCapacity * sizeof(Item));
			return true;
		};
		try {
			for (const auto &node : root.at("nodes"))
				sourceClasses.emplace(
					node.at("id").get_ref<const std::string &>(),
					node.at("type").get_ref<const std::string &>()
				);
			const auto nodeById = [&](std::string_view id) {
				return std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &n) {
					return n.Id == id;
				});
			};
			const auto groupById = [&](std::string_view id) {
				return std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &g) {
					return g.Id == id;
				});
			};
			const auto classOf = [&](const Node &node) -> std::string_view {
				std::string_view id = node.Id;
				for (size_t hop = 0; hop <= document.Nodes.size(); ++hop) {
					const auto source = sourceClasses.find(id);
					if (source != sourceClasses.end()) return source->second;
					const auto current = nodeById(id);
					if (current == document.Nodes.end() || current->InstanceBase.empty()) return {};
					id = current->InstanceBase;
				}
				return {};
			};
			const auto newId = [&](std::string_view group, std::string_view base, std::string &out) {
				constexpr std::string_view marker = "/instance/";
				if (group.size() > Limits::MaximumTextBytes - marker.size() ||
					base.size() > Limits::MaximumTextBytes - marker.size() - group.size())
					return false;
				if (!holdText(std::string_view{}) || !admit(group.size() + marker.size() + base.size()))
					return false;
				out.reserve(group.size() + marker.size() + base.size());
				out.assign(group);
				out.append(marker);
				out.append(base);
				return nodeById(out) == document.Nodes.end() && groupById(out) == document.Groups.end() &&
					   std::none_of(
						   document.Junctions.begin(), document.Junctions.end(), [&](const Junction &j) {
							   return j.Id == out;
						   }
					   );
			};
			const auto topo = [&](std::string_view owner, ImportVector<std::string> &order) {
				ImportVector<std::string_view> members{ImportAllocator<std::string_view>{budget}};
				for (const Node &node : document.Nodes)
					if (node.GroupId == owner) members.push_back(node.Id);
				for (const Group &group : document.Groups)
					if (group.ParentId == owner &&
						!(group.OwnerNodeId == group.Id && nodeById(group.Id) != document.Nodes.end()))
						members.push_back(group.Id);
				using Edge = std::pair<std::string_view, std::string_view>;
				ImportVector<Edge> edges{ImportAllocator<Edge>{budget}};
				const auto producer = [&](std::string_view id) {
					for (size_t hop = 0; hop < document.Junctions.size(); ++hop) {
						const auto junction = std::find_if(
							document.Junctions.begin(), document.Junctions.end(), [&](const Junction &j) {
								return j.Id == id;
							}
						);
						if (junction == document.Junctions.end()) break;
						const auto wire =
							std::find_if(document.Links.begin(), document.Links.end(), [&](const Link &l) {
								return l.ToNode == id;
							});
						if (wire == document.Links.end()) return std::string_view{};
						id = wire->FromNode;
					}
					const auto node = nodeById(id);
					std::string_view groupId =
						node == document.Nodes.end() ? id : std::string_view(node->GroupId);
					for (size_t hop = 0; hop < document.Groups.size(); ++hop) {
						const auto group = groupById(groupId);
						if (group == document.Groups.end() || group->Id == owner) break;
						if (group->ParentId == owner) return std::string_view(group->Id);
						groupId = group->ParentId;
					}
					return id;
				};
				for (std::string_view target : members) {
					const auto node = nodeById(target);
					// Collections contribute their ordered parent sockets, without a
					// synthetic executor.
					if (node == document.Nodes.end()) {
						const auto group = groupById(target);
						if (group == document.Groups.end()) return false;
						for (const auto &port : group->Ports) {
							if (port.Direction != PortDirection::Input) continue;
							const auto wire = std::find_if(
								document.Links.begin(), document.Links.end(), [&](const Link &link) {
									return link.ToNode == port.JunctionId;
								}
							);
							if (wire == document.Links.end()) continue;
							const auto source = producer(wire->FromNode);
							if (source != target &&
								std::find(members.begin(), members.end(), source) != members.end())
								edges.emplace_back(source, target);
						}
						continue;
					}
					// Source previous-node order is declared input order, not native link
					// insertion order.
					if (const auto *catalogue = FindCatalogueEntry(node->Type)) {
						ImportVector<const CatalogueInput *> inputOrder{
							ImportAllocator<const CatalogueInput *>{budget}
						};
						inputOrder.reserve(catalogue->Inputs.size());
						for (const auto &input : catalogue->Inputs)
							if (input.SourceIndex >= 0) inputOrder.push_back(&input);
						std::sort(
							inputOrder.begin(), inputOrder.end(), [](const auto *left, const auto *right) {
								return left->SourceIndex < right->SourceIndex;
							}
						);
						for (const auto *entry : inputOrder) {
							const auto &input = *entry;
							if (input.Id == "parent_value") continue;
							const auto wire = std::find_if(
								document.Links.begin(), document.Links.end(), [&](const Link &l) {
									return l.ToNode == target && l.ToPort == input.Id;
								}
							);
							if (wire == document.Links.end()) continue;
							const auto source = producer(wire->FromNode);
							if (source != target &&
								std::find(members.begin(), members.end(), source) != members.end()) {
								edges.emplace_back(source, target);
							}
						}
					} else {
						for (const Link &wire : document.Links)
							if (wire.ToNode == target) {
								const auto source = producer(wire.FromNode);
								if (source != target &&
									std::find(members.begin(), members.end(), source) != members.end()) {
									edges.emplace_back(source, target);
								}
							}
					}
				}
				ViewSet sorted(std::less<>{}, ImportAllocator<std::string_view>{budget});
				ViewSet visiting(std::less<>{}, ImportAllocator<std::string_view>{budget});
				const auto visit = [&](auto &&self, std::string_view id) -> bool {
					if (sorted.contains(id)) return true;
					if (!visiting.insert(id).second) return false;
					for (const auto &[parent, child] : edges)
						if (child == id && !self(self, parent)) return false;
					visiting.erase(id);
					sorted.insert(id);
					if (!holdText(id)) return false;
					order.emplace_back(id);
					return true;
				};
				for (std::string_view id : members)
					if (std::none_of(
							edges.begin(), edges.end(), [&](const Edge &edge) { return edge.first == id; }
						) &&
						!visit(visit, id))
						return false;
				return order.size() == members.size();
			};
			IdSet active(std::less<>{}, ImportAllocator<std::string>{budget});
			IdSet complete(std::less<>{}, ImportAllocator<std::string>{budget});
			const auto resolve = [&](auto &&self, const std::string &targetId) -> bool {
				auto target = groupById(targetId);
				if (target == document.Groups.end()) return fail("instance group is missing");
				if (target->InstanceBase.empty() || complete.contains(targetId)) return true;
				if (!holdText(targetId)) return fail("instance traversal names exceed operation bounds");
				if (!active.insert(targetId).second) return fail("group instance base cycle");
				if (!holdText(target->InstanceBase))
					return fail("instance base identity exceeds operation bounds");
				const std::string baseId = target->InstanceBase;
				if (!self(self, baseId)) return false;
				target = groupById(targetId);
				const auto base = groupById(baseId);
				if (base == document.Groups.end()) return fail("group instance base is missing");
				ImportVector<std::string> baseOrder{ImportAllocator<std::string>{budget}};
				ImportVector<std::string> targetOrder{ImportAllocator<std::string>{budget}};
				if (!topo(baseId, baseOrder) || !topo(targetId, targetOrder))
					return fail("instance child topology is cyclic or unrepresented");
				IdMap mapping(std::less<>{}, ImportAllocator<IdPair>{budget});
				IdSet used(std::less<>{}, ImportAllocator<std::string>{budget});
				const auto mapId = [&](std::string_view from, std::string_view to) {
					if (!holdText(from) || !holdText(to)) return false;
					mapping.emplace(std::string(from), std::string(to));
					return true;
				};
				const auto useId = [&](const std::string &id) {
					if (!holdText(id)) return false;
					used.insert(id);
					return true;
				};
				for (const std::string &id : baseOrder) {
					const auto node = nodeById(id);
					if (node == document.Nodes.end()) {
						const auto nestedBase = groupById(id);
						if (nestedBase == document.Groups.end())
							return fail("nested instance member is missing");
						const auto kind = sourceClasses.find(id);
						if (kind == sourceClasses.end())
							return fail("nested instance source class is unrepresented");
						const auto match = std::find_if(
							targetOrder.begin(), targetOrder.end(), [&](const std::string &candidate) {
								const auto current = groupById(candidate);
								const auto currentKind = sourceClasses.find(candidate);
								return current != document.Groups.end() && !used.contains(candidate) &&
									   currentKind != sourceClasses.end() &&
									   currentKind->second == kind->second;
							}
						);
						if (match == targetOrder.end())
							return fail(
								"missing nested instance requires source shallow-clone "
								"callback construction"
							);
						if (!holdText(*match) || !holdText(id))
							return fail("nested instance binding names exceed operation bounds");
						const std::string nestedTargetId(*match);
						std::string baseName(id);
						groupById(nestedTargetId)->InstanceBase.swap(baseName);
						if (!self(self, nestedTargetId)) return false;
						if (!mapId(id, nestedTargetId) || !useId(nestedTargetId))
							return fail("nested instance mapping exceeds operation bounds");
						for (const auto direction : {PortDirection::Input, PortDirection::Output}) {
							auto sourcePort = groupById(id)->Ports.begin();
							auto targetPort = groupById(nestedTargetId)->Ports.begin();
							while (true) {
								sourcePort = std::find_if(
									sourcePort, groupById(id)->Ports.end(), [&](const GroupPort &port) {
										return port.Direction == direction;
									}
								);
								targetPort = std::find_if(
									targetPort,
									groupById(nestedTargetId)->Ports.end(),
									[&](const GroupPort &port) { return port.Direction == direction; }
								);
								if (sourcePort == groupById(id)->Ports.end() ||
									targetPort == groupById(nestedTargetId)->Ports.end()) {
									if (sourcePort != groupById(id)->Ports.end() ||
										targetPort != groupById(nestedTargetId)->Ports.end())
										return fail(
											"nested instance boundary reconciliation did not "
											"preserve ordered sockets"
										);
									break;
								}
								if (!mapId(sourcePort->JunctionId, targetPort->JunctionId) ||
									!mapId(sourcePort->ControlNodeId, targetPort->ControlNodeId) ||
									!useId(targetPort->JunctionId))
									return fail("nested instance socket mapping exceeds operation bounds");
								++sourcePort;
								++targetPort;
							}
						}
						continue;
					}
					if (node->Type == "pc.group_input" || node->Type == "pc.group_output") continue;
					const auto kind = classOf(*node);
					if (kind.empty() || node->Type.starts_with("pxcx.opaque/"))
						return fail("instance child class is unrepresented");
					auto match = std::find_if(
						targetOrder.begin(), targetOrder.end(), [&](const std::string &candidate) {
							const auto current = nodeById(candidate);
							return !used.contains(candidate) && current != document.Nodes.end() &&
								   classOf(*current) == kind;
						}
					);
					std::string copiedId;
					if (match == targetOrder.end()) {
						if (document.Nodes.size() == Limits::MaximumNodes || !newId(targetId, id, copiedId))
							return fail("instance node expansion exceeds durable ID or count bounds");
						const auto bytes = NodeClonePayloadBytes(*node);
						if (!bytes ||
							!admit(*bytes + textBytes(copiedId) + textBytes(targetId) + textBytes(id)))
							return fail(
								"instance clone payload exceeds document bounds before "
								"copying"
							);
						if (!reserve(document.Nodes, document.Nodes.size() + 1))
							return fail("instance node storage exceeds operation bounds");
						const auto original = nodeById(id);
						std::string copiedName(copiedId), copiedGroup(targetId), copiedBase(id);
						Node copied = *original;
						copied.Id.swap(copiedName);
						copied.GroupId.swap(copiedGroup);
						copied.InstanceBase.swap(copiedBase);
						document.Nodes.push_back(std::move(copied));
						// Node.clone preserves local animators for a later per-input
						// override.
						const size_t originalKeyCount = document.Keyframes.size();
						for (size_t keyIndex = 0; keyIndex < originalKeyCount; ++keyIndex) {
							if (document.Keyframes[keyIndex].NodeId != id) continue;
							const auto keyBytes = KeyframePayloadBytes(document.Keyframes[keyIndex]);
							if (document.Keyframes.size() == Limits::MaximumKeyframes || !keyBytes ||
								!admit(*keyBytes + textBytes(copiedId)) ||
								!reserve(document.Keyframes, document.Keyframes.size() + 1))
								return fail("instance local animator clone exceeds operation bounds");
							std::string copiedName(copiedId);
							Keyframe key = document.Keyframes[keyIndex];
							key.NodeId.swap(copiedName);
							document.Keyframes.push_back(std::move(key));
						}
						const size_t originalTrackCount = document.Tracks.size();
						for (size_t trackIndex = 0; trackIndex < originalTrackCount; ++trackIndex) {
							const auto &track = document.Tracks[trackIndex];
							if (track.NodeId != id) continue;
							const auto names = textBytes(track.NodeId) + textBytes(track.Port) +
											   textBytes(track.End) + textBytes(copiedId);
							if (document.Tracks.size() == Limits::MaximumTracks || !admit(names) ||
								!reserve(document.Tracks, document.Tracks.size() + 1))
								return fail("instance local track clone exceeds operation bounds");
							std::string copiedName(copiedId);
							AnimationTrack copiedTrack = document.Tracks[trackIndex];
							copiedTrack.NodeId.swap(copiedName);
							document.Tracks.push_back(std::move(copiedTrack));
						}
					} else {
						if (!holdText(*match))
							return fail("instance match identity exceeds operation bounds");
						copiedId = *match;
					}
					auto copied = nodeById(copiedId);
					if (!holdText(id)) return fail("instance child base name exceeds operation bounds");
					std::string baseName(id);
					copied->InstanceBase.swap(baseName);
					copied->Position = nodeById(id)->Position;
					if (!mapId(id, copiedId) || !useId(copiedId))
						return fail("instance mapping exceeds operation bounds");
					// Generic Node.clone is shallow for Pixel Builder: its collection has no
					// onClone callback. Matched builders retain their local children and sockets.
					const auto sourceScope = groupById(id);
					if (sourceScope != document.Groups.end() && sourceScope->OwnerNodeId == id) {
						if (nodeById(copiedId)->Type != "pc.pixel_builder")
							return fail("owned source collection class is not represented by shallow clone");
						auto targetScope = groupById(copiedId);
						if (targetScope == document.Groups.end()) {
							if (!sourceScope->Ports.empty())
								return fail(
									"shallow Pixel Builder clone has no source callback to construct custom "
									"sockets"
								);
							if (document.Groups.size() == Limits::MaximumGroups || !admit(sizeof(Group)) ||
								!holdText(copiedId) || !holdText(copiedId) || !holdText(sourceScope->Name) ||
								!holdText(nodeById(copiedId)->GroupId) ||
								!reserve(document.Groups, document.Groups.size() + 1))
								return fail("shallow owned collection storage exceeds operation bounds");
							const auto originalScope = groupById(id);
							Group scope{copiedId, originalScope->Name};
							scope.ParentId = nodeById(copiedId)->GroupId;
							scope.OwnerNodeId = copiedId;
							scope.ColorDepth = originalScope->ColorDepth;
							scope.Interpolation = originalScope->Interpolation;
							scope.Oversample = originalScope->Oversample;
							document.Groups.push_back(std::move(scope));
							targetScope = groupById(copiedId);
						}
						if (targetScope->OwnerNodeId != copiedId ||
							targetScope->ParentId != nodeById(copiedId)->GroupId)
							return fail("matched collection has a foreign local owner");
						for (PortDirection direction : {PortDirection::Input, PortDirection::Output}) {
							const auto &sourcePorts = groupById(id)->Ports;
							const auto &targetPorts = groupById(copiedId)->Ports;
							auto from = sourcePorts.begin(), to = targetPorts.begin();
							while (true) {
								from = std::find_if(from, sourcePorts.end(), [&](const GroupPort &port) {
									return port.Direction == direction;
								});
								to = std::find_if(to, targetPorts.end(), [&](const GroupPort &port) {
									return port.Direction == direction;
								});
								if (from == sourcePorts.end() || to == targetPorts.end()) {
									if (from != sourcePorts.end() || to != targetPorts.end())
										return fail("matched Pixel Builder custom socket counts differ");
									break;
								}
								if (!mapId(from->JunctionId, to->JunctionId))
									return fail("owned collection socket remap exceeds bounds");
								++from;
								++to;
							}
						}
					}
				}
				for (PortDirection direction : {PortDirection::Input, PortDirection::Output}) {
					ImportVector<GroupPort> basePorts{ImportAllocator<GroupPort>{budget}};
					ImportVector<GroupPort> targetPorts{ImportAllocator<GroupPort>{budget}};
					const auto holdPort = [&](const GroupPort &port) {
						return holdText(port.Id) && holdText(port.JunctionId) && holdText(port.ControlNodeId);
					};
					for (const auto &port : groupById(baseId)->Ports)
						if (port.Direction == direction) {
							if (!holdPort(port))
								return fail("instance input socket names exceed operation bounds");
							basePorts.push_back(port);
						}
					for (const auto &port : groupById(targetId)->Ports)
						if (port.Direction == direction) {
							if (!holdPort(port))
								return fail("instance output socket names exceed operation bounds");
							targetPorts.push_back(port);
						}
					for (size_t index = 0; index < basePorts.size(); ++index) {
						if (index >= targetPorts.size()) {
							if (document.Nodes.size() == Limits::MaximumNodes ||
								document.Junctions.size() == Limits::MaximumJunctions ||
								groupById(targetId)->Ports.size() == Limits::MaximumGroupPorts)
								return fail("instance boundary expansion exceeds count bounds");
							std::string id;
							if (!newId(targetId, basePorts[index].ControlNodeId, id) ||
								id.size() > Limits::MaximumTextBytes - 13)
								return fail("instance boundary identity exceeds limits");
							if (!admit(textBytes(id) + 13))
								return fail("instance junction name exceeds operation bounds");
							const std::string junctionId = id + "/parent-value";
							const auto original = nodeById(basePorts[index].ControlNodeId);
							const auto bytes = NodeClonePayloadBytes(*original);
							if (!bytes || !admit(*bytes + id.size() + junctionId.size() + targetId.size()))
								return fail(
									"instance boundary clone exceeds document bounds "
									"before copying"
								);
							if (!reserve(document.Nodes, document.Nodes.size() + 1) ||
								!reserve(document.Junctions, document.Junctions.size() + 1) ||
								!reserve(groupById(targetId)->Ports, groupById(targetId)->Ports.size() + 1))
								return fail("instance boundary storage exceeds operation bounds");
							// Admit both boundary GroupId copies and all port names before
							// constructing them.
							if (!holdText(id) || !holdText(junctionId) || !holdText(id) || !holdText(id) ||
								!holdText(junctionId) || !holdText(id) || !holdText(junctionId) ||
								!holdText(targetId))
								return fail("instance boundary copied names exceed operation bounds");
							const auto currentOriginal = nodeById(basePorts[index].ControlNodeId);
							Node copied{
								id,
								currentOriginal->Type,
								targetId,
								currentOriginal->Position,
								{},
								{},
								currentOriginal->Id,
								{},
								currentOriginal->SourceAnimatedInputs,
								currentOriginal->SourceStaticInputs
							};
							document.Nodes.push_back(std::move(copied));
							document.Junctions.push_back(
								{junctionId,
								 targetId,
								 ValueType::Any,
								 direction == PortDirection::Input ? std::optional<Value>{0.0} : std::nullopt}
							);
							GroupPort port{id, junctionId, direction, id};
							groupById(targetId)->Ports.push_back(port);
							targetPorts.push_back(std::move(port));
						}
						if (!mapId(basePorts[index].ControlNodeId, targetPorts[index].ControlNodeId) ||
							!mapId(basePorts[index].JunctionId, targetPorts[index].JunctionId) ||
							!holdText(basePorts[index].ControlNodeId))
							return fail("instance boundary mapping exceeds operation bounds");
						std::string baseName(basePorts[index].ControlNodeId);
						nodeById(targetPorts[index].ControlNodeId)->InstanceBase.swap(baseName);
						if (!useId(targetPorts[index].ControlNodeId))
							return fail("instance used identities exceed operation bounds");
					}
					for (size_t index = basePorts.size(); index < targetPorts.size(); ++index) {
						const auto &removed = targetPorts[index];
						std::erase_if(document.Nodes, [&](const Node &n) {
							return n.Id == removed.ControlNodeId;
						});
						std::erase_if(document.Keyframes, [&](const Keyframe &key) {
							return key.NodeId == removed.ControlNodeId;
						});
						std::erase_if(document.Tracks, [&](const AnimationTrack &track) {
							return track.NodeId == removed.ControlNodeId;
						});
						std::erase_if(document.Junctions, [&](const Junction &j) {
							return j.Id == removed.JunctionId;
						});
						std::erase_if(groupById(targetId)->Ports, [&](const GroupPort &p) {
							return p.Id == removed.Id;
						});
						std::erase_if(document.Links, [&](const Link &l) {
							return l.FromNode == removed.ControlNodeId || l.ToNode == removed.ControlNodeId ||
								   l.FromNode == removed.JunctionId || l.ToNode == removed.JunctionId;
						});
					}
				}
				IdSet removed(std::less<>{}, ImportAllocator<std::string>{budget});
				IdSet removedGroups(std::less<>{}, ImportAllocator<std::string>{budget});
				const auto markRemoved = [&](auto &ids, std::string_view id) {
					if (ids.contains(id)) return true;
					if (!holdText(id)) return false;
					ids.emplace(id);
					return true;
				};
				for (const std::string &id : targetOrder) {
					const auto node = nodeById(id);
					if (!used.contains(id) && groupById(id) != document.Groups.end() &&
						!markRemoved(removedGroups, id))
						return fail("instance removed collection names exceed operation bounds");
					if (node != document.Nodes.end() && node->Type != "pc.group_input" &&
						node->Type != "pc.group_output" && !used.contains(id)) {
						if (!holdText(id)) return fail("instance removal identities exceed operation bounds");
						removed.insert(id);
					}
				}
				// Source collection disable recursively deactivates descendants. Keep their archived
				// records, but remove the inactive subtree from the executable native projection.
				for (size_t pass = 0; pass < document.Groups.size(); ++pass) {
					bool changed = false;
					for (const Group &group : document.Groups)
						if (removedGroups.contains(group.ParentId) && !removedGroups.contains(group.Id)) {
							if (!markRemoved(removedGroups, group.Id))
								return fail("instance removed descendant names exceed operation bounds");
							changed = true;
						}
					if (!changed) break;
				}
				for (const Node &node : document.Nodes)
					if (removedGroups.contains(node.GroupId) && !markRemoved(removed, node.Id))
						return fail("instance removed child names exceed operation bounds");
				for (const Junction &junction : document.Junctions)
					if (removedGroups.contains(junction.GroupId) && !markRemoved(removed, junction.Id))
						return fail("instance removed boundary names exceed operation bounds");
				std::erase_if(document.Groups, [&](const Group &group) {
					return removedGroups.contains(group.Id);
				});
				std::erase_if(document.Junctions, [&](const Junction &junction) {
					return removed.contains(junction.Id);
				});
				std::erase_if(document.Outputs, [&](const Output &output) {
					return removed.contains(output.NodeId) || removedGroups.contains(output.NodeId);
				});
				std::erase_if(document.Nodes, [&](const Node &n) { return removed.contains(n.Id); });
				std::erase_if(document.Keyframes, [&](const Keyframe &key) {
					return removed.contains(key.NodeId);
				});
				std::erase_if(document.Tracks, [&](const AnimationTrack &track) {
					return removed.contains(track.NodeId);
				});
				ImportVector<Link> replacement{ImportAllocator<Link>{budget}};
				for (const Link &link : document.Links)
					if (mapping.contains(link.FromNode) && mapping.contains(link.ToNode)) {
						if (replacement.size() == Limits::MaximumLinks)
							return fail("instance links exceed count bounds");
						if (!holdText(mapping.at(link.FromNode)) || !holdText(link.FromPort) ||
							!holdText(mapping.at(link.ToNode)) || !holdText(link.ToPort))
							return fail("instance remapped link names exceed operation bounds");
						replacement.push_back(
							{mapping.at(link.FromNode), link.FromPort, mapping.at(link.ToNode), link.ToPort}
						);
					}
				std::erase_if(document.Links, [&](const Link &link) {
					if (removed.contains(link.FromNode) || removed.contains(link.ToNode) ||
						removedGroups.contains(link.FromNode) || removedGroups.contains(link.ToNode))
						return true;
					if (!used.contains(link.ToNode)) return false;
					return link.ToPort != "parent_value";
				});
				for (Link &link : replacement) {
					const bool duplicate =
						std::any_of(document.Links.begin(), document.Links.end(), [&](const Link &existing) {
							return existing == link;
						});
					if (duplicate) continue;
					if (document.Links.size() == Limits::MaximumLinks)
						return fail("instance remapped links exceed count bounds");
					if (!reserve(document.Links, document.Links.size() + 1))
						return fail("instance remapped link storage exceeds operation bounds");
					document.Links.push_back(std::move(link));
				}
				active.erase(targetId);
				if (!holdText(targetId)) return fail("instance completed identities exceed operation bounds");
				complete.insert(targetId);
				return true;
			};
			ImportVector<std::string> instances{ImportAllocator<std::string>{budget}};
			for (const Group &group : document.Groups)
				if (!group.InstanceBase.empty()) {
					if (!holdText(group.Id)) return fail("instance operation identities exceed bounds");
					instances.push_back(group.Id);
				}
			for (const auto &id : instances)
				if (groupById(id) != document.Groups.end() && !resolve(resolve, id)) return false;
			const auto finalBytes = DocumentRetainedPayloadBytes(document);
			if (!finalBytes || *finalBytes > Limits::MaximumDocumentBytes)
				return fail("instance expanded document exceeds retained document bounds");
			return true;
		} catch (const std::bad_alloc &) {
			return fail("instance operation exceeds allocation budget");
		}
	}
} // namespace engine::imagegraphio::detail
