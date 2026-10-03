#pragma once
#include "ImageGraphGroupHost.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>

namespace studio::detail {
	struct HlslInputAddress {
		size_t Group = 0, Field = 0;
	};
	inline std::optional<HlslInputAddress> HlslInputPort(std::string_view port) {
		constexpr std::string_view prefixes[]{"argument_name_", "argument_type_", "argument_value_"};
		for (size_t field = 0; field < 3; ++field) {
			if (!port.starts_with(prefixes[field])) continue;
			const auto suffix = port.substr(prefixes[field].size());
			size_t group = 0;
			const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), group);
			if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() ||
				std::to_string(group) != suffix)
				return {};
			return HlslInputAddress{group, field};
		}
		return {};
	}

	// The transaction owner publishes this staged document and its replay rebind
	// together. Original input and key identities survive physical port
	// renumbering.
	inline bool StageHlslGroupRemoval(
		engine::imagegraph::Document &document,
		std::string_view nodeId,
		size_t group,
		engine::imagegraph::Diagnostic &error
	) {
		using namespace engine::imagegraph;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
			return item.Id == nodeId;
		});
		const auto refuse = [&](std::string message) {
			error = {
				Status::InvalidValue,
				std::string(nodeId),
				"argument_name_" + std::to_string(group),
				std::move(message)
			};
			return false;
		};
		if (node == document.Nodes.end() || node->Type != "pc.hlsl")
			return refuse("Shader node does not exist");
		if (node->DynamicInputs.size() % 3 || node->DynamicInputs.size() > 3 * 64)
			return refuse("Shader argument groups are incomplete or exceed their limit");
		const size_t count = node->DynamicInputs.size() / 3;
		if (group >= count) return refuse("Shader argument group does not exist");
		std::array<bool, 3 * 64> present{};
		for (const auto &input : node->DynamicInputs) {
			const auto address = HlslInputPort(input.Id);
			if (!address || address->Group >= count || present[3 * address->Group + address->Field])
				return refuse("Shader argument layout is not a complete ordered group interface");
			present[3 * address->Group + address->Field] = true;
		}
		const auto removed = [&](std::string_view port) {
			const auto address = HlslInputPort(port);
			return address && address->Group == group;
		};
		const auto move = [&](std::string &port) {
			const auto address = HlslInputPort(port);
			if (!address || address->Group <= group) return;
			constexpr std::string_view prefixes[]{"argument_name_", "argument_type_", "argument_value_"};
			port = std::string(prefixes[address->Field]) + std::to_string(address->Group - 1);
		};
		std::erase_if(node->DynamicInputs, [&](const auto &item) { return removed(item.Id); });
		for (auto &input : node->DynamicInputs)
			move(input.Id);
		std::erase_if(node->Values, [&](const auto &item) { return removed(item.Port); });
		for (auto &input : node->Values)
			move(input.Port);
		for (auto *ports :
			 {&node->SourceStaticInputs, &node->SourceAnimatedInputs, &node->InstanceOverrides}) {
			std::erase_if(*ports, removed);
			for (auto &port : *ports)
				move(port);
		}
		std::erase_if(node->SourceInputExpressions, [&](const auto &item) { return removed(item.Port); });
		for (auto &input : node->SourceInputExpressions)
			move(input.Port);
		std::erase_if(document.Keyframes, [&](const auto &key) {
			return key.NodeId == nodeId && removed(key.Port);
		});
		for (auto &key : document.Keyframes)
			if (key.NodeId == nodeId) move(key.Port);
		std::erase_if(document.Tracks, [&](const auto &track) {
			return track.NodeId == nodeId && removed(track.Port);
		});
		for (auto &track : document.Tracks)
			if (track.NodeId == nodeId) move(track.Port);
		const auto bypassInput = [](std::string_view port) {
			constexpr std::string_view suffix = ".bypass";
			return port.ends_with(suffix) ? port.substr(0, port.size() - suffix.size()) : std::string_view{};
		};
		const auto removedBypass = [&](std::string_view port) {
			const auto input = bypassInput(port);
			return !input.empty() && removed(input);
		};
		const auto moveBypass = [&](std::string &port) {
			const auto input = bypassInput(port);
			if (input.empty()) return;
			std::string moved(input);
			move(moved);
			port = std::move(moved) + ".bypass";
		};
		std::erase_if(document.Links, [&](const auto &link) {
			return (link.ToNode == nodeId && removed(link.ToPort)) ||
				   (link.FromNode == nodeId && removedBypass(link.FromPort));
		});
		for (auto &link : document.Links) {
			if (link.ToNode == nodeId) move(link.ToPort);
			if (link.FromNode == nodeId) moveBypass(link.FromPort);
		}
		std::erase_if(document.Outputs, [&](const auto &output) {
			return output.NodeId == nodeId && removedBypass(output.Port);
		});
		for (auto &output : document.Outputs)
			if (output.NodeId == nodeId) moveBypass(output.Port);
		document.FormatVersion = std::max(document.FormatVersion, 9u);
		error = {};
		return true;
	}

	// Physical socket moves and original animator writers are admitted before
	// history. Neither owner is published when validation or undo admission fails.
	template <class BeforeCommit>
		requires std::invocable<BeforeCommit, engine::imagegraph::Document &, ImageGraphGroupHost &>
	inline bool ApplyHlslGroupRangeRemoval(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphGroupHost &host,
		uint64_t revision,
		std::string_view nodeId,
		size_t group,
		size_t removeCount,
		engine::imagegraph::Diagnostic &error,
		const BeforeCommit &beforeCommit,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const auto refuse = [&] {
			error = {
				Status::LimitExceeded,
				std::string(nodeId),
				{},
				"Shader group removal exceeds its live payload or undo budget"
			};
			return false;
		};
		const auto originalBytes = DocumentRetainedPayloadBytes(document);
		if (!originalBytes || *originalBytes > maximumBytes / 2 ||
			host.Replay.RetainedBytes() > maximumBytes - 2 * *originalBytes)
			return refuse();
		Document staged = document;
		if (!removeCount || removeCount > 64 || group > 64 - removeCount) {
			error = {Status::InvalidValue, std::string(nodeId), {}, "Shader removal range is invalid"};
			return false;
		}
		for (size_t count = removeCount; count; --count)
			if (!StageHlslGroupRemoval(staged, nodeId, group + count - 1, error)) return false;
		const auto original = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
			return n.Id == nodeId;
		});
		const auto target = std::find_if(staged.Nodes.begin(), staged.Nodes.end(), [&](const auto &n) {
			return n.Id == nodeId;
		});
		std::array<SourceInputMove, 3 * 64> moves{};
		size_t moveCount = 0;
		for (const auto &input : original->DynamicInputs) {
			const auto address = HlslInputPort(input.Id);
			if (address->Group < group) continue;
			std::string_view destination;
			if (address->Group >= group + removeCount) {
				const auto next = std::find_if(
					target->DynamicInputs.begin(), target->DynamicInputs.end(), [&](const auto &n) {
						const auto to = HlslInputPort(n.Id);
						return to && to->Field == address->Field && to->Group + removeCount == address->Group;
					}
				);
				destination = next->Id;
			}
			moves[moveCount++] = {original->Id, input.Id, destination};
		}
		ImageGraphGroupHost candidate;
		const auto stagedBytes = DocumentRetainedPayloadBytes(staged);
		if (!stagedBytes || *originalBytes > maximumBytes || *stagedBytes > maximumBytes - *originalBytes ||
			host.Replay.RetainedBytes() > maximumBytes - *originalBytes - *stagedBytes ||
			sizeof(moves) > maximumBytes - *originalBytes - *stagedBytes - host.Replay.RetainedBytes())
			return refuse();
		const uint64_t held = *originalBytes + *stagedBytes + host.Replay.RetainedBytes() + sizeof(moves);
		const uint64_t nextRevision = revision == UINT64_MAX ? 1 : revision + 1;
		if (RebindGroupReplayWithInputMoves(
				document,
				staged,
				{moves.data(), moveCount},
				host.Replay,
				nextRevision,
				candidate.Replay,
				error,
				maximumBytes - held
			) != Status::Ok)
			return false;
		candidate.Revision = nextRevision;
		candidate.BorrowedBytes = Limits::MaximumEvaluationBytes - maximumBytes + *originalBytes +
								  host.Replay.RetainedBytes() + sizeof(moves);
		if (!beforeCommit(staged, candidate)) return false;
		Plan plan;
		if (Compile(staged, plan, error) != Status::Ok) return false;
		const auto finalBytes = DocumentRetainedPayloadBytes(staged);
		if (!finalBytes || sizeof(moves) > maximumBytes - *originalBytes ||
			*finalBytes > maximumBytes - *originalBytes - sizeof(moves) ||
			host.Replay.RetainedBytes() > maximumBytes - *originalBytes - sizeof(moves) - *finalBytes ||
			candidate.Replay.RetainedBytes() >
				maximumBytes - *originalBytes - sizeof(moves) - *finalBytes - host.Replay.RetainedBytes())
			return refuse();
		if (!history.TryRecord(document, staged)) return refuse();
		candidate.BorrowedBytes = 0;
		document = std::move(staged);
		host = std::move(candidate);
		error = {};
		return true;
	} catch (const std::bad_alloc &) {
		error = {
			engine::imagegraph::Status::LimitExceeded,
			std::string(nodeId),
			{},
			"Shader group removal allocation was refused"
		};
		return false;
	}
	template <class BeforeCommit>
		requires std::invocable<BeforeCommit, engine::imagegraph::Document &, ImageGraphGroupHost &>
	inline bool ApplyHlslGroupRemoval(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphGroupHost &host,
		uint64_t revision,
		std::string_view nodeId,
		size_t group,
		engine::imagegraph::Diagnostic &error,
		const BeforeCommit &beforeCommit,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		return ApplyHlslGroupRangeRemoval(
			document, history, host, revision, nodeId, group, 1, error, beforeCommit, maximumBytes
		);
	}
	inline bool ApplyHlslGroupRemoval(
		engine::imagegraph::Document &document,
		ImageGraphHistory &history,
		ImageGraphGroupHost &host,
		uint64_t revision,
		std::string_view nodeId,
		size_t group,
		engine::imagegraph::Diagnostic &error,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) {
		return ApplyHlslGroupRemoval(
			document,
			history,
			host,
			revision,
			nodeId,
			group,
			error,
			[](auto &, auto &) { return true; },
			maximumBytes
		);
	}
} // namespace studio::detail
