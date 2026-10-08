#pragma once

#include "SourceTunnel.hpp"

#include <engine/imagegraph/CacheGroupReplay.hpp>

namespace engine::imagegraph::detail {
	inline bool ValidateSourceTunnelRegistryObservations(
		const Document &document,
		std::span<const SourceTunnelRegistryObservation> observations,
		Diagnostic &diagnostic
	) {
		if (observations.size() > Limits::MaximumNodes || observations.size() > document.Nodes.size()) {
			diagnostic = {Status::LimitExceeded, {}, {}, "tunnel registry observation count exceeds bounds"};
			return false;
		}
		uint64_t work = 0;
		for (size_t i = 0; i < observations.size(); ++i) {
			const auto &observation = observations[i];
			if (observation.NodeId.empty() || observation.NodeId.size() > Limits::MaximumTextBytes ||
				observation.Name.size() > Limits::MaximumTextBytes ||
				(observation.Scope && !std::isfinite(*observation.Scope))) {
				diagnostic = {
					Status::InvalidValue, observation.NodeId, "name", "tunnel registry observation is invalid"
				};
				return false;
			}
			auto node = document.Nodes.end();
			for (auto candidate = document.Nodes.begin(); candidate != document.Nodes.end(); ++candidate) {
				const uint64_t units = 1 + std::min(candidate->Id.size(), observation.NodeId.size());
				if (units > 64'000'000 - work) {
					diagnostic = {
						Status::LimitExceeded,
						observation.NodeId,
						"name",
						"tunnel observation lookup exceeds bounded text work"
					};
					return false;
				}
				work += units;
				if (candidate->Id == observation.NodeId) {
					node = candidate;
					break;
				}
			}
			if (node == document.Nodes.end() ||
				(node->Type != "pc.tunnel_in" && node->Type != "pc.tunnel_out") ||
				((node->Type == "pc.tunnel_in") != observation.Scope.has_value())) {
				diagnostic = {
					Status::InvalidValue,
					observation.NodeId,
					"name",
					"tunnel registry observation does not match source declaration"
				};
				return false;
			}
			for (size_t j = 0; j < i; ++j) {
				const auto bytes = std::min(observations[j].NodeId.size(), observation.NodeId.size());
				if (bytes + 1 > 64'000'000 - work) {
					diagnostic = {
						Status::LimitExceeded,
						observation.NodeId,
						"name",
						"tunnel registry observation comparison exceeds bounded work"
					};
					return false;
				}
				work += bytes + 1;
				if (observations[j].NodeId == observation.NodeId) {
					diagnostic = {
						Status::DuplicateId,
						observation.NodeId,
						"name",
						"tunnel registry observation identity is duplicated"
					};
					return false;
				}
			}
		}
		return true;
	}
	// grug reads prior output before current producers run. Fresh output uses exact source constructor.
	template <class Constructor>
	const Value *FindSourceTunnelPriorValue(
		const Document &document,
		const Link &link,
		const CacheGroupReplayState *previous,
		Constructor constructor,
		Diagnostic &diagnostic,
		uint64_t &work
	) {
		const auto spend = [&](size_t bytes) {
			if (bytes + 1 > 64'000'000 - work) {
				diagnostic = {
					Status::LimitExceeded,
					link.ToNode,
					link.ToPort,
					"tunnel prior lookup exceeds bounded text work"
				};
				return false;
			}
			work += bytes + 1;
			return true;
		};
		auto source = document.Nodes.end();
		for (auto candidate = document.Nodes.begin(); candidate != document.Nodes.end(); ++candidate) {
			if (!spend(std::min(candidate->Id.size(), link.FromNode.size()))) return nullptr;
			if (candidate->Id == link.FromNode) {
				source = candidate;
				break;
			}
		}
		if (source == document.Nodes.end()) {
			diagnostic = {
				Status::InvalidValue,
				link.FromNode,
				link.FromPort,
				"tunnel registry producer identity is absent"
			};
			return nullptr;
		}
		if (previous) {
			for (const auto &node : previous->Nodes) {
				if (!spend(
						std::min(node.NodeId.size(), source->Id.size()) +
						std::min(node.NodeType.size(), source->Type.size())
					))
					return nullptr;
				if (node.NodeId != source->Id || node.NodeType != source->Type) continue;
				for (const auto &output : node.Outputs) {
					if (!spend(std::min(output.Port.size(), link.FromPort.size()))) return nullptr;
					if (output.Port != link.FromPort) continue;
					if (output.Refusal) {
						diagnostic = *output.Refusal;
						return nullptr;
					}
					if (!output.Data) {
						diagnostic = {
							Status::TypeMismatch,
							link.ToNode,
							link.ToPort,
							"tunnel registry selector needs an observed scalar or text value"
						};
						return nullptr;
					}
					return &*output.Data;
				}
			}
		}
		return constructor(*source, link.FromPort, diagnostic);
	}
}
