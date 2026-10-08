#pragma once

#include "NodeExecutors.hpp"

namespace engine::imagegraph::detail {
	inline constexpr std::string_view SourceTunnelRouteName = "$source.tunnel";
	bool ExecuteSourceTunnel(NodeContext &context);
	bool SourceTunnelSenderDomain(NodeContext &, bool callback, SourceSocketDomain &);
	// grug compares declared order, never sorted ids. Caller resolves source getter
	// ownership.
	template <class Selector, class AddRoute>
	bool CompileSourceTunnelRoutes(
		const Document &document, Selector selector, AddRoute addRoute, Diagnostic &diagnostic
	) {
		uint64_t work = 0;
		const auto name = [&](const Node &node, std::string_view &result) {
			const Value *value = nullptr;
			if (!selector(node, "name", value)) return false;
			if (!value) {
				result = {};
				return true;
			}
			const auto *text = std::get_if<std::string>(value);
			if (!text || text->size() > Limits::MaximumTextBytes) {
				diagnostic = {
					Status::InvalidValue, node.Id, "name", "tunnel registry name must be bounded text"
				};
				return false;
			}
			result = *text;
			return true;
		};
		for (size_t consumer = 0; consumer < document.Nodes.size(); ++consumer) {
			const auto &receiver = document.Nodes[consumer];
			if (receiver.Type != "pc.tunnel_out") continue;
			std::string_view key;
			if (!name(receiver, key)) return false;
			if (key.empty()) continue;
			std::optional<size_t> local, global;
			for (size_t producer = 0; producer < document.Nodes.size(); ++producer) {
				const auto &sender = document.Nodes[producer];
				if (++work > 64'000'000) {
					diagnostic = {
						Status::LimitExceeded, receiver.Id, "name", "tunnel registry exceeds bounded work"
					};
					return false;
				}
				if (sender.Type != "pc.tunnel_in") continue;
				std::string_view senderKey;
				if (!name(sender, senderKey)) return false;
				const uint64_t bytes = std::min(key.size(), senderKey.size());
				if (bytes > 64'000'000 - work) {
					diagnostic = {
						Status::LimitExceeded,
						receiver.Id,
						"name",
						"tunnel registry text exceeds bounded work"
					};
					return false;
				}
				work += bytes;
				if (senderKey.empty() || senderKey != key) continue;
				const Value *scopeValue = nullptr;
				if (!selector(sender, "scope", scopeValue)) return false;
				const auto scope = scopeValue ? SourceChoiceNumber(*scopeValue) : std::optional<double>{1};
				if (!scope) {
					diagnostic = {
						Status::InvalidValue,
						sender.Id,
						"scope",
						"tunnel registry scope must be a source choice"
					};
					return false;
				}
				if (*scope == 1) {
					const uint64_t groupBytes = std::min(sender.GroupId.size(), receiver.GroupId.size());
					if (groupBytes > 64'000'000 - work) {
						diagnostic = {
							Status::LimitExceeded,
							receiver.Id,
							"name",
							"tunnel group names exceed bounded work"
						};
						return false;
					}
					work += groupBytes;
					if (sender.GroupId == receiver.GroupId) local = producer;
				} else
					global = producer;
			}
			const auto producer = local ? local : global;
			if (producer && !addRoute(consumer, *producer, SourceTunnelRouteName, "value_in", true))
				return false;
		}
		return true;
	}
} // namespace engine::imagegraph::detail
