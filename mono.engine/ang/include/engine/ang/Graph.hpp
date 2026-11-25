#pragma once

// The authored graph document: nodes, links and groups, changed only through edits.
//
// Every edit returns its inverse so a host can keep undo without ANG owning it.
// The graph knows nothing about types; Registry::CheckLink and Compile decide
// whether a link is legal, so a document with unknown node types still loads.
// @tier L3 · shared

#include <engine/ang/Value.hpp>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace engine::ang {

	class Registry;

	// One node in a document.
	struct NodeInstance {
		// Durable within the document.
		std::string Id{};
		// NodeDef or SubgraphDef id.
		std::string Def{};
		// Definition version this node was saved with. Zero means "current".
		uint32_t Version = 0;
		// Sorted by name so the hash does not depend on insertion order.
		std::map<std::string, Value> Params{};
		core::Vector2 Position{};
		std::string Group{};
		bool Bypassed = false;
	};

	// One directed link from an output to an input.
	struct Link {
		std::string FromNode{};
		std::string FromPort{};
		std::string ToNode{};
		std::string ToPort{};

		friend bool operator==(const Link &, const Link &) = default;
	};

	// A labelled frame around nodes, or a free comment when no node names it.
	struct Group {
		std::string Id{};
		std::string Label{};
		core::Vector2 Position{};
		core::Vector2 Size{};
	};

	namespace edit {
		struct AddNode {
			NodeInstance Node{};
		};
		struct RemoveNode {
			std::string Node{};
		};
		struct MoveNode {
			std::string Node{};
			core::Vector2 Position{};
		};
		// An empty Value removes the param, falling back to the definition default.
		struct SetParam {
			std::string Node{};
			std::string Name{};
			Value Param{};
		};
		struct SetBypass {
			std::string Node{};
			bool Bypassed = false;
		};
		struct SetNodeGroup {
			std::string Node{};
			std::string Group{};
		};
		struct AddLink {
			Link Wire{};
		};
		struct RemoveLink {
			Link Wire{};
		};
		struct AddGroup {
			Group Frame{};
		};
		struct RemoveGroup {
			std::string Group{};
		};
		struct SetGroupFrame {
			std::string Group{};
			core::Vector2 Position{};
			core::Vector2 Size{};
		};
		struct SetGroupLabel {
			std::string Group{};
			std::string Label{};
		};
	}

	using Edit = std::variant<
		edit::AddNode,
		edit::RemoveNode,
		edit::MoveNode,
		edit::SetParam,
		edit::SetBypass,
		edit::SetNodeGroup,
		edit::AddLink,
		edit::RemoveLink,
		edit::AddGroup,
		edit::RemoveGroup,
		edit::SetGroupFrame,
		edit::SetGroupLabel>;

	// What an applied edit did and the edits that undo it, in the order to apply them.
	struct EditRecord {
		std::vector<Edit> Applied{};
		std::vector<Edit> Inverse{};
	};

	// Result of Graph::Apply. On refusal the graph is unchanged.
	struct EditResult {
		bool Ok = false;
		std::string Error{};
		EditRecord Record{};
	};

	class Graph {
	  public:
		std::span<const NodeInstance> Nodes() const {
			return NodeList;
		}
		std::span<const Link> Links() const {
			return LinkList;
		}
		std::span<const Group> Groups() const {
			return GroupList;
		}

		const NodeInstance *FindNode(std::string_view id) const;
		const Group *FindGroup(std::string_view id) const;

		// Applies one edit. Refuses unknown nodes, duplicate ids and duplicate links.
		EditResult Apply(const Edit &change);

		// Applies edits in order, all or nothing. The record's inverse undoes the batch.
		EditResult Apply(std::span<const Edit> changes);

		// Replaces the document's nodes and links in one step and drops its groups.
		// Makes no undo record, so it is for loading and generated graphs. Refuses
		// what Apply would, leaving the graph unchanged.
		EditResult Load(std::vector<NodeInstance> nodes, std::vector<Link> links);

		// A node id not yet used, built from `stem` ("math.add" -> "add1", "add2").
		std::string NewNodeId(std::string_view stem) const;

		// Bumped by every applied edit. Hosts compare it to skip recompiles.
		uint64_t Revision() const {
			return RevisionCount;
		}

		// Runs each node's MigrateFn from its saved version up to its definition's.
		// Returns the number of nodes changed; refused or missing steps are listed.
		size_t Upgrade(const Registry &registry, std::vector<std::string> *refused = nullptr);

	  private:
		EditResult ApplyOne(const Edit &change);

		struct TextHash {
			using is_transparent = void;
			size_t operator()(std::string_view text) const {
				return std::hash<std::string_view>{}(text);
			}
		};

		void IndexNodes();

		std::vector<NodeInstance> NodeList{};
		// Position of each node id in NodeList.
		std::unordered_map<std::string, size_t, TextHash, std::equal_to<>> NodeAt{};
		std::vector<Link> LinkList{};
		// Every link as one key, so a duplicate is refused without a scan.
		std::unordered_set<std::string> LinkKeys{};
		std::vector<Group> GroupList{};
		uint64_t RevisionCount = 0;
	};
}
