#include "PipelineInternals.hpp"
#include "WorkBudgetInternal.hpp"
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

		bool DescendantForJoint(ecs::Store &store, Entity owner, Entity ancestor) {
			for (Entity parent = owner; parent != NULL_ENTITY; parent = store.ParentOf(parent)) {
				ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
				if (parent == ancestor) return true;
			}
			return false;
		}

		bool ActiveLink(ecs::Store &store, Entity workspace, Entity owner, Entity part0, Entity part1) {
			return workspace != NULL_ENTITY && part0 != NULL_ENTITY && part1 != NULL_ENTITY &&
				   part0 != part1 && store.Get<scene::Transform>(part0) != nullptr &&
				   store.Get<scene::Transform>(part1) != nullptr &&
				   DescendantForJoint(store, owner, workspace) &&
				   DescendantForJoint(store, part0, workspace) && DescendantForJoint(store, part1, workspace);
		}

		RigidNode *NodeOf(std::vector<RigidNode> &nodes, Entity part) {
			const auto found =
				std::lower_bound(nodes.begin(), nodes.end(), part, [](const RigidNode &node, Entity wanted) {
					return node.Part.Id < wanted.Id;
				});
			return found != nodes.end() && found->Part == part ? &*found : nullptr;
		}
	}

	void SolveRigidJoints(ecs::Store &store) {
		ChargePhysicsWork(store, PhysicsWorkStage::Welds, 0);
		ENGINE_PROFILE_CAT("physics.welds", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		if (const auto *budget = PhysicsWorkBudgetOf(store))
			AdmitPhysicsCount(
				store,
				PhysicsWorkStage::Welds,
				store.Query<const scene::JointInstance>().Count() +
					store.Query<const scene::WeldConstraint>().Count(),
				budget->Limits.Joints
			);

		std::vector<RigidEdge> &edges = PipelineInternals::RigidEdges(*world);
		std::vector<RigidNode> &nodes = PipelineInternals::RigidNodes(*world);
		std::vector<WeldPose> &poses = PipelineInternals::WeldPoses(*world);
		std::vector<WeldPose> &nextPoses = PipelineInternals::WeldPosesNext(*world);
		edges.clear();
		nodes.clear();
		nextPoses.clear();

		const Entity workspace = scene::WorkspaceOf(store);
		store.Each<const scene::JointInstance>([&](Entity owner, const scene::JointInstance &joint) {
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			if (!joint.Enabled || !ActiveLink(store, workspace, owner, joint.Part0, joint.Part1)) {
				return;
			}
			edges.push_back(RigidEdge{owner, joint.Part0, joint.Part1, joint.C0 * joint.C1.Inverse()});
		});

		store.Each<const scene::WeldConstraint>([&](Entity owner, const scene::WeldConstraint &joint) {
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			if (!ActiveLink(store, workspace, owner, joint.Part0, joint.Part1)) {
				return;
			}

			const auto found =
				std::lower_bound(poses.begin(), poses.end(), owner, [](const WeldPose &pose, Entity wanted) {
					return pose.Owner.Id < wanted.Id;
				});
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
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			nodes.push_back(RigidNode{edge.Part0, NULL_ENTITY, CFrame{}, false});
			nodes.push_back(RigidNode{edge.Part1, NULL_ENTITY, CFrame{}, false});
		}
		std::sort(nodes.begin(), nodes.end(), [](const RigidNode &a, const RigidNode &b) {
			return a.Part.Id < b.Part.Id;
		});
		nodes.erase(
			std::unique(
				nodes.begin(),
				nodes.end(),
				[](const RigidNode &a, const RigidNode &b) { return a.Part == b.Part; }
			),
			nodes.end()
		);

		// Disconnected assemblies must not rescan one another. Scratch is bounded
		// by the admitted joint rows and never changes the public world layout.
		ChargePhysicsWork(store, PhysicsWorkStage::Welds, nodes.size());
		ChargePhysicsWork(store, PhysicsWorkStage::Welds, nodes.size());
		ChargePhysicsWork(store, PhysicsWorkStage::Welds, nodes.size());
		std::vector<size_t> parents(nodes.size());
		std::vector<std::vector<size_t>> componentNodes(nodes.size());
		std::vector<std::vector<size_t>> componentEdges(nodes.size());
		for (size_t node = 0; node < nodes.size(); ++node)
			parents[node] = node;
		const auto findComponent = [&](size_t node) {
			size_t root = node;
			while (parents[root] != root) {
				ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
				root = parents[root];
			}
			while (parents[node] != node) {
				ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
				const size_t parent = parents[node];
				parents[node] = root;
				node = parent;
			}
			return root;
		};
		const auto nodeIndex = [&](Entity part) {
			return static_cast<size_t>(NodeOf(nodes, part) - nodes.data());
		};
		for (const RigidEdge &edge : edges) {
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			const size_t first = findComponent(nodeIndex(edge.Part0));
			const size_t second = findComponent(nodeIndex(edge.Part1));
			if (first != second) parents[std::max(first, second)] = std::min(first, second);
		}
		for (size_t node = 0; node < nodes.size(); ++node) {
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			componentNodes[findComponent(node)].push_back(node);
		}
		for (size_t edge = 0; edge < edges.size(); ++edge) {
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			componentEdges[findComponent(nodeIndex(edges[edge].Part0))].push_back(edge);
		}

		// Minimum node index orders components as the original sorted-node walk.
		// Appending sorted node and edge indices retains their order inside each
		// assembly, including first-reach pose selection in inconsistent cycles.
		for (size_t component = 0; component < componentNodes.size(); ++component) {
			ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
			const auto &members = componentNodes[component];
			if (members.empty()) continue;
			const auto &links = componentEdges[component];
			Entity root = NULL_ENTITY;
			for (size_t member : members) {
				ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
				const RigidNode &node = nodes[member];
				const bool anchored = !store.Has<scene::Simulated>(node.Part);
				const bool rootAnchored = root != NULL_ENTITY && !store.Has<scene::Simulated>(root);
				if (root == NULL_ENTITY || (anchored && !rootAnchored) ||
					(anchored == rootAnchored && node.Part.Id < root.Id)) {
					root = node.Part;
				}
			}

			for (size_t member : members) {
				ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
				RigidNode &node = nodes[member];
				node.Root = root;
				node.Placed = false;
			}

			RigidNode *rootNode = NodeOf(nodes, root);
			rootNode->Frame = store.Get<scene::Transform>(root)->Frame;
			rootNode->Placed = true;
			bool expanded = true;
			while (expanded) {
				expanded = false;
				for (size_t link : links) {
					ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
					const RigidEdge &edge = edges[link];
					RigidNode *first = NodeOf(nodes, edge.Part0);
					RigidNode *second = NodeOf(nodes, edge.Part1);
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
			for (size_t member : members) {
				ChargePhysicsWork(store, PhysicsWorkStage::Welds, 1);
				RigidNode &node = nodes[member];
				if (!store.Has<scene::Simulated>(node.Part)) continue;
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
