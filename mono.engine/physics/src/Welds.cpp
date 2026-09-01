#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Welds.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Services.hpp>

#include <algorithm>

namespace engine::physics {

	namespace {
		using core::CFrame;
		using ecs::Entity;
		using ecs::NULL_ENTITY;

		bool ActiveLink(const ecs::Store &store, Entity workspace, Entity owner, Entity part0, Entity part1) {
			return workspace != NULL_ENTITY && part0 != NULL_ENTITY && part1 != NULL_ENTITY && part0 != part1 &&
				store.Get<scene::Transform>(part0) != nullptr && store.Get<scene::Transform>(part1) != nullptr &&
				store.IsDescendantOf(owner, workspace) && store.IsDescendantOf(part0, workspace) &&
				store.IsDescendantOf(part1, workspace);
		}

		RigidNode *NodeOf(std::vector<RigidNode> &nodes, Entity part) {
			const auto found = std::lower_bound(
				nodes.begin(), nodes.end(), part,
				[](const RigidNode &node, Entity wanted) { return node.Part.Id < wanted.Id; }
			);
			return found != nodes.end() && found->Part == part ? &*found : nullptr;
		}
	}

	void SolveRigidJoints(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.welds", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		std::vector<RigidEdge> &edges = PipelineInternals::RigidEdges(*world);
		std::vector<RigidNode> &nodes = PipelineInternals::RigidNodes(*world);
		std::vector<WeldPose> &poses = PipelineInternals::WeldPoses(*world);
		std::vector<WeldPose> &nextPoses = PipelineInternals::WeldPosesNext(*world);
		edges.clear();
		nodes.clear();
		nextPoses.clear();

		const Entity workspace = scene::WorkspaceOf(store);
		store.Each<const scene::JointInstance>([&](Entity owner, const scene::JointInstance &joint) {
			if (!joint.Enabled || !ActiveLink(store, workspace, owner, joint.Part0, joint.Part1)) {
				return;
			}
			edges.push_back(RigidEdge{owner, joint.Part0, joint.Part1, joint.C0 * joint.C1.Inverse()});
		});

		store.Each<const scene::WeldConstraint>([&](Entity owner, const scene::WeldConstraint &joint) {
			if (!ActiveLink(store, workspace, owner, joint.Part0, joint.Part1)) {
				return;
			}

			const auto found = std::lower_bound(
				poses.begin(), poses.end(), owner,
				[](const WeldPose &pose, Entity wanted) { return pose.Owner.Id < wanted.Id; }
			);
			WeldPose pose;
			if (found != poses.end() && found->Owner == owner && found->Part0 == joint.Part0 &&
				found->Part1 == joint.Part1) {
				pose = *found;
			} else {
				const scene::Transform &first = *store.Get<scene::Transform>(joint.Part0);
				const scene::Transform &second = *store.Get<scene::Transform>(joint.Part1);
				pose = WeldPose{owner, joint.Part0, joint.Part1, first.Frame.ToObjectSpace(second.Frame)};
			}
			nextPoses.push_back(pose);
			if (joint.Enabled) {
				edges.push_back(RigidEdge{owner, joint.Part0, joint.Part1, pose.Part0ToPart1});
			}
		});

		std::sort(nextPoses.begin(), nextPoses.end(), [](const WeldPose &a, const WeldPose &b) {
			return a.Owner.Id < b.Owner.Id;
		});
		poses.swap(nextPoses);
		std::sort(edges.begin(), edges.end(), [](const RigidEdge &a, const RigidEdge &b) {
			return a.Owner.Id < b.Owner.Id;
		});

		for (const RigidEdge &edge : edges) {
			nodes.push_back(RigidNode{edge.Part0, NULL_ENTITY, CFrame{}, false});
			nodes.push_back(RigidNode{edge.Part1, NULL_ENTITY, CFrame{}, false});
		}
		std::sort(nodes.begin(), nodes.end(), [](const RigidNode &a, const RigidNode &b) {
			return a.Part.Id < b.Part.Id;
		});
		nodes.erase(
			std::unique(nodes.begin(), nodes.end(), [](const RigidNode &a, const RigidNode &b) {
				return a.Part == b.Part;
			}),
			nodes.end()
		);

		for (RigidNode &start : nodes) {
			if (start.Root != NULL_ENTITY) {
				continue;
			}

			start.Root = start.Part;
			bool expanded = true;
			while (expanded) {
				expanded = false;
				for (const RigidEdge &edge : edges) {
					RigidNode *first = NodeOf(nodes, edge.Part0);
					RigidNode *second = NodeOf(nodes, edge.Part1);
					if (first->Root == start.Part && second->Root == NULL_ENTITY) {
						second->Root = start.Part;
						expanded = true;
					} else if (second->Root == start.Part && first->Root == NULL_ENTITY) {
						first->Root = start.Part;
						expanded = true;
					}
				}
			}

			Entity root = NULL_ENTITY;
			for (const RigidNode &node : nodes) {
				if (node.Root != start.Part) {
					continue;
				}
				const bool anchored = !store.Has<scene::Simulated>(node.Part);
				const bool rootAnchored = root != NULL_ENTITY && !store.Has<scene::Simulated>(root);
				if (root == NULL_ENTITY || (anchored && !rootAnchored) || (anchored == rootAnchored && node.Part.Id < root.Id)) {
					root = node.Part;
				}
			}

			for (RigidNode &node : nodes) {
				if (node.Root == start.Part) {
					node.Root = root;
					node.Placed = false;
				}
			}

			RigidNode *rootNode = NodeOf(nodes, root);
			rootNode->Frame = store.Get<scene::Transform>(root)->Frame;
			rootNode->Placed = true;
			expanded = true;
			while (expanded) {
				expanded = false;
				for (const RigidEdge &edge : edges) {
					RigidNode *first = NodeOf(nodes, edge.Part0);
					RigidNode *second = NodeOf(nodes, edge.Part1);
					if (first->Root != root || second->Root != root) {
						continue;
					}
					if (first->Placed && !second->Placed) {
						second->Frame = first->Frame * edge.Part0ToPart1;
						second->Placed = true;
						expanded = true;
					} else if (second->Placed && !first->Placed) {
						first->Frame = second->Frame * edge.Part0ToPart1.Inverse();
						first->Placed = true;
						expanded = true;
					}
				}
			}

			const scene::Motion *rootMotionRow = store.Get<scene::Motion>(root);
			const bool rootMoving = rootMotionRow != nullptr;
			const scene::Motion rootMotion = rootMotionRow == nullptr ? scene::Motion{} : *rootMotionRow;
			for (RigidNode &node : nodes) {
				if (node.Root != root || !store.Has<scene::Simulated>(node.Part)) {
					continue;
				}
				scene::Transform *transform = store.GetMutable<scene::Transform>(node.Part);
				if (!transform->Frame.FuzzyEq(node.Frame, 1e-6f)) {
					transform->Frame = node.Frame;
					store.MarkChanged<scene::Transform>(node.Part);
					world->Wake(node.Part);
				}

				if (store.Has<scene::Motion>(node.Part) || rootMoving) {
					const core::Vector3 offset = node.Frame.Position - rootNode->Frame.Position;
					store.Set(
						node.Part,
						scene::Motion{
							rootMotion.Linear + rootMotion.Angular.Cross(offset), rootMotion.Angular
						}
					);
				}
			}
		}
	}
}
