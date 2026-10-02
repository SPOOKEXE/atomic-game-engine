#include "InlineCollections.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace engine::imagegraphio::detail {
	namespace {
		bool SupportedInlineCollection(std::string_view type) {
			return type == "pc.verlet_sim_inline" || type == "pc.flip_group_inline";
		}
	}
	bool ProjectInlineCollections(
		const nlohmann::json &root, imagegraph::Document &document, ImportBudget &budget, std::string &failure
	) {
		using Json = nlohmann::json;
		using namespace imagegraph;
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		using Pair = std::pair<const std::string_view, std::string_view>;
		std::map<std::string_view, std::string_view, std::less<>, ImportAllocator<Pair>> owners(
			std::less<>{}, ImportAllocator<Pair>{budget}
		);
		const auto &sources = root.at("nodes");
		const auto sourceNode = [&](std::string_view id) -> const Json * {
			for (const auto &source : sources)
				if (source.at("id").get_ref<const std::string &>() == id) return &source;
			return nullptr;
		};
		const auto nativeNode = [&](std::string_view id) -> Node * {
			for (auto &node : document.Nodes)
				if (node.Id == id) return &node;
			return nullptr;
		};
		const auto sourceOwner = [&](const Node &owner) -> const Json * {
			const Node *node = &owner;
			for (size_t hop = 0; node && hop <= document.Nodes.size(); ++hop) {
				if (const auto *source = sourceNode(node->Id)) return source;
				if (node->InstanceBase.empty()) break;
				node = nativeNode(node->InstanceBase);
			}
			return nullptr;
		};
		const auto assign = [&](std::string_view child, std::string_view owner) {
			if (child == owner ||
				(!sourceNode(child) && !nativeNode(child) &&
				 std::none_of(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
					 return group.Id == child;
				 })))
				return fail("inline member is absent or names its own owner");
			const auto [found, inserted] = owners.emplace(child, owner);
			if (!inserted && found->second != owner)
				return fail("inline member has conflicting source owners");
			return true;
		};
		try {
			for (const auto &source : sources) {
				if (source.at("type") != "Node_VerletSim_Inline" &&
					source.at("type") != "Node_FLIP_Group_Inline")
					continue;
				const auto &id = source.at("id").get_ref<const std::string &>();
				const auto *node = nativeNode(id);
				if (!node || !SupportedInlineCollection(node->Type)) continue;
				const auto attributes = source.find("attri");
				if (attributes == source.end() || !attributes->contains("members")) continue;
				const auto &members = attributes->at("members");
				if (!members.is_array() || members.size() > Limits::MaximumNodes)
					return fail("inline member list exceeds its source bounds");
				for (const auto &member : members) {
					if (!member.is_string() ||
						member.get_ref<const std::string &>().size() > Limits::MaximumTextBytes)
						return fail("inline member is not a bounded durable ID");
					if (!assign(member.get_ref<const std::string &>(), id)) return false;
				}
			}
			// loadGroup restores ictx after collection.refreshMember, adding members omitted from the list.
			for (const auto &source : sources) {
				const auto context = source.find("ictx");
				if (context == source.end() || *context == "" || *context == -1) continue;
				if (!context->is_string() ||
					context->get_ref<const std::string &>().size() > Limits::MaximumTextBytes)
					return fail("inline context is not a bounded durable ID");
				const auto *owner = nativeNode(context->get_ref<const std::string &>());
				if (owner && SupportedInlineCollection(owner->Type) &&
					!assign(
						source.at("id").get_ref<const std::string &>(),
						context->get_ref<const std::string &>()
					))
					return false;
			}
			// Source group instances rename local child IDs while retaining their InstanceBase relation.
			for (const Node &owner : document.Nodes) {
				if (!SupportedInlineCollection(owner.Type)) continue;
				const auto *source = sourceOwner(owner);
				if (!source) return fail("inline collection clone has no source owner record");
				const auto &originalId = source->at("id").get_ref<const std::string &>();
				if (originalId == owner.Id) continue;
				for (const auto &[child, originalOwner] : owners) {
					if (originalOwner != originalId) continue;
					const Node *copied = nullptr;
					for (const Node &node : document.Nodes)
						if (node.InstanceBase == child && node.GroupId == owner.GroupId) {
							if (copied) return fail("inline collection clone member is ambiguous");
							copied = &node;
						}
					if (copied) {
						if (!assign(copied->Id, owner.Id)) return false;
						continue;
					}
					const Group *nested = nullptr;
					for (const Group &group : document.Groups)
						if (group.InstanceBase == child && group.ParentId == owner.GroupId) {
							if (nested) return fail("inline collection cloned group member is ambiguous");
							nested = &group;
						}
					if (!nested || !assign(nested->Id, owner.Id))
						return fail("inline collection clone member has no renamed target");
				}
			}
			const size_t collectionCount =
				std::count_if(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
					return SupportedInlineCollection(node.Type);
				});
			if (collectionCount > Limits::MaximumGroups - document.Groups.size())
				return fail("inline collection count exceeds native limits");
			if (collectionCount && !budget.Hold((document.Groups.size() + collectionCount) * sizeof(Group)))
				return fail("inline collection group storage exceeds import budget");
			if (collectionCount) document.Groups.reserve(document.Groups.size() + collectionCount);
			for (const Node &ownerNode : document.Nodes) {
				if (!SupportedInlineCollection(ownerNode.Type)) continue;
				const auto *stored = sourceOwner(ownerNode);
				if (!stored) return fail("inline collection has no source record");
				const auto &source = *stored;
				const auto &id = ownerNode.Id;
				const auto *owner = &ownerNode;
				if (document.Groups.size() == Limits::MaximumGroups ||
					id.size() > Limits::MaximumTextBytes - 7)
					return fail("inline collection group exceeds native limits");
				Group group;
				group.Id = id + "/inline";
				if (std::any_of(
						document.Groups.begin(),
						document.Groups.end(),
						[&](const auto &existing) { return existing.Id == group.Id; }
					) ||
					nativeNode(group.Id))
					return fail("inline collection group identity collides with an authored object");
				const auto name = source.find("name");
				if (name != source.end() &&
					(!name->is_string() ||
					 name->get_ref<const std::string &>().size() > Limits::MaximumTextBytes))
					return fail("inline collection name is not bounded text");
				const std::string_view title =
					name == source.end()
						? (ownerNode.Type == "pc.flip_group_inline" ? std::string_view("FLIP Fluid")
																	: std::string_view("VerletSim"))
						: std::string_view(name->get_ref<const std::string &>());
				if (!budget.Hold(title.size() + 16))
					return fail("inline collection name exceeds import budget");
				group.Name = title;
				group.ParentId = owner->GroupId;
				group.OwnerNodeId = id;
				if (group.Name.size() > Limits::MaximumTextBytes ||
					!budget.Hold(
						sizeof(Group) + group.Id.size() + group.Name.size() + group.ParentId.size() +
						group.OwnerNodeId.size() + 64
					))
					return fail("inline collection metadata exceeds import budget");
				document.Groups.push_back(std::move(group));
				document.FormatVersion = std::max<uint32_t>(document.FormatVersion, 9);
			}
			// An ordinary nested group member moves as one group, preserving its child ports and connections.
			for (const auto &[child, owner] : owners) {
				const auto group =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &item) {
						return item.OwnerNodeId == owner;
					});
				if (group == document.Groups.end())
					return fail("inline source owner has no native collection group");
				if (!budget.Hold(group->Id.size() + 16))
					return fail("inline membership exceeds import budget");
				if (auto *node = nativeNode(child))
					node->GroupId = group->Id;
				else {
					const auto nested =
						std::find_if(document.Groups.begin(), document.Groups.end(), [&](const auto &item) {
							return item.Id == child;
						});
					if (nested == document.Groups.end())
						return fail("inline member has no projected node or group");
					nested->ParentId = group->Id;
				}
			}
			// Collection owners may themselves belong to an outer inline collection.
			for (auto &group : document.Groups)
				if (!group.OwnerNodeId.empty()) {
					const auto *owner = nativeNode(group.OwnerNodeId);
					if (owner && group.ParentId != owner->GroupId) {
						if (!budget.Hold(owner->GroupId.size() + 16))
							return fail("inline parent metadata exceeds import budget");
						group.ParentId = owner->GroupId;
					}
				}
			return true;
		} catch (const std::bad_alloc &) {
			return fail("inline collection projection allocation failed");
		}
	}
}
