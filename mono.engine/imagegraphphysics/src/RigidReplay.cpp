#include <engine/core/Profiling.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>

#include <algorithm>
#include <box2d/box2d.h>
#include <cmath>
#include <mutex>
#include <numbers>
#include <set>
#include <tuple>

namespace engine::imagegraphphysics {
	namespace {
		using namespace imagegraph;
		constexpr size_t MAX_FRAMES = 4096, MAX_BODIES = 256, MAX_COMMANDS = 65536;
		constexpr double MAX_COORDINATE = 1'000'000;
		bool Number(double value, double maximum = MAX_COORDINATE) {
			return std::isfinite(value) && std::abs(value) <= maximum;
		}
		bool Vector(Vector2 value) {
			return Number(value.X) && Number(value.Y);
		}
		Status Fail(Diagnostic &diagnostic, Status status, std::string message) {
			diagnostic = {};
			diagnostic.Code = status;
			diagnostic.Message = std::move(message);
			return status;
		}
		b2Vec2 Vec(Vector2 value, double divisor = 1) {
			return {static_cast<float>(value.X / divisor), static_cast<float>(value.Y / divisor)};
		}
		bool Change(const std::optional<SourceRigidScalarChange> &change) {
			return !change || (Number(change->Value) && change->Mode <= SourceRigidChangeMode::Multiply);
		}
		double Changed(double original, const SourceRigidScalarChange &change) {
			if (change.Mode == SourceRigidChangeMode::Add) return original + change.Value;
			if (change.Mode == SourceRigidChangeMode::Multiply) return original * change.Value;
			return change.Value;
		}
		struct WorldOwner {
			b2WorldId Id;
			explicit WorldOwner(b2WorldId id) : Id(id) {}
			WorldOwner(const WorldOwner &) = delete;
			WorldOwner &operator=(const WorldOwner &) = delete;
			~WorldOwner() {
				b2DestroyWorld(Id);
			}
		};
		struct Body {
			std::string_view Name;
			b2BodyId Id;
			b2ShapeId Shape;
			bool Sensor = false;
		};
	}

	imagegraph::Status ReplayRigid(
		const imagegraph::SourceRigidHistory &history,
		uint64_t tick,
		imagegraph::SourceRigidSnapshot &output,
		imagegraph::Diagnostic &diagnostic,
		std::optional<imagegraph::SourceRigidEventPosition> captureAt,
		uint64_t maximumSnapshotBytes
	) {
		ENGINE_PROFILE("imagegraph rigid replay");
		using namespace imagegraph;
		const auto &world = history.World;
		if (maximumSnapshotBytes < sizeof(SourceRigidSnapshot))
			return Fail(diagnostic, Status::LimitExceeded, "Rigid snapshot byte budget exceeded");
		if (history.Frames.size() > MAX_FRAMES || tick >= MAX_FRAMES)
			return Fail(diagnostic, Status::LimitExceeded, "Rigid replay exceeds 4096 recorded frames");
		if (tick >= history.Frames.size())
			return Fail(
				diagnostic, Status::InvalidValue, "Rigid replay requires every requested source frame"
			);
		if (!Vector(world.Dimension) || world.Dimension.X <= 0 || world.Dimension.Y <= 0 ||
			!Number(world.Scale) || world.Scale < .001 || world.Walls > 15 || !Number(world.WallFriction) ||
			world.WallFriction < 0 || !Number(world.WallRestitution, 1) || world.WallRestitution < 0)
			return Fail(diagnostic, Status::InvalidValue, "Invalid bounded rigid world controls");
		// Box2D 3.1.0 rejects segment lengths at or below its 0.005 metre linear slop.
		const double initialScale = history.Frames.front().SimulationScale.value_or(world.Scale);
		const Vector2 initialCanvas = history.Frames.front().CanvasDimension.value_or(world.Dimension);
		const double sourceWidth = initialCanvas.X / initialScale;
		const double sourceHeight = initialCanvas.Y / initialScale;
		const float wallWidth = static_cast<float>(sourceWidth * 2) - static_cast<float>(-sourceWidth);
		const float wallHeight = static_cast<float>(sourceHeight * 2) - static_cast<float>(-sourceHeight);
		if (((world.Walls & 3) && wallWidth * wallWidth <= .005f * .005f) ||
			((world.Walls & 12) && wallHeight * wallHeight <= .005f * .005f))
			return Fail(
				diagnostic, Status::InvalidValue, "Rigid boundary segment is below the native minimum length"
			);
		if (history.OwnerId.empty() || history.OwnerId.size() > 256)
			return Fail(
				diagnostic, Status::InvalidValue, "Rigid history requires a bounded stable owner name"
			);
		std::set<std::string_view> names, jointNames;
		size_t commands = 0;
		std::optional<size_t> stop;
		// Validate the full recording before vendor allocation, including future seek frames.
		for (size_t frameIndex = 0; frameIndex < history.Frames.size(); ++frameIndex) {
			const auto &frame = history.Frames[frameIndex];
			if (!Vector(frame.Gravity) ||
				(frame.CanvasDimension && (!Vector(*frame.CanvasDimension) || frame.CanvasDimension->X <= 0 ||
										   frame.CanvasDimension->Y <= 0)) ||
				(frame.SimulationScale && (!Number(*frame.SimulationScale) || *frame.SimulationScale < .001)))
				return Fail(diagnostic, Status::InvalidValue, "Invalid bounded rigid world controls");
			if (frame.Events.size() > MAX_COMMANDS - commands)
				return Fail(diagnostic, Status::LimitExceeded, "Rigid replay command budget exceeded");
			commands += frame.Events.size();
			std::set<std::tuple<std::string_view, uint32_t, uint32_t>> identities;
			for (size_t eventIndex = 0; eventIndex < frame.Events.size(); ++eventIndex) {
				const auto &event = frame.Events[eventIndex];
				if (event.Position.ConsumerId.empty() || event.Position.ConsumerId.size() > 256 ||
					!identities
						 .emplace(
							 event.Position.ConsumerId, event.Position.ProcessorRow, event.Position.Ordinal
						 )
						 .second)
					return Fail(
						diagnostic,
						Status::DuplicateId,
						"Rigid event requires a unique consumer/ordinal identity"
					);
				if (frameIndex == tick && captureAt && event.Position == *captureAt) stop = eventIndex;
				if (const auto *body = std::get_if<SourceRigidBody>(&event.Command)) {
					if (names.size() == MAX_BODIES)
						return Fail(diagnostic, Status::LimitExceeded, "Rigid replay body budget exceeded");
					if (body->Id.empty() || body->Id.size() > 256 || body->Id.starts_with("__wall/") ||
						!names.emplace(body->Id).second)
						return Fail(
							diagnostic, Status::DuplicateId, "Rigid body requires a unique stable name"
						);
					if (body->Shape > SourceRigidShape::Segment || !Vector(body->Position) ||
						!Vector(body->Size) || body->Size.X <= 0 || body->Size.Y <= 0 ||
						static_cast<float>(
							body->Size.X / (2 * frame.SimulationScale.value_or(world.Scale))
						) <= 0 ||
						static_cast<float>(
							body->Size.Y / (2 * frame.SimulationScale.value_or(world.Scale))
						) <= 0 ||
						!Vector(body->InitialVelocity) || !Number(body->RotationDegrees) ||
						!Number(body->Friction) || body->Friction < 0 || !Number(body->Restitution, 1) ||
						body->Restitution < 0 || !Number(body->LinearDamping) || body->LinearDamping < 0 ||
						!Number(body->AngularDamping) || body->AngularDamping < 0 ||
						!Number(body->GravityScale) || !Number(body->AuthoredMass) ||
						(body->Density && (!Number(*body->Density) || *body->Density < 0)))
						return Fail(diagnostic, Status::InvalidValue, "Invalid bounded rigid spawn controls");
					if (body->Shape == SourceRigidShape::Polygon ||
						body->Shape == SourceRigidShape::Segment) {
						const bool segment = body->Shape == SourceRigidShape::Segment;
						if (body->Points.size() < (segment ? 2u : 3u) ||
							body->Points.size() > (segment ? 2u : 8u))
							return Fail(
								diagnostic,
								Status::InvalidValue,
								"Rigid fixture point count is outside the native profile"
							);
						b2Vec2 points[8]{};
						for (size_t i = 0; i < body->Points.size(); ++i) {
							if (!Vector(body->Points[i]))
								return Fail(diagnostic, Status::InvalidValue, "Invalid rigid fixture point");
							points[i] = Vec(body->Points[i], frame.SimulationScale.value_or(world.Scale));
						}
						if (segment) {
							const float dx = points[1].x - points[0].x, dy = points[1].y - points[0].y;
							if (dx * dx + dy * dy <= .005f * .005f)
								return Fail(
									diagnostic,
									Status::InvalidValue,
									"Rigid segment is below native minimum length"
								);
						} else {
							const auto hull = b2ComputeHull(points, static_cast<int>(body->Points.size()));
							if (hull.count < 3 || !b2ValidateHull(&hull))
								return Fail(
									diagnostic,
									Status::InvalidValue,
									"Rigid polygon has no valid bounded convex hull"
								);
						}
					}
				} else if (const auto *force = std::get_if<SourceRigidForce>(&event.Command)) {
					if (!names.contains(force->BodyId) || !Vector(force->Force) || !Vector(force->Point) ||
						!Number(force->Torque) || force->Kind > SourceRigidForceKind::AngularImpulse)
						return Fail(
							diagnostic,
							Status::InvalidValue,
							"Rigid force requires an existing body and finite controls"
						);
				} else if (const auto *explosion = std::get_if<SourceRigidExplosion>(&event.Command)) {
					if (explosion->Bodies.size() > MAX_BODIES || !Vector(explosion->Position) ||
						!Number(explosion->Radius) || explosion->Radius < 0 || !Number(explosion->Strength) ||
						!Number(explosion->Torque))
						return Fail(
							diagnostic, Status::InvalidValue, "Invalid bounded rigid explosion controls"
						);
					for (const auto &id : explosion->Bodies)
						if (!names.contains(id))
							return Fail(
								diagnostic,
								Status::InvalidValue,
								"Rigid explosion requires existing named bodies"
							);
				} else if (const auto *joint = std::get_if<SourceRigidJoint>(&event.Command)) {
					if (jointNames.size() >= 4096)
						return Fail(diagnostic, Status::LimitExceeded, "Rigid joint budget exceeded");
					if (joint->Id.empty() || joint->Id.size() > 256 || !jointNames.insert(joint->Id).second ||
						joint->BodyA == joint->BodyB || !names.contains(joint->BodyA) ||
						!names.contains(joint->BodyB) || joint->Kind > SourceRigidJointKind::Motor ||
						(joint->Anchor && !Vector(*joint->Anchor)) || !Vector(joint->Offset) ||
						!Number(joint->Stiffness) || joint->Stiffness < 0 || !Number(joint->Damping) ||
						joint->Damping < 0 || !Number(joint->MaximumForce) || joint->MaximumForce < 0 ||
						!Number(joint->MaximumTorque) || joint->MaximumTorque < 0 ||
						!Number(joint->BreakForce) || joint->BreakForce < 0)
						return Fail(diagnostic, Status::InvalidValue, "Invalid bounded rigid joint controls");
				} else if (const auto *change = std::get_if<SourceRigidChange>(&event.Command)) {
					if (!names.contains(change->BodyId) ||
						(change->PositionWorld && !Vector(*change->PositionWorld)) ||
						(change->LinearVelocity && !Vector(*change->LinearVelocity)) ||
						!Change(change->RotationRadians) || !Change(change->Mass) ||
						!Change(change->Friction) || !Change(change->Restitution) ||
						!Change(change->GravityScale))
						return Fail(diagnostic, Status::InvalidValue, "Invalid rigid body change controls");
				} else if (const auto *step = std::get_if<SourceRigidStep>(&event.Command)) {
					if (!Number(step->TimeStepMilliseconds, 1000) || step->TimeStepMilliseconds < 0 ||
						step->Quality == 0 || step->Quality > 64)
						return Fail(diagnostic, Status::InvalidValue, "Invalid bounded rigid step controls");
				} else if (!std::holds_alternative<SourceRigidEvent::Checkpoint>(event.Command)) {
					return Fail(diagnostic, Status::InvalidValue, "Invalid rigid event command");
				}
			}
		}
		if (captureAt && !stop)
			return Fail(
				diagnostic, Status::InvalidOutput, "Rigid capture event is absent at the requested tick"
			);
		// Box2D world creation/destruction share process-global slots.
		static std::mutex worldMutex;
		const std::lock_guard lock(worldMutex);
		auto definition = b2DefaultWorldDef();
		definition.workerCount = 0;
		WorldOwner owner{b2CreateWorld(&definition)};
		std::vector<Body> bodies;
		struct Joint {
			b2JointId Id;
			float BreakForce;
			bool Broken = false;
		};
		std::vector<Joint> joints;
		joints.reserve(jointNames.size());
		bodies.reserve(names.size() + 4);
		const double width = sourceWidth, height = sourceHeight;
		const b2Segment walls[] = {
			{{static_cast<float>(-width), 0}, {static_cast<float>(width * 2), 0}},
			{{static_cast<float>(-width), static_cast<float>(height)},
			 {static_cast<float>(width * 2), static_cast<float>(height)}},
			{{0, static_cast<float>(-height)}, {0, static_cast<float>(height * 2)}},
			{{static_cast<float>(width), static_cast<float>(-height)},
			 {static_cast<float>(width), static_cast<float>(height * 2)}}
		};
		constexpr std::string_view wallNames[] = {
			"__wall/top", "__wall/bottom", "__wall/left", "__wall/right"
		};
		for (size_t side = 0; side < 4; ++side) {
			if (!(world.Walls & (1 << side))) continue;
			auto bodyDef = b2DefaultBodyDef();
			bodyDef.type = b2_dynamicBody;
			const auto body = b2CreateBody(owner.Id, &bodyDef);
			auto shapeDef = b2DefaultShapeDef();
			shapeDef.enableSensorEvents = true;
			const auto shape = b2CreateSegmentShape(body, &shapeDef, &walls[side]);
			b2Body_SetType(body, b2_staticBody);
			b2Shape_SetFriction(shape, static_cast<float>(world.WallFriction));
			b2Shape_SetRestitution(shape, static_cast<float>(world.WallRestitution));
			bodies.push_back({wallNames[side], body, shape});
		}
		for (size_t frameIndex = 0; frameIndex <= tick; ++frameIndex) {
			const auto &frame = history.Frames[frameIndex];
			const double scale = frame.SimulationScale.value_or(world.Scale);
			b2World_SetGravity(owner.Id, Vec(frame.Gravity));
			b2World_EnableSleeping(owner.Id, frame.Sleepable);
			b2World_EnableContinuous(owner.Id, frame.Continuous);
			for (size_t eventIndex = 0; eventIndex < frame.Events.size(); ++eventIndex) {
				if (frameIndex == tick && stop && eventIndex > *stop) break;
				const auto &event = frame.Events[eventIndex];
				if (const auto *spawn = std::get_if<SourceRigidBody>(&event.Command)) {
					const auto &source = *spawn;
					auto bodyDef = b2DefaultBodyDef();
					// The pinned wrapper creates a dynamic body and default fixture before source setters.
					bodyDef.type = source.Sensor ? b2_kinematicBody : b2_dynamicBody;
					bodyDef.position = Vec(source.Position, scale);
					const auto body = b2CreateBody(owner.Id, &bodyDef);
					auto shapeDef = b2DefaultShapeDef();
					shapeDef.enableSensorEvents = true;
					shapeDef.isSensor = source.Sensor;
					b2ShapeId shape;
					const float halfWidth = static_cast<float>(source.Size.X / (2 * scale));
					const float halfHeight = static_cast<float>(source.Size.Y / (2 * scale));
					if (source.Shape == SourceRigidShape::Box) {
						const auto polygon = b2MakeBox(halfWidth, halfHeight);
						shape = b2CreatePolygonShape(body, &shapeDef, &polygon);
					} else if (source.Shape == SourceRigidShape::Circle) {
						const b2Circle circle{{0, 0}, std::min(halfWidth, halfHeight)};
						shape = b2CreateCircleShape(body, &shapeDef, &circle);
					} else {
						b2Vec2 points[8]{};
						for (size_t i = 0; i < source.Points.size(); ++i)
							points[i] = Vec(source.Points[i], scale);
						if (source.Shape == SourceRigidShape::Segment) {
							const b2Segment segment{points[0], points[1]};
							shape = b2CreateSegmentShape(body, &shapeDef, &segment);
						} else {
							const auto hull = b2ComputeHull(points, static_cast<int>(source.Points.size()));
							const auto polygon = b2MakePolygon(&hull, 0);
							shape = b2CreatePolygonShape(body, &shapeDef, &polygon);
						}
					}
					if (source.Enabled)
						b2Body_Enable(body);
					else
						b2Body_Disable(body);
					b2Body_SetTransform(
						body,
						Vec(source.Position, scale),
						b2MakeRot(static_cast<float>(source.RotationDegrees * std::numbers::pi / 180))
					);
					b2Body_SetFixedRotation(body, false);
					b2Body_SetType(
						body,
						source.Sensor ? b2_kinematicBody : (source.Dynamic ? b2_dynamicBody : b2_staticBody)
					);
					b2Body_SetLinearDamping(body, static_cast<float>(source.LinearDamping));
					b2Body_SetAngularDamping(body, static_cast<float>(source.AngularDamping));
					b2Body_SetGravityScale(body, static_cast<float>(source.GravityScale));
					b2Body_SetBullet(body, source.Bullet);
					b2Body_SetFixedRotation(body, source.FixedRotation);
					b2Body_EnableSleep(body, source.Sleepable);
					if (source.Density) b2Shape_SetDensity(shape, static_cast<float>(*source.Density), true);
					b2Shape_SetFriction(shape, static_cast<float>(source.Friction));
					b2Shape_SetRestitution(shape, static_cast<float>(source.Restitution));
					if (source.UseInitialVelocity)
						b2Body_SetLinearVelocity(body, Vec(source.InitialVelocity));
					bodies.push_back({source.Id, body, shape, source.Sensor});
				} else if (const auto *force = std::get_if<SourceRigidForce>(&event.Command)) {
					const auto body = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
						return item.Name == force->BodyId;
					});
					b2Vec2 point = Vec(force->Point, scale);
					if (force->SourceLocalPoint) point = b2Body_GetLocalPoint(body->Id, point);
					switch (force->Kind) {
					case SourceRigidForceKind::Force:
						if (force->AtCentre)
							b2Body_ApplyForceToCenter(body->Id, Vec(force->Force), force->Wake);
						else
							b2Body_ApplyForce(body->Id, Vec(force->Force), point, force->Wake);
						break;
					case SourceRigidForceKind::Impulse:
						if (force->AtCentre)
							b2Body_ApplyLinearImpulseToCenter(body->Id, Vec(force->Force), force->Wake);
						else
							b2Body_ApplyLinearImpulse(body->Id, Vec(force->Force), point, force->Wake);
						break;
					case SourceRigidForceKind::Torque:
						b2Body_ApplyTorque(body->Id, static_cast<float>(force->Torque), force->Wake);
						break;
					case SourceRigidForceKind::AngularImpulse:
						b2Body_ApplyAngularImpulse(body->Id, static_cast<float>(force->Torque), force->Wake);
						break;
					}
				} else if (const auto *explosion = std::get_if<SourceRigidExplosion>(&event.Command)) {
					const auto point = Vec(explosion->Position, scale);
					for (const auto &id : explosion->Bodies) {
						const auto body = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
							return item.Name == id;
						});
						const auto centre = b2Body_GetWorldCenterOfMass(body->Id);
						const double dx = centre.x - point.x, dy = centre.y - point.y;
						const double distance = std::hypot(dx, dy);
						if (distance >= explosion->Radius) continue;
						if (explosion->Activate) b2Body_Enable(body->Id);
						const double falloff = 1 - distance / explosion->Radius;
						const double strength = explosion->Strength * falloff * falloff;
						const double divisor = explosion->DivideImpulseByScale ? scale : 1;
						const double x = distance > 0 ? dx / distance : 1,
									 y = distance > 0 ? dy / distance : 0;
						b2Body_ApplyLinearImpulse(
							body->Id, Vec({x * strength, y * strength}, divisor), point, true
						);
						if (explosion->Torque != 0)
							b2Body_ApplyTorque(
								body->Id, static_cast<float>(strength * explosion->Torque * -y * x), true
							);
					}
				} else if (const auto *source = std::get_if<SourceRigidJoint>(&event.Command)) {
					const auto a = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
						return item.Name == source->BodyA;
					});
					const auto b = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
						return item.Name == source->BodyB;
					});
					b2JointId joint;
					if (source->Kind == SourceRigidJointKind::Weld) {
						auto definition = b2DefaultWeldJointDef();
						definition.bodyIdA = a->Id;
						definition.bodyIdB = b->Id;
						const auto ca = b2Body_GetWorldCenterOfMass(a->Id),
								   cb = b2Body_GetWorldCenterOfMass(b->Id);
						const b2Vec2 anchor = source->Anchor
												  ? Vec(*source->Anchor, scale)
												  : b2Vec2{(ca.x + cb.x) * .5f, (ca.y + cb.y) * .5f};
						definition.localAnchorA = b2Body_GetLocalPoint(a->Id, anchor);
						definition.localAnchorB = b2Body_GetLocalPoint(b->Id, anchor);
						definition.linearHertz = definition.angularHertz =
							static_cast<float>(source->Stiffness);
						definition.linearDampingRatio = definition.angularDampingRatio =
							static_cast<float>(source->Damping);
						definition.collideConnected = false;
						joint = b2CreateWeldJoint(owner.Id, &definition);
					} else {
						auto definition = b2DefaultMotorJointDef();
						definition.bodyIdA = a->Id;
						definition.bodyIdB = b->Id;
						definition.linearOffset = Vec(source->Offset, scale);
						definition.maxForce = static_cast<float>(source->MaximumForce);
						definition.maxTorque = static_cast<float>(source->MaximumTorque);
						joint = b2CreateMotorJoint(owner.Id, &definition);
					}
					joints.push_back({joint, static_cast<float>(source->BreakForce)});
				} else if (const auto *change = std::get_if<SourceRigidChange>(&event.Command)) {
					const auto body = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
						return item.Name == change->BodyId;
					});
					auto position = b2Body_GetPosition(body->Id);
					if (change->PositionWorld) {
						const auto authored = Vec(*change->PositionWorld);
						position = change->RelativePosition
									   ? b2Vec2{position.x + authored.x, position.y + authored.y}
									   : authored;
					}
					const auto rotation =
						change->RotationRadians
							? b2MakeRot(
								  static_cast<float>(Changed(
									  b2Rot_GetAngle(b2Body_GetRotation(body->Id)), *change->RotationRadians
								  ))
							  )
							: b2Body_GetRotation(body->Id);
					if (change->PositionWorld || change->RotationRadians)
						b2Body_SetTransform(body->Id, position, rotation);
					if (change->Mass) {
						auto mass = b2Body_GetMassData(body->Id);
						const double value = Changed(mass.mass, *change->Mass);
						if (!Number(value) || value <= 0)
							return Fail(
								diagnostic,
								Status::InvalidValue,
								"Rigid mass change must remain positive and bounded"
							);
						mass.mass = static_cast<float>(value);
						// The source wrapper leaves centre/inertia uninitialized. This native profile
						// preserves them.
						b2Body_SetMassData(body->Id, mass);
					}
					if (change->Friction) {
						const double value = Changed(b2Shape_GetFriction(body->Shape), *change->Friction);
						if (!Number(value) || value < 0)
							return Fail(diagnostic, Status::InvalidValue, "Invalid resulting rigid friction");
						b2Shape_SetFriction(body->Shape, static_cast<float>(value));
					}
					if (change->Restitution) {
						const double value =
							Changed(b2Shape_GetRestitution(body->Shape), *change->Restitution);
						if (!Number(value, 1) || value < 0)
							return Fail(
								diagnostic, Status::InvalidValue, "Invalid resulting rigid restitution"
							);
						b2Shape_SetRestitution(body->Shape, static_cast<float>(value));
					}
					if (change->GravityScale) {
						const double value = Changed(b2Body_GetGravityScale(body->Id), *change->GravityScale);
						if (!Number(value))
							return Fail(
								diagnostic, Status::InvalidValue, "Invalid resulting rigid gravity scale"
							);
						b2Body_SetGravityScale(body->Id, static_cast<float>(value));
					}
					if (change->LinearVelocity)
						b2Body_SetLinearVelocity(body->Id, Vec(*change->LinearVelocity));
					if (change->Enabled) {
						if (*change->Enabled)
							b2Body_Enable(body->Id);
						else
							b2Body_Disable(body->Id);
					}
					if (change->Awake) b2Body_SetAwake(body->Id, *change->Awake);
				} else if (const auto *step = std::get_if<SourceRigidStep>(&event.Command)) {
					if (step->Simulate && step->Playing) {
						b2World_Step(
							owner.Id,
							static_cast<float>(step->TimeStepMilliseconds / 1000),
							static_cast<int>(step->Quality)
						);
						for (auto &joint : joints) {
							if (joint.Broken || joint.BreakForce <= 0) continue;
							const auto force = b2Joint_GetConstraintForce(joint.Id);
							if (force.x * force.x + force.y * force.y > joint.BreakForce * joint.BreakForce) {
								b2DestroyJoint(joint.Id);
								joint.Broken = true;
							}
						}
					}
				}
			}
		}
		SourceRigidSnapshot candidate;
		for (const auto &joint : joints) {
			if (joint.Broken)
				++candidate.BrokenJoints;
			else
				++candidate.ActiveJoints;
		}
		size_t visibleBodies = 0;
		uint64_t projectedBytes = sizeof(candidate);
		for (const auto &body : bodies) {
			if (body.Name.starts_with("__wall/")) continue;
			++visibleBodies;
			projectedBytes += sizeof(SourceRigidBodyState) + std::max<size_t>(body.Name.size(), 15) + 1;
		}
		if (projectedBytes > maximumSnapshotBytes)
			return Fail(diagnostic, Status::LimitExceeded, "Rigid snapshot byte budget exceeded");
		candidate.Bodies.reserve(visibleBodies);
		const double snapshotScale =
			history.Frames[static_cast<size_t>(tick)].SimulationScale.value_or(world.Scale);
		candidate.SimulationScale = snapshotScale;
		candidate.CanvasDimension =
			history.Frames[static_cast<size_t>(tick)].CanvasDimension.value_or(world.Dimension);
		for (size_t index = 0; index < bodies.size(); ++index) {
			const auto &body = bodies[index];
			if (!body.Name.starts_with("__wall/")) {
				const auto position = b2Body_GetPosition(body.Id),
						   velocity = b2Body_GetLinearVelocity(body.Id);
				candidate.Bodies.push_back(
					{std::string(body.Name),
					 {position.x * snapshotScale, position.y * snapshotScale},
					 {velocity.x, velocity.y},
					 b2Rot_GetAngle(b2Body_GetRotation(body.Id)) * 180 / std::numbers::pi,
					 b2Body_GetAngularVelocity(body.Id),
					 b2Body_IsAwake(body.Id),
					 {b2Body_GetWorldCenterOfMass(body.Id).x * snapshotScale,
					  b2Body_GetWorldCenterOfMass(body.Id).y * snapshotScale},
					 b2Body_GetMass(body.Id),
					 b2Shape_GetFriction(body.Shape),
					 b2Shape_GetRestitution(body.Shape),
					 b2Body_GetGravityScale(body.Id),
					 b2Body_IsEnabled(body.Id),
					 body.Sensor}
				);
			}
			if (body.Sensor) {
				const int capacity = b2Shape_GetSensorCapacity(body.Shape);
				if (capacity < 0 || capacity > 1024)
					return Fail(diagnostic, Status::LimitExceeded, "Rigid sensor overlap budget exceeded");
				std::vector<b2ShapeId> shapes(static_cast<size_t>(capacity));
				const int count = b2Shape_GetSensorOverlaps(body.Shape, shapes.data(), capacity);
				for (int i = 0; i < count; ++i) {
					const auto visitor = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
						return B2_ID_EQUALS(item.Shape, shapes[static_cast<size_t>(i)]);
					});
					if (visitor == bodies.end()) continue;
					projectedBytes += sizeof(SourceRigidOverlap) + std::max<size_t>(body.Name.size(), 15) +
									  std::max<size_t>(visitor->Name.size(), 15) + 2;
					if (candidate.Overlaps.size() == 8192 || projectedBytes > maximumSnapshotBytes)
						return Fail(
							diagnostic, Status::LimitExceeded, "Rigid sensor snapshot byte budget exceeded"
						);
					candidate.Overlaps.push_back({std::string(body.Name), std::string(visitor->Name)});
				}
				continue;
			}
			const int capacity = b2Body_GetContactCapacity(body.Id);
			if (capacity > 1024 || capacity < 0)
				return Fail(diagnostic, Status::LimitExceeded, "Rigid contact budget exceeded");
			std::vector<b2ContactData> contacts(static_cast<size_t>(capacity));
			const int count = b2Body_GetContactData(body.Id, contacts.data(), capacity);
			for (int c = 0; c < count; ++c) {
				const auto &contact = contacts[static_cast<size_t>(c)];
				const auto a = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
					return B2_ID_EQUALS(item.Shape, contact.shapeIdA);
				});
				const auto b = std::find_if(bodies.begin(), bodies.end(), [&](const auto &item) {
					return B2_ID_EQUALS(item.Shape, contact.shapeIdB);
				});
				if (a == bodies.end() || b == bodies.end() || a->Id.index1 != body.Id.index1) continue;
				double impulse = 0;
				for (int point = 0; point < contact.manifold.pointCount; ++point)
					impulse += contact.manifold.points[point].totalNormalImpulse;
				if (candidate.Contacts.size() == 8192)
					return Fail(diagnostic, Status::LimitExceeded, "Rigid snapshot contact budget exceeded");
				projectedBytes += sizeof(SourceRigidContact) + std::max<size_t>(a->Name.size(), 15) +
								  std::max<size_t>(b->Name.size(), 15) + 2;
				if (projectedBytes > maximumSnapshotBytes)
					return Fail(diagnostic, Status::LimitExceeded, "Rigid snapshot byte budget exceeded");
				SourceRigidContact captured;
				captured.A = a->Name;
				captured.B = b->Name;
				captured.Normal = {contact.manifold.normal.x, contact.manifold.normal.y};
				captured.NormalImpulse = impulse;
				captured.RollingImpulse = contact.manifold.rollingImpulse;
				captured.PointCount = static_cast<uint32_t>(contact.manifold.pointCount);
				for (uint32_t p = 0; p < captured.PointCount; ++p) {
					const auto &point = contact.manifold.points[p];
					captured.Points[p] = {
						{point.point.x, point.point.y},
						{point.anchorA.x, point.anchorA.y},
						{point.anchorB.x, point.anchorB.y},
						point.separation,
						point.normalImpulse,
						point.tangentImpulse,
						point.totalNormalImpulse,
						point.normalVelocity,
						point.id,
						point.persisted
					};
				}
				candidate.Contacts.push_back(std::move(captured));
			}
		}
		uint64_t retainedBytes = sizeof(candidate) +
								 candidate.Bodies.capacity() * sizeof(SourceRigidBodyState) +
								 candidate.Contacts.capacity() * sizeof(SourceRigidContact) +
								 candidate.Overlaps.capacity() * sizeof(SourceRigidOverlap);
		for (const auto &body : candidate.Bodies)
			retainedBytes += body.Id.capacity() + 1;
		for (const auto &contact : candidate.Contacts)
			retainedBytes += contact.A.capacity() + contact.B.capacity() + 2;
		for (const auto &overlap : candidate.Overlaps)
			retainedBytes += overlap.Sensor.capacity() + overlap.Visitor.capacity() + 2;
		if (retainedBytes > maximumSnapshotBytes)
			return Fail(diagnostic, Status::LimitExceeded, "Rigid snapshot byte budget exceeded");
		output = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}
	imagegraph::Status RigidProvider::Replay(
		const imagegraph::SourceRigidHistory &history,
		uint64_t tick,
		std::optional<imagegraph::SourceRigidEventPosition> captureAt,
		uint64_t maximumSnapshotBytes,
		imagegraph::SourceRigidSnapshot &output,
		imagegraph::Diagnostic &diagnostic
	) {
		return ReplayRigid(history, tick, output, diagnostic, std::move(captureAt), maximumSnapshotBytes);
	}

}
