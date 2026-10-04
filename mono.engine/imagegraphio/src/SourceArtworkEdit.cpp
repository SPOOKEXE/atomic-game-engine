#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraphio/SourceArtworkEdit.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraphio {
	using namespace imagegraph;
	namespace {
		const CatalogueInput *SourceInput(const CatalogueEntry &entry, int32_t index) {
			for (const auto &input : entry.Inputs)
				if (input.SourceIndex == index) return &input;
			return nullptr;
		}
		const CatalogueOutput *ContentOutput(const CatalogueEntry &entry) {
			for (const auto &output : entry.Outputs)
				if (output.SourceIndex == 1) return &output;
			return nullptr;
		}
		uint64_t MetadataBytes(const SourceArtworkMetadata &value) {
			uint64_t bytes = sizeof(value) + value.Layers.capacity() * sizeof(SourceArtworkLayer) +
							 value.Tags.capacity() * sizeof(SourceArtworkTag);
			for (const auto &layer : value.Layers)
				bytes += layer.Name.capacity() + 1;
			for (const auto &tag : value.Tags)
				bytes += tag.Name.capacity() + 1;
			return bytes;
		}
	}
	Status ApplySourceArtworkEdit(
		const Document &document,
		const HostNodeCapture &prepared,
		const GroupReplayState &replay,
		const SourceArtworkEditOptions &options,
		Document &result,
		GroupReplayState &resultReplay,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraphio.artwork_edit");
		const auto fail = [&](Status code, std::string message, std::string_view port = {}) {
			diagnostic = {code, prepared.Authored.Id, std::string(port), std::move(message)};
			return code;
		};
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (options.Action != SourceArtworkAction::GenerateLayers &&
			options.Action != SourceArtworkAction::MatchFrames &&
			options.Action != SourceArtworkAction::ImportTags)
			return fail(Status::InvalidValue, "unknown source artwork authoring action");
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		const auto resultBytes =
			&document == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		const auto captureBytes = HostCaptureRetainedPayloadBytes(prepared);
		if (!documentBytes || !resultBytes || !captureBytes || *documentBytes > maximumBytes ||
			*resultBytes > maximumBytes - *documentBytes ||
			*captureBytes > maximumBytes - *documentBytes - *resultBytes ||
			replay.RetainedBytes() > maximumBytes - *documentBytes - *resultBytes - *captureBytes)
			return fail(Status::LimitExceeded, "artwork transaction retained owners exceed operation bounds");
		const uint64_t priorReplay = &resultReplay == &replay ? 0 : resultReplay.RetainedBytes();
		if (priorReplay >
			maximumBytes - *documentBytes - *resultBytes - *captureBytes - replay.RetainedBytes())
			return fail(Status::LimitExceeded, "prior artwork result owner exceeds operation bounds");
		const uint64_t external = *documentBytes + *resultBytes + *captureBytes + priorReplay;
		const auto selected =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
				return node.Id == prepared.Authored.Id;
			});
		if (selected == document.Nodes.end() || *selected != prepared.Authored ||
			!ValidFrameTime({prepared.Tick, prepared.Subframe, prepared.NegativeFrame}) ||
			!replay.InstancesBound() || replay.AuthoringRevision() != options.AuthoringRevision ||
			!options.NextAuthoringRevision || options.NextAuthoringRevision == options.AuthoringRevision)
			return fail(
				Status::InvalidValue, "artwork transaction requires current prepared node and bound replay"
			);
		if (selected->Type != "pc.ase_file_read" && selected->Type != "pc.ora_file_read" &&
			selected->Type != "pc.krita_file_read")
			return fail(Status::UnknownNode, "artwork callback requires a mapped source file node");
		if (options.CanvasGroup &&
			(options.CanvasGroup->size() > Limits::MaximumTextBytes ||
			 (!options.CanvasGroup->empty() &&
			  std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
				  return group.Id == *options.CanvasGroup;
			  }))))
			return fail(Status::InvalidValue, "artwork target canvas group is absent or exceeds name bounds");
		SourceArtworkMetadata metadata;
		if (ReadSourceArtworkMetadata(
				prepared,
				metadata,
				diagnostic,
				maximumBytes - *documentBytes - *resultBytes - priorReplay - replay.RetainedBytes()
			) != Status::Ok)
			return diagnostic.Code;
		const bool ase = selected->Type == "pc.ase_file_read";
		const char *layerType = ase									   ? "pc.ase_layer"
								: selected->Type == "pc.ora_file_read" ? "pc.ora_layer"
																	   : "pc.krita_layer";
		const CatalogueEntry *layerEntry = nullptr;
		Node constructor;
		const auto emptyConstructorBytes = NodeClonePayloadBytes(constructor);
		if (!emptyConstructorBytes)
			return fail(Status::LimitExceeded, "artwork empty constructor exceeds payload bounds");
		uint64_t constructorBytes = *emptyConstructorBytes;
		if (options.Action == SourceArtworkAction::GenerateLayers) {
			layerEntry = FindCatalogueEntry(layerType);
			if (!layerEntry) return fail(Status::UnknownNode, "artwork layer declarations are unavailable");
			const uint64_t metadataHeld = external + MetadataBytes(metadata) + replay.RetainedBytes();
			const auto emptyBytes = NodeClonePayloadBytes(constructor);
			if (!emptyBytes || metadataHeld > maximumBytes)
				return fail(Status::LimitExceeded, "artwork constructor overlap exceeds operation bounds");
			uint64_t admission = *emptyBytes + std::char_traits<char>::length(layerType);
			// These three source constructors have primitive defaults. Reserve both empty
			// tables once, and admit their port/default text before catalogue decoding.
			for (const auto &input : layerEntry->Inputs) {
				if (!input.Default.empty() && input.Type != ValueType::Boolean &&
					input.Type != ValueType::Text && input.Type != ValueType::Enum)
					return fail(
						Status::UnsupportedExecution,
						"artwork constructor default requires an unsupported native profile"
					);
				admission += sizeof(AuthoredValue) + sizeof(std::string) + sizeof(Value) +
							 2 * (std::max(input.Id.size(), std::string{}.capacity()) + 1) +
							 2 * (std::max(input.Default.size(), std::string{}.capacity()) + 1);
			}
			if (admission > maximumBytes - metadataHeld)
				return fail(Status::LimitExceeded, "artwork constructor tables exceed operation bounds");
			constructor.Values.reserve(layerEntry->Inputs.size());
			constructor.SourceStaticInputs.reserve(layerEntry->Inputs.size());
			if (constructor.Values.capacity() > layerEntry->Inputs.size() ||
				constructor.SourceStaticInputs.capacity() > layerEntry->Inputs.size())
				return fail(Status::LimitExceeded, "artwork constructor backing exceeds admitted slots");
			constructor.Type = layerType;
			for (const auto &input : layerEntry->Inputs) {
				if (std::any_of(
						layerEntry->Schema.Properties.begin(),
						layerEntry->Schema.Properties.end(),
						[&](const auto &property) { return property.Id == input.Id; }
					))
					if (auto value = CatalogueDefault(input))
						constructor.Values.push_back({std::string(input.Id), std::move(*value)});
				if (input.SourceIndex >= 0 && input.SourceKind != "Trigger")
					constructor.SourceStaticInputs.emplace_back(input.Id);
			}
			// The source writer stores physical sockets in index order. Match its import
			// projection even when catalogue declarations use a different display order.
			const auto sourceIndex = [&](std::string_view port) {
				const auto *input = FindCatalogueInput(*layerEntry, port);
				return input ? input->SourceIndex : INT32_MAX;
			};
			std::sort(
				constructor.Values.begin(), constructor.Values.end(), [&](const auto &a, const auto &b) {
					return sourceIndex(a.Port) < sourceIndex(b.Port);
				}
			);
			const auto measured = NodeClonePayloadBytes(constructor);
			if (!measured)
				return fail(Status::LimitExceeded, "artwork catalogue constructor exceeds payload bounds");
			if (*measured > admission)
				return fail(Status::LimitExceeded, "artwork constructor payload exceeds preflight admission");
			constructorBytes = *measured;
		}
		const uint64_t base = external + MetadataBytes(metadata) + constructorBytes;
		if (base > maximumBytes || replay.RetainedBytes() > maximumBytes - base ||
			*documentBytes > maximumBytes - base - replay.RetainedBytes())
			return fail(Status::LimitExceeded, "artwork candidate overlap exceeds operation bounds");
		// Admit conservative backing growth before cloning or reserving the transaction's tables.
		const uint64_t count = metadata.Layers.size();
		const uint64_t eventCount = count * (prepared.Authored.Type == "pc.ase_file_read" ? 3 : 1);
		uint64_t scratch =
			eventCount * (sizeof(GroupRefreshEvent) + sizeof(Value) + 3 * sizeof(std::string) + 128);
		uint64_t growth =
			(document.Nodes.size() + count) * sizeof(Node) + (document.Links.size() + count) * sizeof(Link);
		for (const auto &layer : metadata.Layers) {
			const uint64_t text =
				layer.Name.size() + prepared.Authored.Id.size() +
				(options.CanvasGroup ? options.CanvasGroup->size() : prepared.Authored.GroupId.size()) + 128;
			growth += text * 12 + constructorBytes * 2;
			scratch += text * 6;
		}
		for (const auto &tag : metadata.Tags)
			growth += sizeof(AnimationRegion) + tag.Name.size() + 32;
		const uint64_t owned = base + replay.RetainedBytes();
		if (scratch > maximumBytes - owned || growth > maximumBytes - owned - scratch ||
			*documentBytes > maximumBytes - owned - scratch - growth)
			return fail(
				Status::LimitExceeded, "artwork layout and event table admission exceeds operation bounds"
			);
		const uint64_t apiBase = base + scratch;
		Document candidate = document;
		GroupReplayState finalReplay;

		if (options.Action == SourceArtworkAction::MatchFrames) {
			if (!ase || !metadata.Frames || !candidate.Timeline)
				return fail(
					Status::UnsupportedExecution, "Match Frames requires mapped ASE and project timeline"
				);
			auto &timeline = *candidate.Timeline;
			if (!timeline.SourceBounds)
				return fail(
					Status::UnsupportedExecution, "Match Frames requires retained source bound intent"
				);
			timeline.Frames = metadata.Frames;
			if (ProjectSourceTimelineWindow(timeline, diagnostic) != Status::Ok) return diagnostic.Code;
		} else if (options.Action == SourceArtworkAction::ImportTags) {
			if (!ase) return fail(Status::UnsupportedExecution, "Import Tags requires prepared ASE content");
			if (!candidate.Project) candidate.Project.emplace();
			auto &regions = candidate.Project->AnimationRegions;
			if (regions.size() > Limits::MaximumAnimationRegions ||
				uint64_t(metadata.Tags.size()) * (regions.size() + metadata.Tags.size()) > 16'777'216)
				return fail(Status::LimitExceeded, "ASE region match work exceeds operation bounds");
			size_t additions = 0;
			for (size_t i = 0; i < metadata.Tags.size(); ++i) {
				bool matches = false;
				if (options.MatchRegionNames) {
					matches = std::any_of(regions.begin(), regions.end(), [&](const auto &region) {
						return region.Label == metadata.Tags[i].Name;
					});
					for (size_t j = 0; !matches && j < i; ++j)
						matches = metadata.Tags[j].Name == metadata.Tags[i].Name;
				}
				if (!matches) ++additions;
			}
			if (additions > Limits::MaximumAnimationRegions - regions.size())
				return fail(Status::LimitExceeded, "ASE region transaction exceeds source region bound");
			regions.reserve(regions.size() + additions);
			if (regions.capacity() > regions.size() + additions)
				return fail(Status::LimitExceeded, "ASE region backing exceeds admitted capacity");
			for (const auto &tag : metadata.Tags) {
				AnimationRegion *target = nullptr;
				if (options.MatchRegionNames)
					for (auto &region : regions)
						if (region.Label == tag.Name) target = &region;
				if (!target) {
					regions.emplace_back();
					target = &regions.back();
				}
				*target = {tag.Name, tag.Color, {tag.First + 1, 0, false}, {tag.Last + 1, 0, false}};
			}
		} else {
			const auto *sourceEntry = FindCatalogueEntry(selected->Type);
			const auto *content = sourceEntry ? ContentOutput(*sourceEntry) : nullptr;

			if (!content || !layerEntry)
				return fail(Status::UnknownNode, "artwork layer declarations are unavailable");
			uint64_t remainingWork = 16'777'216;
			bool workExceeded = false;
			const auto step = [&] {
				if (remainingWork == 0) {
					workExceeded = true;
					return false;
				}
				--remainingWork;
				return true;
			};
			size_t addedNodes = 0, addedLinks = 0;
			for (size_t i = 0; i < metadata.Layers.size(); ++i) {
				const auto &layer = metadata.Layers[i];
				if (!layer.Renderable) continue;
				bool previous = false;
				for (size_t j = 0; j < i; ++j) {
					if (!step()) break;
					if (metadata.Layers[j].Renderable && metadata.Layers[j].Name == layer.Name) {
						previous = true;
						break;
					}
				}
				if (workExceeded)
					return fail(
						Status::LimitExceeded, "artwork layout preflight exceeds operation work bounds"
					);
				if (previous) continue;
				// Saved source projects omit inactive tombstones. Native authored membership is
				// the structural active-node projection, independently of render/Active controls.
				const Node *reused = nullptr;
				for (const auto &link : document.Links) {
					if (!step()) break;
					if (link.FromNode != selected->Id || link.FromPort != content->Id) continue;
					const auto found =
						std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
							return step() && node.Id == link.ToNode;
						});
					if (found != document.Nodes.end() && found->SourceDisplayName == layer.Name) {
						reused = &*found;
						break;
					}
				}
				if (workExceeded)
					return fail(
						Status::LimitExceeded, "artwork layout preflight exceeds operation work bounds"
					);
				if (!reused) {
					++addedNodes;
					++addedLinks;
					continue;
				}
				const auto *entry = FindCatalogueEntry(reused->Type);
				const auto *data = entry ? SourceInput(*entry, 0) : nullptr;
				bool wired = false;
				if (data)
					for (const auto &link : document.Links) {
						if (!step()) break;
						if (link.ToNode == reused->Id && link.ToPort == data->Id) {
							wired = true;
							break;
						}
					}
				if (workExceeded)
					return fail(
						Status::LimitExceeded, "artwork layout preflight exceeds operation work bounds"
					);
				if (data && !wired) ++addedLinks;
			}
			if (addedNodes > Limits::MaximumNodes - document.Nodes.size() ||
				addedLinks > Limits::MaximumLinks - document.Links.size())
				return fail(Status::LimitExceeded, "artwork layer layout exceeds node or link bounds");
			struct Edit {
				std::string NodeId;
				std::string Port;
				Value Data;
				bool Animated = false;
			};
			candidate.Nodes.reserve(document.Nodes.size() + addedNodes);
			candidate.Links.reserve(document.Links.size() + addedLinks);
			if (candidate.Nodes.capacity() > document.Nodes.size() + addedNodes ||
				candidate.Links.capacity() > document.Links.size() + addedLinks)
				return fail(Status::LimitExceeded, "artwork layout backing exceeds admitted capacity");
			std::vector<Edit> edits;
			edits.reserve(eventCount);
			if (edits.capacity() > eventCount)
				return fail(Status::LimitExceeded, "artwork edit backing exceeds admitted slots");
			for (size_t i = 0; i < metadata.Layers.size(); ++i) {
				const auto &layer = metadata.Layers[i];
				if (!layer.Renderable) continue;
				size_t index = candidate.Nodes.size();
				for (const auto &link : candidate.Links) {
					if (!step()) break;
					if (link.FromNode != selected->Id || link.FromPort != content->Id) continue;
					const auto found =
						std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &node) {
							return step() && node.Id == link.ToNode;
						});
					if (found != candidate.Nodes.end() && found->SourceDisplayName == layer.Name) {
						index = size_t(found - candidate.Nodes.begin());
						break;
					}
				}
				if (workExceeded)
					return fail(
						Status::LimitExceeded, "artwork source consumer lookup exceeds operation work bounds"
					);
				if (index == candidate.Nodes.size()) {
					Node node = constructor;
					for (uint64_t serial = 0;; ++serial) {
						node.Id = "artwork/" + selected->Id + "/" + std::to_string(serial);
						if (std::none_of(
								candidate.Nodes.begin(),
								candidate.Nodes.end(),
								[&](const auto &old) { return step() && old.Id == node.Id; }
							) ||
							workExceeded)
							break;
					}
					if (workExceeded)
						return fail(
							Status::LimitExceeded,
							"artwork new node identity lookup exceeds operation work bounds"
						);
					node.Type = layerType;
					node.GroupId =
						options.CanvasGroup ? std::string(*options.CanvasGroup) : selected->GroupId;
					node.SourceDisplayName = layer.Name;
					node.SourceInternalName = layer.Name;
					std::replace(node.SourceInternalName.begin(), node.SourceInternalName.end(), ' ', '_');
					node.Position = {
						selected->Position.X + 160,
						selected->Position.Y - double(metadata.Layers.size() - 1) * 32 + double(i) * 64
					};

					candidate.Nodes.push_back(std::move(node));
				}
				auto &node = candidate.Nodes[index];
				const auto *entry = FindCatalogueEntry(node.Type);
				const auto *data = entry ? SourceInput(*entry, 0) : nullptr;
				if (!data || data->Type != ValueType::Object)
					return fail(
						Status::UnsupportedExecution,
						"source reused consumer has no representable artwork data setter",
						node.Id
					);
				const auto old =
					std::find_if(candidate.Links.begin(), candidate.Links.end(), [&](const auto &link) {
						return link.ToNode == node.Id && link.ToPort == data->Id;
					});
				if (old == candidate.Links.end())
					candidate.Links.push_back(
						{selected->Id, std::string(content->Id), node.Id, std::string(data->Id)}
					);
				else
					*old = {selected->Id, std::string(content->Id), node.Id, std::string(data->Id)};
				const auto set = [&](int32_t inputIndex, Value value, ValueType required) {
					const auto *input = SourceInput(*entry, inputIndex);
					if (!input || input->Type != required) return false;
					const auto *binding = replay.Binding(node.Id, input->Id);
					const bool staticInput =
						std::find(
							node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), input->Id
						) != node.SourceStaticInputs.end();
					const bool animated = binding ? binding->Getter == GroupSubtypeAnimator::Animated
												  : !staticInput && (std::find(
																		 node.SourceAnimatedInputs.begin(),
																		 node.SourceAnimatedInputs.end(),
																		 input->Id
																	 ) != node.SourceAnimatedInputs.end() ||
																	 std::any_of(
																		 document.Keyframes.begin(),
																		 document.Keyframes.end(),
																		 [&](const Keyframe &key) {
																			 return key.NodeId == node.Id &&
																					key.Port == input->Id;
																		 }
																	 ));
					edits.push_back({node.Id, std::string(input->Id), std::move(value), animated});
					return true;
				};
				bool crop = false;
				if (ase) {
					const auto *cropInput = SourceInput(*sourceEntry, 3);
					const auto value =
						std::find_if(prepared.Inputs.begin(), prepared.Inputs.end(), [&](const auto &input) {
							return cropInput && input.Port == cropInput->Id;
						});
					const auto *boolean =
						value == prepared.Inputs.end() ? nullptr : std::get_if<bool>(&value->Data);
					if (!boolean)
						return fail(Status::InvalidValue, "prepared ASE crop observation is absent");
					crop = *boolean;
				}
				if (!(ase ? set(1, crop, ValueType::Boolean) && set(2, layer.Name, ValueType::Text) &&
								set(3, layer.Loop, ValueType::Boolean)
						  : set(1, layer.Name, ValueType::Text)))
					return fail(
						Status::UnsupportedExecution,
						"source reused consumer setters have unsupported native domains",
						node.Id
					);
			}
			const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
			if (!candidateBytes || *candidateBytes > maximumBytes - apiBase - replay.RetainedBytes())
				return fail(Status::LimitExceeded, "generated artwork layout exceeds operation bounds");
			GroupReplayState bound, edited;
			if (RebindGroupReplay(
					candidate, replay, options.AuthoringRevision, bound, diagnostic, maximumBytes - apiBase
				) != Status::Ok)
				return diagnostic.Code;
			std::vector<GroupRefreshEvent> events;
			events.reserve(edits.size());
			if (events.capacity() > eventCount)
				return fail(Status::LimitExceeded, "artwork event backing exceeds admitted slots");
			for (const auto &edit : edits) {
				GroupRefreshEvent event;
				event.NodeId = edit.NodeId;
				event.Reason = GroupRefreshReason::Edit;
				event.At.Tick = prepared.Tick;
				event.At.Subframe = prepared.Subframe;
				event.At.NegativeFrame = prepared.NegativeFrame;
				event.ReplaceExistingKey = options.ReplaceExistingKeys;
				event.EditedPort = edit.Port;
				event.LocalValue = &edit.Data;
				event.LocalAnimated = edit.Animated;
				events.push_back(std::move(event));
			}
			uint64_t scratchHeld =
				edits.capacity() * sizeof(Edit) + events.capacity() * sizeof(GroupRefreshEvent);
			const auto chargeScratch = [&](uint64_t bytes) {
				if (scratchHeld > scratch || bytes > scratch - scratchHeld) return false;
				scratchHeld += bytes;
				return true;
			};
			for (const auto &edit : edits) {
				const auto payload = ValueClonePayloadBytes(edit.Data);
				if (!payload || !chargeScratch(edit.NodeId.capacity() + 1) ||
					!chargeScratch(edit.Port.capacity() + 1) || !chargeScratch(*payload))
					return fail(Status::LimitExceeded, "artwork edit payload exceeds preadmitted scratch");
			}
			for (const auto &event : events)
				if (!chargeScratch(event.NodeId.capacity() + 1))
					return fail(Status::LimitExceeded, "artwork event payload exceeds preadmitted scratch");
			// Each callee admits its document, previous owner and candidate owner. Keep all
			// other live owners outside that nested budget, including the edit/event scratch.
			if (ReplayGroupAnimatorEdits(
					candidate,
					events,
					bound,
					options.AuthoringRevision,
					edited,
					diagnostic,
					maximumBytes - apiBase - replay.RetainedBytes()
				) != Status::Ok)
				return diagnostic.Code;
			bound = {};
			std::vector<Edit>().swap(edits);
			std::vector<GroupRefreshEvent>().swap(events);
			Document projected;
			if (ProjectGroupReplay(
					candidate,
					edited,
					options.AuthoringRevision,
					projected,
					diagnostic,
					maximumBytes - apiBase - replay.RetainedBytes()
				) != Status::Ok)
				return diagnostic.Code;
			if (RebindProjectedGroupReplay(
					projected,
					edited,
					options.NextAuthoringRevision,
					finalReplay,
					diagnostic,
					maximumBytes - apiBase - replay.RetainedBytes() - *candidateBytes
				) != Status::Ok)
				return diagnostic.Code;
			candidate = std::move(projected);
		}

		const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
		if (!candidateBytes || *candidateBytes > maximumBytes - apiBase - replay.RetainedBytes())
			return fail(Status::LimitExceeded, "artwork authoring result exceeds operation bounds");
		if (options.Action != SourceArtworkAction::GenerateLayers && RebindProjectedGroupReplay(
																		 candidate,
																		 replay,
																		 options.NextAuthoringRevision,
																		 finalReplay,
																		 diagnostic,
																		 maximumBytes - apiBase
																	 ) != Status::Ok)
			return diagnostic.Code;
		if (finalReplay.RetainedBytes() > maximumBytes - apiBase - replay.RetainedBytes() - *candidateBytes)
			return fail(Status::LimitExceeded, "artwork result and rebound owner exceed operation bounds");
		core::Metrics::Count("imagegraphio.artwork_edit_bytes", *candidateBytes);
		result = std::move(candidate);
		resultReplay = std::move(finalReplay);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			Status::LimitExceeded, prepared.Authored.Id, {}, "artwork transaction allocation failed"
		};
		return diagnostic.Code;
	}
}
