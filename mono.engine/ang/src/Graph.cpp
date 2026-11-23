#include <engine/ang/Graph.hpp>
#include <engine/ang/Registry.hpp>

#include <algorithm>

namespace engine::ang {

	namespace {
		template <typename T> auto FindById(std::vector<T> &list, std::string_view id) {
			return std::find_if(list.begin(), list.end(), [id](const T &entry) { return entry.Id == id; });
		}

		template <class... Ts> struct Overloaded : Ts... {
			using Ts::operator()...;
		};

		EditResult Refuse(std::string error) {
			return {.Ok = false, .Error = std::move(error)};
		}

		EditResult Done(const Edit &applied, std::vector<Edit> inverse) {
			return {.Ok = true, .Record = {.Applied = {applied}, .Inverse = std::move(inverse)}};
		}
	}

	const NodeInstance *Graph::FindNode(std::string_view id) const {
		for (const auto &node : NodeList) {
			if (node.Id == id) {
				return &node;
			}
		}
		return nullptr;
	}

	const Group *Graph::FindGroup(std::string_view id) const {
		for (const auto &group : GroupList) {
			if (group.Id == id) {
				return &group;
			}
		}
		return nullptr;
	}

	std::string Graph::NewNodeId(std::string_view stem) const {
		const size_t dot = stem.rfind('.');
		std::string base(dot == std::string_view::npos ? stem : stem.substr(dot + 1));
		if (base.empty()) {
			base = "node";
		}
		for (size_t number = 1;; ++number) {
			std::string candidate = base + std::to_string(number);
			if (FindNode(candidate) == nullptr) {
				return candidate;
			}
		}
	}

	EditResult Graph::Apply(const Edit &change) {
		EditResult result = ApplyOne(change);
		if (result.Ok) {
			++RevisionCount;
		}
		return result;
	}

	EditResult Graph::Apply(std::span<const Edit> changes) {
		// Apply to a copy so a refusal halfway leaves this graph untouched.
		Graph working = *this;
		EditRecord record;
		for (const Edit &change : changes) {
			EditResult step = working.ApplyOne(change);
			if (!step.Ok) {
				return step;
			}
			record.Applied.push_back(change);
			// Undo runs the latest change's inverse first.
			record.Inverse.insert(
				record.Inverse.begin(), step.Record.Inverse.begin(), step.Record.Inverse.end()
			);
		}
		working.RevisionCount = RevisionCount + (changes.empty() ? 0 : 1);
		*this = std::move(working);
		return {.Ok = true, .Record = std::move(record)};
	}

	EditResult Graph::ApplyOne(const Edit &change) {
		return std::visit(
			Overloaded{
				[&](const edit::AddNode &add) -> EditResult {
					if (add.Node.Id.empty()) {
						return Refuse("node id is empty");
					}
					if (FindNode(add.Node.Id) != nullptr) {
						return Refuse("node '" + add.Node.Id + "' already exists");
					}
					NodeList.push_back(add.Node);
					return Done(change, {edit::RemoveNode{add.Node.Id}});
				},
				[&](const edit::RemoveNode &remove) -> EditResult {
					auto found = FindById(NodeList, remove.Node);
					if (found == NodeList.end()) {
						return Refuse("node '" + remove.Node + "' does not exist");
					}
					// Restore the node first, then its links in their old order.
					std::vector<Edit> inverse{edit::AddNode{*found}};
					std::vector<Link> kept;
					for (const Link &wire : LinkList) {
						if (wire.FromNode == remove.Node || wire.ToNode == remove.Node) {
							inverse.push_back(edit::AddLink{wire});
						} else {
							kept.push_back(wire);
						}
					}
					LinkList = std::move(kept);
					NodeList.erase(found);
					return Done(change, std::move(inverse));
				},
				[&](const edit::MoveNode &move) -> EditResult {
					auto found = FindById(NodeList, move.Node);
					if (found == NodeList.end()) {
						return Refuse("node '" + move.Node + "' does not exist");
					}
					const core::Vector2 previous = found->Position;
					found->Position = move.Position;
					return Done(change, {edit::MoveNode{move.Node, previous}});
				},
				[&](const edit::SetParam &set) -> EditResult {
					auto found = FindById(NodeList, set.Node);
					if (found == NodeList.end()) {
						return Refuse("node '" + set.Node + "' does not exist");
					}
					if (set.Name.empty()) {
						return Refuse("param name is empty");
					}
					Value previous;
					if (auto existing = found->Params.find(set.Name); existing != found->Params.end()) {
						previous = existing->second;
					}
					if (set.Param.Empty()) {
						found->Params.erase(set.Name);
					} else {
						found->Params[set.Name] = set.Param;
					}
					return Done(change, {edit::SetParam{set.Node, set.Name, previous}});
				},
				[&](const edit::SetBypass &set) -> EditResult {
					auto found = FindById(NodeList, set.Node);
					if (found == NodeList.end()) {
						return Refuse("node '" + set.Node + "' does not exist");
					}
					const bool previous = found->Bypassed;
					found->Bypassed = set.Bypassed;
					return Done(change, {edit::SetBypass{set.Node, previous}});
				},
				[&](const edit::SetNodeGroup &set) -> EditResult {
					auto found = FindById(NodeList, set.Node);
					if (found == NodeList.end()) {
						return Refuse("node '" + set.Node + "' does not exist");
					}
					if (!set.Group.empty() && FindGroup(set.Group) == nullptr) {
						return Refuse("group '" + set.Group + "' does not exist");
					}
					std::string previous = found->Group;
					found->Group = set.Group;
					return Done(change, {edit::SetNodeGroup{set.Node, std::move(previous)}});
				},
				[&](const edit::AddLink &add) -> EditResult {
					const Link &wire = add.Wire;
					if (FindNode(wire.FromNode) == nullptr || FindNode(wire.ToNode) == nullptr) {
						return Refuse("link names a node that does not exist");
					}
					if (wire.FromPort.empty() || wire.ToPort.empty()) {
						return Refuse("link port is empty");
					}
					if (std::find(LinkList.begin(), LinkList.end(), wire) != LinkList.end()) {
						return Refuse("link already exists");
					}
					LinkList.push_back(wire);
					return Done(change, {edit::RemoveLink{wire}});
				},
				[&](const edit::RemoveLink &remove) -> EditResult {
					auto found = std::find(LinkList.begin(), LinkList.end(), remove.Wire);
					if (found == LinkList.end()) {
						return Refuse("link does not exist");
					}
					LinkList.erase(found);
					return Done(change, {edit::AddLink{remove.Wire}});
				},
				[&](const edit::AddGroup &add) -> EditResult {
					if (add.Frame.Id.empty()) {
						return Refuse("group id is empty");
					}
					if (FindGroup(add.Frame.Id) != nullptr) {
						return Refuse("group '" + add.Frame.Id + "' already exists");
					}
					GroupList.push_back(add.Frame);
					return Done(change, {edit::RemoveGroup{add.Frame.Id}});
				},
				[&](const edit::RemoveGroup &remove) -> EditResult {
					auto found = FindById(GroupList, remove.Group);
					if (found == GroupList.end()) {
						return Refuse("group '" + remove.Group + "' does not exist");
					}
					std::vector<Edit> inverse{edit::AddGroup{*found}};
					for (auto &node : NodeList) {
						if (node.Group == remove.Group) {
							inverse.push_back(edit::SetNodeGroup{node.Id, remove.Group});
							node.Group.clear();
						}
					}
					GroupList.erase(found);
					return Done(change, std::move(inverse));
				},
				[&](const edit::SetGroupFrame &set) -> EditResult {
					auto found = FindById(GroupList, set.Group);
					if (found == GroupList.end()) {
						return Refuse("group '" + set.Group + "' does not exist");
					}
					edit::SetGroupFrame previous{set.Group, found->Position, found->Size};
					found->Position = set.Position;
					found->Size = set.Size;
					return Done(change, {std::move(previous)});
				},
				[&](const edit::SetGroupLabel &set) -> EditResult {
					auto found = FindById(GroupList, set.Group);
					if (found == GroupList.end()) {
						return Refuse("group '" + set.Group + "' does not exist");
					}
					edit::SetGroupLabel previous{set.Group, found->Label};
					found->Label = set.Label;
					return Done(change, {std::move(previous)});
				},
			},
			change
		);
	}

	size_t Graph::Upgrade(const Registry &registry, std::vector<std::string> *refused) {
		size_t changed = 0;
		for (auto &node : NodeList) {
			const auto def = registry.FindNode(node.Def);
			if (def == nullptr) {
				continue;
			}
			if (node.Version == 0) {
				node.Version = def->Version;
				continue;
			}
			if (node.Version >= def->Version) {
				continue;
			}
			// Step on a copy so a refused step leaves the saved node as it was.
			NodeInstance working = node;
			bool ok = static_cast<bool>(def->Migrate);
			while (ok && working.Version < def->Version) {
				ok = def->Migrate(working, working.Version);
				if (ok) {
					++working.Version;
				}
			}
			if (!ok) {
				if (refused != nullptr) {
					refused->push_back(node.Id);
				}
				continue;
			}
			node = std::move(working);
			++changed;
		}
		if (changed != 0) {
			++RevisionCount;
		}
		return changed;
	}
}
