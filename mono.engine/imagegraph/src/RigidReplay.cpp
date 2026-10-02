#include "MeshPayload.hpp"
#include "ValuePayload.hpp"

#include <engine/imagegraph/RigidReplay.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>

namespace engine::imagegraph {
	namespace {
		using detail::MeshAddBytes;
		template <class T> uint64_t Slots(const std::vector<T> &items) {
			return detail::MeshVectorBytes<true>(items);
		}
		uint64_t BodyBytes(const SourceRigidBody &body) {
			return MeshAddBytes(body.Id.capacity(), Slots(body.Points));
		}
		uint64_t CommandBytes(const SourceRigidEvent &event) {
			return std::visit(
				[](const auto &command) -> uint64_t {
					using T = std::decay_t<decltype(command)>;
					if constexpr (std::is_same_v<T, SourceRigidBody>)
						return BodyBytes(command);
					else if constexpr (std::is_same_v<T, SourceRigidForce> ||
									   std::is_same_v<T, SourceRigidChange>)
						return command.BodyId.capacity();
					else if constexpr (std::is_same_v<T, SourceRigidJoint>)
						return MeshAddBytes(
							MeshAddBytes(command.Id.capacity(), command.BodyA.capacity()),
							command.BodyB.capacity()
						);
					else if constexpr (std::is_same_v<T, SourceRigidExplosion>) {
						uint64_t bytes = Slots(command.Bodies);
						for (const auto &id : command.Bodies)
							bytes = MeshAddBytes(bytes, id.capacity());
						return bytes;
					} else
						return 0;
				},
				event.Command
			);
		}
		uint64_t VisualBytes(const RigidVisualState &visual) {
			return MeshAddBytes(
				visual.BodyId.capacity(), visual.Texture ? detail::RetainedPayloadBytes(*visual.Texture) : 0
			);
		}
		bool Finite(Vector2 value) {
			return std::isfinite(value.X) && std::isfinite(value.Y);
		}
		bool Name(std::string_view value, size_t bound = Limits::MaximumTextBytes) {
			return !value.empty() && value.size() <= bound;
		}
		bool VisualValid(const RigidVisualState &visual) {
			return Name(visual.BodyId, 256) && std::isfinite(visual.XOffset) &&
				   std::isfinite(visual.YOffset) && std::isfinite(visual.XScale) &&
				   std::isfinite(visual.YScale) && std::isfinite(visual.Alpha) &&
				   (!visual.Texture || detail::ValidPayload(*visual.Texture, true));
		}
		bool CommandValid(const SourceRigidEvent &event) {
			return std::visit(
				[](const auto &command) {
					using T = std::decay_t<decltype(command)>;
					if constexpr (std::is_same_v<T, SourceRigidBody>) {
						if (!Name(command.Id, 256) || command.Shape > SourceRigidShape::Segment ||
							!Finite(command.Position) || !Finite(command.Size) ||
							!Finite(command.InitialVelocity) || command.Points.size() > 8 ||
							(command.Density && !std::isfinite(*command.Density)))
							return false;
						for (const auto &point : command.Points)
							if (!Finite(point)) return false;
						for (double value :
							 {command.RotationDegrees,
							  command.Friction,
							  command.Restitution,
							  command.LinearDamping,
							  command.AngularDamping,
							  command.GravityScale,
							  command.AuthoredMass})
							if (!std::isfinite(value)) return false;
						return true;
					} else if constexpr (std::is_same_v<T, SourceRigidForce>) {
						return Name(command.BodyId, 256) &&
							   command.Kind <= SourceRigidForceKind::AngularImpulse &&
							   Finite(command.Force) && Finite(command.Point) &&
							   std::isfinite(command.Torque);
					} else if constexpr (std::is_same_v<T, SourceRigidStep>) {
						return std::isfinite(command.TimeStepMilliseconds) &&
							   command.TimeStepMilliseconds >= 0 && command.TimeStepMilliseconds <= 1000 &&
							   command.Quality > 0 && command.Quality <= 64;
					} else if constexpr (std::is_same_v<T, SourceRigidChange>) {
						if (!Name(command.BodyId, 256) ||
							(command.PositionWorld && !Finite(*command.PositionWorld)) ||
							(command.LinearVelocity && !Finite(*command.LinearVelocity)))
							return false;
						for (const auto &value :
							 {command.RotationRadians,
							  command.Mass,
							  command.Friction,
							  command.Restitution,
							  command.GravityScale})
							if (value && (!std::isfinite(value->Value) ||
										  value->Mode > SourceRigidChangeMode::Multiply))
								return false;
						return true;
					} else if constexpr (std::is_same_v<T, SourceRigidJoint>) {
						if (!Name(command.Id, 256) || !Name(command.BodyA, 256) ||
							!Name(command.BodyB, 256) || command.Kind > SourceRigidJointKind::Motor ||
							(command.Anchor && !Finite(*command.Anchor)) || !Finite(command.Offset))
							return false;
						for (double value :
							 {command.Stiffness,
							  command.Damping,
							  command.MaximumForce,
							  command.MaximumTorque,
							  command.BreakForce})
							if (!std::isfinite(value)) return false;
						return true;
					} else if constexpr (std::is_same_v<T, SourceRigidExplosion>) {
						return command.Bodies.size() <= 256 &&
							   std::all_of(
								   command.Bodies.begin(),
								   command.Bodies.end(),
								   [](const auto &id) { return Name(id, 256); }
							   ) &&
							   Finite(command.Position) && std::isfinite(command.Radius) &&
							   std::isfinite(command.Strength) && std::isfinite(command.Torque);
					} else
						return true;
				},
				event.Command
			);
		}

	}
	uint64_t RetainedRigidReplayBytes(const RigidReplayState &state) {
		uint64_t bytes = MeshAddBytes(sizeof(state), Slots(state.Owners));
		for (const auto &owner : state.Owners) {
			bytes = MeshAddBytes(bytes, owner.History.OwnerId.capacity());
			bytes = MeshAddBytes(bytes, Slots(owner.History.Frames));
			for (const auto &frame : owner.History.Frames) {
				bytes = MeshAddBytes(bytes, Slots(frame.Events));
				for (const auto &event : frame.Events) {
					bytes = MeshAddBytes(bytes, event.Position.ConsumerId.capacity());
					bytes = MeshAddBytes(bytes, CommandBytes(event));
				}
			}
			bytes = MeshAddBytes(bytes, Slots(owner.VisualFrames));
			for (const auto &frame : owner.VisualFrames) {
				bytes = MeshAddBytes(bytes, Slots(frame.Mutations));
				for (const auto &mutation : frame.Mutations) {
					bytes = MeshAddBytes(bytes, mutation.Position.ConsumerId.capacity());
					bytes = MeshAddBytes(bytes, VisualBytes(mutation.Data));
				}
			}
			bytes = MeshAddBytes(bytes, Slots(owner.NodeFrames));
			for (const auto &nodeFrame : owner.NodeFrames) {
				bytes = MeshAddBytes(bytes, Slots(nodeFrame.Nodes));
				for (const auto &node : nodeFrame.Nodes) {
					bytes = MeshAddBytes(bytes, node.NodeId.capacity());
					bytes = MeshAddBytes(bytes, Slots(node.OutputBodyIds));
					for (const auto &id : node.OutputBodyIds)
						bytes = MeshAddBytes(bytes, id.capacity());
					bytes = MeshAddBytes(bytes, Slots(node.CollisionPairs));
					for (const auto &pair : node.CollisionPairs)
						bytes = MeshAddBytes(bytes, MeshAddBytes(pair.A.capacity(), pair.B.capacity()));
				}
				bytes = MeshAddBytes(bytes, Slots(nodeFrame.Recipes));
				for (const auto &recipe : nodeFrame.Recipes) {
					bytes = MeshAddBytes(bytes, recipe.NodeId.capacity());
					bytes = MeshAddBytes(bytes, Slots(recipe.Prototypes));
					for (const auto &body : recipe.Prototypes)
						bytes = MeshAddBytes(bytes, BodyBytes(body));
					bytes = MeshAddBytes(bytes, Slots(recipe.Visuals));
					for (const auto &visual : recipe.Visuals)
						bytes = MeshAddBytes(bytes, VisualBytes(visual));
				}
			}
		}
		return bytes;
	}
	Status ValidateRigidReplay(const RigidReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic) {
		diagnostic = {};
		const auto refuse = [&](Status status, std::string message, std::string_view node = {}) {
			diagnostic = {status, std::string(node), {}, std::move(message)};
			return status;
		};
		if (state.Owners.size() > Limits::MaximumGroups)
			return refuse(Status::LimitExceeded, "rigid replay owner count exceeds the graph bound");
		size_t events = 0, nodes = 0, visuals = 0;
		for (const auto &owner : state.Owners) {
			if (owner.History.Frames.size() > 4096 ||
				owner.VisualFrames.size() > owner.History.Frames.size() ||
				owner.NodeFrames.size() > owner.History.Frames.size())
				return refuse(
					Status::LimitExceeded,
					"rigid replay history or node count exceeds its bound",
					owner.History.OwnerId
				);
			for (const auto &nodeFrame : owner.NodeFrames) {
				if (nodeFrame.Nodes.size() > 65536 - nodes || nodeFrame.Recipes.size() > Limits::MaximumNodes)
					return refuse(
						Status::LimitExceeded,
						"rigid replay node frames exceed their count bound",
						owner.History.OwnerId
					);
				nodes += nodeFrame.Nodes.size();
			}
			for (const auto &frame : owner.VisualFrames) {
				if (frame.Mutations.size() > 65536 - visuals)
					return refuse(
						Status::LimitExceeded,
						"rigid visual history exceeds its record bound",
						owner.History.OwnerId
					);
				visuals += frame.Mutations.size();
			}
			for (const auto &frame : owner.History.Frames) {
				if (frame.Events.size() > 65536 - events)
					return refuse(
						Status::LimitExceeded,
						"rigid replay command count exceeds its bound",
						owner.History.OwnerId
					);
				events += frame.Events.size();
			}
		}
		const uint64_t retained = RetainedRigidReplayBytes(state);
		if (retained > maximumBytes || retained > Limits::MaximumEvaluationBytes ||
			events > (maximumBytes - retained) / sizeof(size_t))
			return refuse(
				Status::LimitExceeded, "rigid replay or validation workspace exceeds its byte bound"
			);
		for (size_t oi = 0; oi < state.Owners.size(); ++oi) {
			const auto &owner = state.Owners[oi];
			if (!Name(owner.History.OwnerId) || !Finite(owner.History.World.Dimension) ||
				!std::isfinite(owner.History.World.Scale))
				return refuse(
					Status::InvalidValue, "rigid replay owner controls are invalid", owner.History.OwnerId
				);
			for (size_t before = 0; before < oi; ++before)
				if (state.Owners[before].History.OwnerId == owner.History.OwnerId)
					return refuse(
						Status::DuplicateId, "rigid replay repeats an owner identity", owner.History.OwnerId
					);
			for (size_t fi = 0; fi < owner.History.Frames.size(); ++fi) {
				const auto &frame = owner.History.Frames[fi];
				if (!Finite(frame.Gravity) ||
					(frame.SimulationScale && !std::isfinite(*frame.SimulationScale)) ||
					(frame.CanvasDimension && !Finite(*frame.CanvasDimension)))
					return refuse(
						Status::InvalidValue, "rigid replay frame controls are invalid", owner.History.OwnerId
					);
				std::vector<size_t> order(frame.Events.size());
				std::iota(order.begin(), order.end(), 0);
				const auto key = [&](size_t index) {
					const auto &p = frame.Events[index].Position;
					return std::tie(p.ConsumerId, p.ProcessorRow, p.Ordinal);
				};
				std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
				for (size_t i = 0; i < order.size(); ++i) {
					const auto &event = frame.Events[order[i]];
					if (!Name(event.Position.ConsumerId) ||
						event.Position.ProcessorRow >= Limits::MaximumArrayElements ||
						event.Position.Ordinal >= 65536 || !CommandValid(event))
						return refuse(
							Status::InvalidValue,
							"rigid replay event identity is invalid",
							event.Position.ConsumerId
						);
					if (i && key(order[i - 1]) == key(order[i]))
						return refuse(
							Status::DuplicateId,
							"rigid replay repeats an event identity",
							event.Position.ConsumerId
						);
				}
				if (fi < owner.VisualFrames.size())
					for (const auto &mutation : owner.VisualFrames[fi].Mutations) {
						const auto wanted = std::tie(
							mutation.Position.ConsumerId,
							mutation.Position.ProcessorRow,
							mutation.Position.Ordinal
						);
						const auto match = std::lower_bound(
							order.begin(), order.end(), wanted, [&](size_t index, const auto &position) {
								return key(index) < position;
							}
						);
						if (!VisualValid(mutation.Data) || match == order.end() || key(*match) != wanted)
							return refuse(
								Status::InvalidValue,
								"rigid visual mutation has invalid data or no matching event",
								mutation.Position.ConsumerId
							);
					}
			}
			for (const auto &nodeFrame : owner.NodeFrames) {
				for (size_t ni = 0; ni < nodeFrame.Nodes.size(); ++ni) {
					const auto &node = nodeFrame.Nodes[ni];
					if (!Name(node.NodeId) || node.ProcessorRow >= Limits::MaximumArrayElements ||
						node.OutputBodyIds.size() > 256 || node.CollisionPairs.size() > 65536)
						return refuse(Status::InvalidValue, "rigid node history is invalid", node.NodeId);
					for (size_t before = 0; before < ni; ++before)
						if (nodeFrame.Nodes[before].NodeId == node.NodeId &&
							nodeFrame.Nodes[before].ProcessorRow == node.ProcessorRow)
							return refuse(
								Status::DuplicateId, "rigid replay repeats a node row", node.NodeId
							);
					for (const auto &id : node.OutputBodyIds)
						if (!Name(id, 256))
							return refuse(
								Status::InvalidValue, "rigid output body identity is invalid", node.NodeId
							);
					for (const auto &pair : node.CollisionPairs)
						if (!Name(pair.A, 256) || !Name(pair.B, 256) ||
							pair.LastObservedTick > Limits::MaximumTick)
							return refuse(
								Status::InvalidValue, "rigid collision history is invalid", node.NodeId
							);
				}
				for (const auto &recipe : nodeFrame.Recipes) {
					if (!Name(recipe.NodeId) || recipe.ProcessorRow >= Limits::MaximumArrayElements ||
						recipe.Tick > Limits::MaximumTick || recipe.Prototypes.size() > 256 ||
						recipe.Prototypes.size() != recipe.Visuals.size())
						return refuse(Status::InvalidValue, "rigid spawn recipe is invalid", recipe.NodeId);
					for (const auto &visual : recipe.Visuals)
						if (!VisualValid(visual))
							return refuse(
								Status::InvalidValue, "rigid spawn visual recipe is invalid", recipe.NodeId
							);
				}
			}
		}
		return Status::Ok;
	}
}
