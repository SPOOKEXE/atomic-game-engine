#include "ImageGraphCacheEditObservation.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <studio/ImageGraph.hpp>
#include <tuple>

namespace studio::detail {
	namespace {
		using namespace engine::imagegraph;
		constexpr uint64_t WORK_LIMIT = 64ull * 1024 * 1024;
		struct EditWork {
			uint64_t Remaining = WORK_LIMIT;
			bool Refused = false;
			bool Charge(uint64_t count, uint64_t bytes) {
				if (count && bytes > Remaining / count) {
					Refused = true;
					return false;
				}
				Remaining -= count * bytes;
				return true;
			}
			bool Same(std::string_view left, std::string_view right) {
				return Charge(1, std::max(left.size(), right.size()) + 1) && left == right;
			}
		};
		auto LinkKey(const Link &link) {
			return std::tie(link.ToNode, link.ToPort, link.FromNode, link.FromPort);
		}
		Document InputObservation(const Document &document) {
			Document inputs;
			inputs.Nodes.reserve(document.Nodes.size());
			for (const auto &node : document.Nodes) {
				Node input;
				input.Id = node.Id;
				input.Type = node.Type;
				input.GroupId = node.GroupId;
				input.Values = node.Values;
				input.DynamicInputs = node.DynamicInputs;
				input.SourceAnimatedInputs = node.SourceAnimatedInputs;
				input.SourceStaticInputs = node.SourceStaticInputs;
				input.SourceSeparatedVec2Animators = node.SourceSeparatedVec2Animators;
				input.NativeSamplerBindings = node.NativeSamplerBindings;
				std::sort(input.Values.begin(), input.Values.end(), [](const auto &left, const auto &right) {
					return left.Port < right.Port;
				});
				std::sort(input.SourceAnimatedInputs.begin(), input.SourceAnimatedInputs.end());
				std::sort(input.SourceStaticInputs.begin(), input.SourceStaticInputs.end());
				inputs.Nodes.push_back(std::move(input));
			}
			std::sort(inputs.Nodes.begin(), inputs.Nodes.end(), [](const auto &left, const auto &right) {
				return left.Id < right.Id;
			});
			inputs.Links = document.Links;
			std::sort(inputs.Links.begin(), inputs.Links.end(), [](const auto &left, const auto &right) {
				return LinkKey(left) < LinkKey(right);
			});
			inputs.Groups.reserve(document.Groups.size());
			for (const auto &group : document.Groups) {
				Group route;
				route.Id = group.Id;
				route.ParentId = group.ParentId;
				route.OwnerNodeId = group.OwnerNodeId;
				route.Ports = group.Ports;
				std::sort(route.Ports.begin(), route.Ports.end(), [](const auto &left, const auto &right) {
					return left.Id < right.Id;
				});
				inputs.Groups.push_back(std::move(route));
			}
			std::sort(inputs.Groups.begin(), inputs.Groups.end(), [](const auto &left, const auto &right) {
				return left.Id < right.Id;
			});
			inputs.Junctions = document.Junctions;
			std::sort(
				inputs.Junctions.begin(), inputs.Junctions.end(), [](const auto &left, const auto &right) {
					return left.Id < right.Id;
				}
			);
			inputs.Keyframes = document.Keyframes;
			inputs.Tracks = document.Tracks;
			const auto nodeOrder = [](const auto &left, const auto &right) {
				return left.NodeId < right.NodeId;
			};
			std::stable_sort(inputs.Keyframes.begin(), inputs.Keyframes.end(), nodeOrder);
			std::stable_sort(inputs.Tracks.begin(), inputs.Tracks.end(), nodeOrder);
			return inputs;
		}
		// Resolve changed junction destinations to real input owners without reading getters.
		bool AddDestination(
			const Document &document,
			std::string_view endpoint,
			std::string_view port,
			std::array<std::string_view, Limits::MaximumNodes> &edited,
			size_t &count,
			EditWork &work
		) {
			struct Endpoint {
				std::string_view Id, Port;
			};
			std::array<Endpoint, Limits::MaximumLinks + Limits::MaximumGroupPorts> pending{};
			size_t pendingCount = 0, next = 0;
			const auto add = [&](std::string_view id, std::string_view socket) {
				for (size_t i = 0; i < pendingCount; ++i)
					if (work.Same(pending[i].Id, id) && work.Same(pending[i].Port, socket)) return true;
				if (work.Refused || pendingCount == pending.size()) return false;
				pending[pendingCount++] = {id, socket};
				return true;
			};
			if (!add(endpoint, port)) return false;
			while (next < pendingCount) {
				const auto current = pending[next++];
				bool nodeFound = false;
				for (const auto &node : document.Nodes) {
					if (!work.Same(node.Id, current.Id)) continue;
					nodeFound = true;
					bool present = false;
					for (size_t i = 0; i < count; ++i)
						if (work.Same(edited[i], node.Id)) {
							present = true;
							break;
						}
					if (!present) {
						if (count == edited.size()) return false;
						edited[count++] = node.Id;
					}
					break;
				}
				if (work.Refused) return false;
				if (nodeFound) continue;
				for (const auto &link : document.Links)
					if (work.Same(link.FromNode, current.Id) && work.Same(link.FromPort, current.Port))
						if (!add(link.ToNode, link.ToPort)) return false;
				if (work.Refused) return false;
			}
			return true;
		}
	}
	bool ApplyImageGraphCacheGroupMember(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		engine::imagegraph::CapturedFeedbackHost &host,
		std::string_view ownerId,
		std::string_view memberId,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return host.ToggleSourceCacheGroupMember(
			document,
			ownerId,
			memberId,
			diagnostic,
			[&](const auto &before, const auto &after) { return history.TryRecord(before, after); },
			maximumBytes
		);
	}

	bool ObserveImageGraphCacheEdits(
		const Document &document,
		ImageGraphCacheEditObservation &observation,
		CapturedFeedbackHost &host,
		ImageGraphCacheEditKind kind,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("studio.imagegraph.cache_edit_observation");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		const bool pendingValue = observation.PendingValueEdit;
		if (kind == ImageGraphCacheEditKind::ValueSetter || kind == ImageGraphCacheEditKind::AnimatorUndo) {
			if (!pendingValue) observation.PendingValueKind = kind;
			observation.PendingValueEdit = true;
		}
		if (kind == ImageGraphCacheEditKind::RenderOnly && pendingValue)
			return fail(
				Status::UnsupportedExecution, "Retry the pending source value edit before a render-only edit"
			);
		if (pendingValue && kind != ImageGraphCacheEditKind::FreshDocument)
			kind = observation.PendingValueKind;
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
			return fail(Status::LimitExceeded, "Source cache edit observation byte cap is outside bounds");
		const auto authoredBytes = DocumentRetainedPayloadBytes(document);
		const auto priorBytes = DocumentRetainedPayloadBytes(observation.Inputs);
		const auto hostBytes = host.RetainedBytes();
		if (!authoredBytes || !priorBytes || *authoredBytes > maximumBytes / 2 ||
			*priorBytes > maximumBytes - 2 * *authoredBytes ||
			hostBytes > maximumBytes - 2 * *authoredBytes - *priorBytes)
			return fail(Status::LimitExceeded, "Source cache edit observations exceed live byte bounds");
		EditWork work;
		// Copy, sorting and payload comparisons have one cap across all edited inputs and routes.
		if (!work.Charge(4, *authoredBytes + *priorBytes))
			return fail(Status::LimitExceeded, "Source cache edit observations exceed copy work bounds");
		uint64_t names = 0;
		for (const auto &node : document.Nodes) {
			names += node.Id.size() + 1;
			for (const auto &value : node.Values)
				names += value.Port.size() + 1;
			for (const auto &port : node.SourceAnimatedInputs)
				names += port.size() + 1;
			for (const auto &port : node.SourceStaticInputs)
				names += port.size() + 1;
		}
		for (const auto &link : document.Links)
			names +=
				link.ToNode.size() + link.ToPort.size() + link.FromNode.size() + link.FromPort.size() + 4;
		for (const auto &group : document.Groups) {
			names += group.Id.size() + 1;
			for (const auto &port : group.Ports)
				names += port.Id.size() + 1;
		}
		for (const auto &junction : document.Junctions)
			names += junction.Id.size() + 1;
		for (const auto &key : document.Keyframes)
			names += key.NodeId.size() + 1;
		for (const auto &track : document.Tracks)
			names += track.NodeId.size() + 1;
		if (!work.Charge(64, names))
			return fail(Status::LimitExceeded, "Source cache input sorting exceeds name work bounds");
		auto candidate = InputObservation(document);
		const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
		if (!candidateBytes || *candidateBytes > *authoredBytes)
			return fail(Status::LimitExceeded, "Source cache edit observation clone exceeds admission");
		if (kind == ImageGraphCacheEditKind::FreshDocument || !observation.Ready) {
			if (kind == ImageGraphCacheEditKind::FreshDocument) {
				std::array<std::string_view, Limits::MaximumNodes> owners{};
				size_t ownerCount = 0;
				for (const auto &node : document.Nodes) {
					if (node.Type != "pc.cache" && node.Type != "pc.cache_array") continue;
					if (ownerCount == owners.size())
						return fail(Status::LimitExceeded, "loaded cache owner count exceeds bounds");
					owners[ownerCount++] = node.Id;
				}
				const uint64_t held = *authoredBytes + *priorBytes + *candidateBytes + hostBytes;
				if (held >= maximumBytes)
					return fail(Status::LimitExceeded, "fresh cache groups exceed live byte bounds");
				CapturedFeedbackHost fresh;
				if (!fresh.RefreshLoadedSourceCacheGroups(
						document, std::span(owners).first(ownerCount), diagnostic, maximumBytes - held
					))
					return false;
				host = std::move(fresh);
			}
			observation.Inputs = std::move(candidate);
			observation.Ready = true;
			observation.PendingValueEdit = false;
			diagnostic = {};
			return true;
		}
		std::array<std::string_view, Limits::MaximumNodes> edited{};
		size_t count = 0;
		const auto add = [&](const Document &inputs, std::string_view id, std::string_view port) {
			return AddDestination(inputs, id, port, edited, count, work);
		};
		const auto &before = observation.Inputs;
		if (kind != ImageGraphCacheEditKind::RenderOnly) {
			size_t prior = 0;
			for (const auto &node : candidate.Nodes) {
				// GLOBAL setters only request RenderAll; no source cacheCheck is called.
				if (node.Type == "pc.global_scope") continue;
				while (prior < before.Nodes.size() && before.Nodes[prior].Id < node.Id)
					++prior;
				if (prior == before.Nodes.size() || before.Nodes[prior].Id != node.Id ||
					before.Nodes[prior].Type != node.Type)
					continue;
				const auto &old = before.Nodes[prior];
				const bool modeChanged =
					old.SourceAnimatedInputs != node.SourceAnimatedInputs ||
					old.SourceStaticInputs != node.SourceStaticInputs ||
					bool(old.SourceSeparatedVec2Animators) != bool(node.SourceSeparatedVec2Animators);
				if (kind == ImageGraphCacheEditKind::AnimatorUndo && modeChanged) continue;
				bool changed = old.Values != node.Values || old.DynamicInputs != node.DynamicInputs ||
							   old.NativeSamplerBindings != node.NativeSamplerBindings ||
							   old.SourceSeparatedVec2Animators != node.SourceSeparatedVec2Animators;
				const auto keys = [&](const auto &left, const auto &right) {
					size_t a = 0, b = 0;
					for (;;) {
						while (a < left.size() && !work.Same(left[a].NodeId, node.Id)) {
							++a;
							if (work.Refused) return true;
						}
						while (b < right.size() && !work.Same(right[b].NodeId, node.Id)) {
							++b;
							if (work.Refused) return true;
						}
						if (a == left.size() || b == right.size())
							return a != left.size() || b != right.size();
						if (left[a] != right[b]) return true;
						++a;
						++b;
					}
				};
				if (!changed)
					changed =
						keys(before.Keyframes, candidate.Keyframes) || keys(before.Tracks, candidate.Tracks);
				if (changed && !add(candidate, node.Id, {}))
					return fail(Status::LimitExceeded, "Source cache edited inputs exceed route work bounds");
			}
		}
		if (kind != ImageGraphCacheEditKind::RenderOnly) {
			for (const auto *inputs : std::array<const Document *, 2>{&before, &candidate})
				for (const auto &junction : inputs->Junctions) {
					const auto &other = inputs == &before ? candidate : before;
					const auto found =
						std::find_if(other.Junctions.begin(), other.Junctions.end(), [&](const auto &item) {
							return work.Same(item.Id, junction.Id);
						});
					if (found != other.Junctions.end() && found->Default == junction.Default) continue;
					if (!add(*inputs, junction.Id, "value"))
						return fail(
							Status::LimitExceeded, "Source cache junction edits exceed route work bounds"
						);
				}
		}
		size_t oldLink = 0, newLink = 0;
		while (oldLink < before.Links.size() || newLink < candidate.Links.size()) {
			if (oldLink < before.Links.size() && newLink < candidate.Links.size() &&
				before.Links[oldLink] == candidate.Links[newLink]) {
				++oldLink;
				++newLink;
				continue;
			}
			if (newLink == candidate.Links.size() ||
				(oldLink < before.Links.size() &&
				 LinkKey(before.Links[oldLink]) < LinkKey(candidate.Links[newLink]))) {
				const auto &link = before.Links[oldLink++];
				if (!add(before, link.ToNode, link.ToPort))
					return fail(Status::LimitExceeded, "Source cache disconnected routes exceed work bounds");
			} else {
				const auto &link = candidate.Links[newLink++];
				if (!add(candidate, link.ToNode, link.ToPort))
					return fail(Status::LimitExceeded, "Source cache connected routes exceed work bounds");
			}
		}
		// Group route edits can change the effective destination without changing a raw link.
		for (const auto *inputs : std::array<const Document *, 2>{&before, &candidate})
			for (const auto &group : inputs->Groups) {
				const auto &other = inputs == &before ? candidate : before;
				const auto found =
					std::find_if(other.Groups.begin(), other.Groups.end(), [&](const auto &item) {
						return work.Same(item.Id, group.Id);
					});
				if (found != other.Groups.end() && found->Ports == group.Ports) continue;
				for (const auto &socket : group.Ports)
					if (!add(*inputs, socket.JunctionId, "value"))
						return fail(
							Status::LimitExceeded, "Source cache group route edits exceed work bounds"
						);
			}
		if (work.Refused)
			return fail(Status::LimitExceeded, "Source cache input edits exceed name work bounds");
		const uint64_t held = *authoredBytes + *priorBytes + *candidateBytes;
		if (held >= maximumBytes ||
			!host.NotifySourceInputEdits(
				document, std::span(edited).first(count), diagnostic, maximumBytes - held
			))
			return false;
		observation.Inputs = std::move(candidate);
		observation.PendingValueEdit = false;
		diagnostic = {};
		return true;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Source cache edit observation allocation refused"};
		return false;
	}
}
