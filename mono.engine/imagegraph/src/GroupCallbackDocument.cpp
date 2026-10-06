#include "GroupCallbackOpaque.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <type_traits>

namespace engine::imagegraph {
	Status PrepareGroupCallbackDocument(
		const Document &document, Document &result, Diagnostic &diagnostic, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.group_callback_document");
		const auto fail = [&](const char *message) {
			diagnostic = {Status::LimitExceeded, {}, {}, message};
			return diagnostic.Code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			document.Nodes.size() > Limits::MaximumNodes || document.Links.size() > Limits::MaximumLinks ||
			document.Groups.size() > Limits::MaximumGroups ||
			document.Junctions.size() > Limits::MaximumJunctions ||
			document.Outputs.size() > Limits::MaximumOutputs ||
			document.Keyframes.size() > Limits::MaximumKeyframes ||
			document.Tracks.size() > Limits::MaximumTracks)
			return fail("Group callback document exceeds count or byte bounds");
		const auto inputBytes = DocumentRetainedPayloadBytes(document);
		const auto resultBytes =
			&document == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		if (!inputBytes || !resultBytes || *inputBytes >= maximumBytes ||
			*resultBytes >= maximumBytes - *inputBytes)
			return fail("Group callback document owners exceed payload bounds");
		uint64_t remaining = maximumBytes - *inputBytes - *resultBytes;
		constexpr uint64_t indexBytes = sizeof(std::array<std::string_view, Limits::MaximumNodes>);
		uint64_t replacementBytes = 0;
		bool hasOpaque = false;
		for (const auto &node : document.Nodes) {
			if (node.Id.empty() || node.Type.empty() || !std::isfinite(node.Position.X) ||
				!std::isfinite(node.Position.Y)) {
				diagnostic = {
					Status::InvalidValue, node.Id, {}, "Group callback source node identity is invalid"
				};
				return diagnostic.Code;
			}
			if (node.Id.size() > Limits::MaximumTextBytes || node.Type.size() > Limits::MaximumTextBytes ||
				node.GroupId.size() > Limits::MaximumTextBytes ||
				node.SourceDisplayName.size() > Limits::MaximumTextBytes ||
				node.SourceInternalName.size() > Limits::MaximumTextBytes)
				return fail("Group callback source node text exceeds bounds");
			if (FindSchema(node.Type)) continue;
			hasOpaque = true;
			const uint64_t bytes = sizeof(Node) + 2 * sizeof(AuthoredValue) + node.Id.capacity() +
								   node.GroupId.capacity() + node.SourceDisplayName.capacity() +
								   node.SourceInternalName.capacity() + node.Type.capacity() + 128;
			if (bytes >= remaining || replacementBytes >= remaining - bytes)
				return fail("Group callback opaque replacements exceed payload bounds");
			replacementBytes += bytes;
		}
		if (!hasOpaque && &document == &result) {
			diagnostic = {};
			return Status::Ok;
		}
		if (hasOpaque) {
			if (indexBytes >= remaining) return fail("Group callback identity index exceeds payload bounds");
			remaining -= indexBytes;
		}
		// grug reserve the old copy and replacement overlap before touching the destination.
		if (*inputBytes >= remaining || replacementBytes >= remaining - *inputBytes)
			return fail("Group callback document clone exceeds payload bounds");
		Document candidate = document;
		for (auto &node : candidate.Nodes) {
			if (FindSchema(node.Type)) continue;
			Node opaque;
			opaque.Id = node.Id;
			opaque.Type = detail::GroupCallbackOpaqueType;
			opaque.GroupId = node.GroupId;
			opaque.Position = node.Position;
			opaque.SourceDisplayName = node.SourceDisplayName;
			opaque.SourceInternalName = node.SourceInternalName;
			opaque.Values.reserve(1);
			AuthoredValue originalType;
			originalType.Port = "source_type";
			originalType.Data = std::move(node.Type);
			opaque.Values.push_back(std::move(originalType));
			node = std::move(opaque);
		}
		if (!hasOpaque) {
			static_assert(std::is_nothrow_move_assignable_v<Document>);
			result = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		}
		std::array<std::string_view, Limits::MaximumNodes> opaqueIds{};
		size_t opaqueCount = 0, longest = 0;
		for (const auto &node : candidate.Nodes)
			if (detail::IsGroupCallbackOpaque(node)) {
				opaqueIds[opaqueCount++] = node.Id;
				longest = std::max(longest, node.Id.size());
			}
		for (const auto &key : candidate.Keyframes)
			longest = std::max(longest, key.NodeId.size());
		for (const auto &track : candidate.Tracks)
			longest = std::max(longest, track.NodeId.size());
		for (const auto &action : candidate.SliceStackActions)
			longest = std::max(longest, action.NodeId.size());
		const uint64_t lookups =
			candidate.Keyframes.size() + candidate.Tracks.size() + candidate.SliceStackActions.size();
		// grug bound worst-case string bytes before sorting and binary searches.
		const uint64_t comparisons = (opaqueCount + lookups) * 32;
		if (longest > Limits::MaximumTextBytes || (comparisons && longest + 1 > 64'000'000 / comparisons))
			return fail("Group callback opaque timeline filtering exceeds work bounds");
		auto names = std::span(opaqueIds).first(opaqueCount);
		std::sort(names.begin(), names.end());
		const auto opaqueId = [&](std::string_view id) {
			return std::binary_search(names.begin(), names.end(), id);
		};
		std::erase_if(candidate.Keyframes, [&](const auto &key) { return opaqueId(key.NodeId); });
		std::erase_if(candidate.Tracks, [&](const auto &track) { return opaqueId(track.NodeId); });
		std::erase_if(candidate.SliceStackActions, [&](const auto &action) {
			return opaqueId(action.NodeId);
		});
		const auto bytes = DocumentRetainedPayloadBytes(candidate);
		if (!bytes || *bytes >= remaining)
			return fail("Group callback scratch exceeds retained payload bounds");
		static_assert(std::is_nothrow_move_assignable_v<Document>);
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "Group callback document allocation refused"};
		return diagnostic.Code;
	}
}
