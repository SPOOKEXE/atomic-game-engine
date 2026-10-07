#include "SourceParticle3DState.hpp"

#include "../MeshPayload.hpp"
#include "../NodeExecutors.hpp"
#include "../SourceGradient.hpp"
#include "../SourceRandom.hpp"
#include "Curve.hpp"
#include "Path3D.hpp"

#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace engine::imagegraph::detail {
	namespace source_particle3d_state {
		constexpr uint64_t MAXIMUM_WORK = 64000000;
		constexpr uint64_t MAXIMUM_SPAWN_INDEX = 9007199254740991ULL / 78;
		bool Refuse(NodeContext &c, std::string_view message) {
			return c.Fail(Status::InvalidValue, std::string(message), "mesh");
		}
		std::array<const Curve *, 5> Curves(const SourceParticle3DControls &c) {
			return {c.SpeedCurve, c.RotationCurve, c.SizeCurve, c.AlphaCurve, c.PathCurve};
		}
		bool Quote(NodeContext &context, const SourceParticle3DControls &c, uint64_t steps, bool initialize) {
			if (!c.PoolCapacity || c.PoolCapacity > 1024 ||
				c.SpawnMeshVertices.size() > Limits::MaximumArrayElements ||
				c.SpawnData.size() > Limits::MaximumArrayElements ||
				c.Palette.size() > Limits::MaximumArrayElements)
				return context.Fail(
					Status::LimitExceeded, "Particle 3D controls exceed bounded pools or arrays", "mesh"
				);
			uint64_t validation = c.SpawnData.size() * 3 + c.Palette.size() + c.PoolCapacity * 72;
			uint64_t vertices = 0, keys = 0, curveWork = 0;
			for (const auto group : c.SpawnMeshVertices) {
				if (group.size() > Limits::MaximumArrayElements - vertices)
					return context.Fail(
						Status::LimitExceeded, "Particle 3D spawn mesh exceeds point bounds", "spawn_mesh"
					);
				vertices += group.size();
			}
			validation += vertices * 3;
			for (const auto *gradient : {c.LifetimeColour, c.RandomColour})
				if (gradient) {
					if (gradient->Keys.size() > Limits::MaximumGradientKeys)
						return context.Fail(Status::LimitExceeded, "Particle 3D gradient exceeds key bounds");
					keys += gradient->Keys.size();
				}
			for (const Curve *curve : Curves(c))
				if (curve) {
					if (curve->Anchors.size() > Limits::MaximumCurveAnchors)
						return context.Fail(Status::LimitExceeded, "Particle 3D curve exceeds anchor bounds");
					validation += curve->Anchors.size() * 6 + 6;
					curveWork += curve->Anchors.size() * 33 * 64;
				}
			if (c.PathSampleWork > MAXIMUM_WORK || ((c.SpawnPath || c.FollowPath) && !c.PathSampleWork))
				return context.Fail(
					Status::LimitExceeded, "Particle 3D path requires an admitted sample-work quote"
				);
			const uint64_t limit = MAXIMUM_WORK / std::max<size_t>(context.ProcessorCount, 1);
			const uint64_t unit = 512 + keys * 8 + c.PathSampleWork * 2;
			const uint64_t base = validation + keys + (initialize ? curveWork : 0);
			if (base > limit || steps > (limit - base) / unit / c.PoolCapacity)
				return context.Fail(
					Status::LimitExceeded, "Particle 3D whole processor replay exceeds work bounds", "mesh"
				);
			return true;
		}
		bool ValidateControls(NodeContext &context, const SourceParticle3DControls &c) {
			if (c.SpawnType < SourceParticle3DSpawnType::Stream ||
				c.SpawnType > SourceParticle3DSpawnType::Trigger ||
				c.SpawnSource < SourceParticle3DSpawnSource::Shape ||
				c.SpawnSource > SourceParticle3DSpawnSource::DirectData ||
				c.SpawnShape < SourceParticle3DSpawnShape::Box ||
				c.SpawnShape > SourceParticle3DSpawnShape::Circle || c.PaletteSelection > 2)
				return Refuse(context, "Particle 3D selector is invalid");
			for (const auto range :
				 {c.SpawnAmount, c.Lifespan, c.ShapeVelocity, c.Size, c.Alpha, c.Gravity, c.GroundOffset})
				if (!MeshFinite(range)) return Refuse(context, "Particle 3D range must be finite");
			if (std::min(c.SpawnAmount.X, c.SpawnAmount.Y) < 0 ||
				std::max(c.SpawnAmount.X, c.SpawnAmount.Y) > std::numeric_limits<int32_t>::max() ||
				std::min(c.Lifespan.X, c.Lifespan.Y) <= 0 || c.SpawnDelay < 0 || c.BurstDuration < 0 ||
				c.SpawnDelay > int64_t(Limits::MaximumTick) || c.BurstDuration > int64_t(Limits::MaximumTick))
				return Refuse(context, "Particle 3D spawn timing, amount or lifespan is invalid");
			if (!MeshFinite(c.SpawnOrigin) || !MeshFinite(c.SpawnSpan) || !MeshFinite(c.SpawnRotation) ||
				(!std::isfinite(c.PathRange.X) || !std::isfinite(c.PathRange.Y) ||
				 !std::isfinite(c.PathRange.Z) || !std::isfinite(c.PathRange.W)) ||
				!std::isfinite(c.BounceAmount) || !std::isfinite(c.BounceFriction) ||
				!std::isfinite(c.QuaternionEpsilon) || c.QuaternionEpsilon <= 0)
				return Refuse(context, "Particle 3D spatial controls must be finite");
			for (const auto *range : {&c.Velocity, &c.Acceleration, &c.Rotation, &c.RotationSpeed, &c.Scale})
				for (double value : *range)
					if (!std::isfinite(value))
						return Refuse(context, "Particle 3D axis ranges must be finite");
			for (const auto group : c.SpawnMeshVertices)
				for (Vector3 p : group)
					if (!MeshFinite(p)) return Refuse(context, "Particle 3D spawn mesh point is nonfinite");
			for (Vector3 p : c.SpawnData)
				if (!MeshFinite(p)) return Refuse(context, "Particle 3D spawn data is nonfinite");
			for (const Gradient *gradient : {c.LifetimeColour, c.RandomColour})
				if (gradient) {
					if (gradient->Mode > 6) return Refuse(context, "Particle 3D gradient mode is invalid");
					double previous = -std::numeric_limits<double>::infinity();
					for (const auto &key : gradient->Keys) {
						if (!std::isfinite(key.Time) || key.Time < previous)
							return Refuse(context, "Particle 3D gradient times must be finite and ordered");
						previous = key.Time;
					}
				}
			for (const Curve *curve : Curves(c))
				if (curve) {
					if (curve->Anchors.size() < 2 || curve->Header[1] == 0)
						return Refuse(context, "Particle 3D curve is malformed");
					for (double value : curve->Header)
						if (!std::isfinite(value))
							return Refuse(context, "Particle 3D curve header is nonfinite");
					for (const auto &anchor : curve->Anchors)
						for (double value : anchor)
							if (!std::isfinite(value))
								return Refuse(context, "Particle 3D curve anchor is nonfinite");
				}
			return true;
		}
		bool ValidateState(NodeContext &c, const SourceParticle3DState &s, uint32_t capacity) {
			if (!s.Initialized || s.BufferIndex > 1 || s.MaximumBufferIndex >= capacity ||
				s.SpawnIndex > MAXIMUM_SPAWN_INDEX || s.Frame < -int64_t(Limits::MaximumTick) ||
				s.Frame > int64_t(Limits::MaximumTick) || s.Buffers[0].size() != capacity ||
				s.Buffers[1].size() != capacity)
				return Refuse(c, "Particle 3D prior snapshot has invalid shape or counters");
			for (const auto &buffer : s.Buffers)
				for (const auto &slot : buffer) {
					for (float value : slot.Transform.Fields)
						if (!std::isfinite(value))
							return Refuse(c, "Particle 3D transform buffer is nonfinite");
					if (!ValidParticleRecord3D(slot.Particle) ||
						(slot.Particle.Active != 0 && slot.Particle.Active != 1) ||
						slot.Particle.MeshIndex < 0 || slot.Particle.MeshIndex > MAXIMUM_SPAWN_INDEX ||
						(slot.Particle.Active != 0 && slot.Particle.LifeMaximum <= 0))
						return Refuse(c, "Particle 3D particle buffer is malformed");
					for (float value : slot.StartPosition)
						if (!std::isfinite(value))
							return Refuse(c, "Particle 3D spawn-position buffer is nonfinite");
				}
			for (const auto &map : s.CurveMaps)
				for (double value : map)
					if (!std::isfinite(value)) return Refuse(c, "Particle 3D curve map is nonfinite");
			if ((s.PathTemporary.Class != SourcePathPointClass::Planar &&
				 s.PathTemporary.Class != SourcePathPointClass::Spatial) ||
				!std::isfinite(s.PathTemporary.Weight) || !MeshFinite(s.PathTemporary.Position) ||
				(s.PathTemporary.Z && !std::isfinite(*s.PathTemporary.Z)))
				return Refuse(c, "Particle 3D path temporary is nonfinite");
			return true;
		}
		bool Allocate(NodeContext &c, uint32_t capacity, SourceParticle3DState &out) {
			auto charge = c.ReserveWorkspace(uint64_t(capacity) * 2 * sizeof(SourceParticle3DSlot));
			if (!charge) return false;
			out.Charge = std::move(*charge);
			for (auto &buffer : out.Buffers) {
				buffer.resize(capacity);
				core::Metrics::Count(
					"imagegraph.particle3d.buffer_allocated_bytes",
					buffer.capacity() * sizeof(SourceParticle3DSlot)
				);
				core::Metrics::Count("imagegraph.particle3d.buffer_allocations", 1);
				for (auto &slot : buffer)
					slot.Particle.Colour = {};
			}
			out.Initialized = true;
			return true;
		}
		bool Maps(NodeContext &c, const SourceParticle3DControls &controls, SourceParticle3DState &out) {
			const auto curves = Curves(controls);
			for (size_t map = 0; map < 5; ++map)
				for (size_t i = 0; i <= 32; ++i) {
					const double value = curves[map] ? EvalCurveX(*curves[map], i / 32., .00001) : 1;
					if (!std::isfinite(value)) return Refuse(c, "Particle 3D curveMap sample is nonfinite");
					out.CurveMaps[map][i] = value;
				}
			return true;
		}
		double Map(const std::array<double, 33> &map, double ratio) {
			if (std::isnan(ratio)) return 0;
			const double index = std::clamp(ratio, 0., 1.) * 32;
			const size_t low = size_t(std::floor(index)), high = size_t(std::ceil(index));
			return map[low] + (map[high] - map[low]) * (index - low);
		}
		Vector3 Range3(SourceRandom &random, const std::array<double, 6> &range) {
			return {
				random.Range(range[0], range[1]),
				random.Range(range[2], range[3]),
				random.Range(range[4], range[5])
			};
		}
		Vector3 Rotate(const SourceParticle3DControls &c, Vector3 p) {
			auto q = c.SpawnRotation;
			const double length = q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W;
			if (length == 0) return p;
			if (length >= c.QuaternionEpsilon) {
				const double n = std::sqrt(length);
				q = {q.X / n, q.Y / n, q.Z / n, q.W / n};
			}
			return {
				(1 - 2 * (q.Y * q.Y + q.Z * q.Z)) * p.X + 2 * (q.X * q.Y - q.W * q.Z) * p.Y +
					2 * (q.X * q.Z + q.W * q.Y) * p.Z,
				2 * (q.X * q.Y + q.W * q.Z) * p.X + (1 - 2 * (q.X * q.X + q.Z * q.Z)) * p.Y +
					2 * (q.Y * q.Z - q.W * q.X) * p.Z,
				2 * (q.X * q.Z - q.W * q.Y) * p.X + 2 * (q.Y * q.Z + q.W * q.X) * p.Y +
					(1 - 2 * (q.X * q.X + q.Y * q.Y)) * p.Z
			};
		}
		Vector3 Add(Vector3 a, Vector3 b) {
			return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
		}
		Vector3 Sub(Vector3 a, Vector3 b) {
			return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
		}
		Vector3 Times(Vector3 p, double scale) {
			return {p.X * scale, p.Y * scale, p.Z * scale};
		}
		Vector3 Jitter(SourceRandom &random, Vector3 span) {
			return {span.X * random.Range(-1, 1), span.Y * random.Range(-1, 1), span.Z * random.Range(-1, 1)};
		}
		bool SpawnPosition(
			NodeContext &context,
			const SourceParticle3DControls &c,
			SourceRandom &random,
			SourceParticle3DState &state,
			Vector3 &position,
			Vector3 &velocity,
			double shapeVelocity
		) {
			switch (c.SpawnSource) {
			case SourceParticle3DSpawnSource::Shape: {
				Vector3 offset;
				switch (c.SpawnShape) {
				case SourceParticle3DSpawnShape::Box:
					offset = Jitter(random, c.SpawnSpan);
					break;
				case SourceParticle3DSpawnShape::Sphere: {
					const double theta = random.Unit() * 2 * std::numbers::pi,
								 phi = random.Unit() * std::numbers::pi, radius = random.Unit();
					offset = {
						c.SpawnSpan.X * radius * std::sin(phi) * std::cos(theta),
						c.SpawnSpan.Y * radius * std::sin(phi) * std::sin(theta),
						c.SpawnSpan.Z * radius * std::cos(phi)
					};
					break;
				}
				case SourceParticle3DSpawnShape::Circle: {
					const double theta = random.Unit() * 2 * std::numbers::pi;
					offset = {c.SpawnSpan.X * std::cos(theta), c.SpawnSpan.Y * std::sin(theta), 0};
					break;
				}
				}
				offset = Rotate(c, offset);
				position = Add(c.SpawnOrigin, offset);
				if (shapeVelocity != 0) {
					if (c.SpawnShape == SourceParticle3DSpawnShape::Circle) offset = Rotate(c, offset);
					const double n = std::hypot(offset.X, offset.Y, offset.Z);
					if (n == 0 || !std::isfinite(n))
						return Refuse(context, "Particle 3D shape-follow velocity has undefined direction");
					velocity = Add(velocity, Times(offset, shapeVelocity / n));
				}
				break;
			}
			case SourceParticle3DSpawnSource::Path: {
				if (!c.SpawnPath || !c.SpawnPath->Valid())
					return context.Fail(
						Status::UnsupportedExecution,
						"source invalid spawn path aborts particle buffer update",
						"spawn_path"
					);
				state.PathTemporary = c.SpawnPath->RatioInto(random.Unit(), 0, state.PathTemporary);
				position =
					Add({state.PathTemporary.Position.X,
						 state.PathTemporary.Position.Y,
						 state.PathTemporary.Z.value_or(0)},
						Jitter(random, c.SpawnSpan));
				break;
			}
			case SourceParticle3DSpawnSource::MeshVertices: {
				if (c.SpawnMeshVertices.empty())
					return context.Fail(
						Status::UnsupportedExecution,
						"source empty spawn mesh aborts particle buffer update",
						"spawn_mesh"
					);
				const size_t group = random.Index(uint32_t(c.SpawnMeshVertices.size()));
				if (group >= c.SpawnMeshVertices.size() || c.SpawnMeshVertices[group].empty())
					return Refuse(context, "Particle 3D random spawn mesh group has no vertex");
				const auto points = c.SpawnMeshVertices[group];
				const size_t point = random.Index(uint32_t(points.size()));
				if (point >= points.size())
					return Refuse(context, "Particle 3D random vertex index exceeds group");
				position = Add(points[point], Jitter(random, c.SpawnSpan));
				break;
			}
			case SourceParticle3DSpawnSource::DirectData: {
				if (c.SpawnData.empty())
					return context.Fail(
						Status::UnsupportedExecution,
						"source empty spawn data aborts particle buffer update",
						"spawn_data"
					);
				const size_t point = random.Index(uint32_t(c.SpawnData.size()));
				if (point >= c.SpawnData.size())
					return Refuse(context, "Particle 3D random data index exceeds rows");
				position = Add(c.SpawnData[point], Jitter(random, c.SpawnSpan));
				break;
			}
			}
			return (MeshFinite(position) && MeshFinite(velocity)) ||
				   Refuse(context, "Particle 3D spawn exceeds finite coordinates");
		}
		Colour Multiply(Colour a, Colour b) {
			return {
				uint8_t(unsigned(a.Red) * b.Red / 255),
				uint8_t(unsigned(a.Green) * b.Green / 255),
				uint8_t(unsigned(a.Blue) * b.Blue / 255),
				uint8_t(unsigned(a.Alpha) * b.Alpha / 255)
			};
		}
		bool Store(
			NodeContext &c,
			const std::array<double, 16> &transform,
			const std::array<double, 16> &particle,
			const Vector3 &start,
			SourceParticle3DSlot &out
		) {
			std::array<float, 16> packed;
			for (size_t i = 0; i < 16; ++i) {
				if (!std::isfinite(transform[i]) || !std::isfinite(particle[i]) ||
					std::abs(transform[i]) > std::numeric_limits<float>::max() ||
					std::abs(particle[i]) > std::numeric_limits<float>::max())
					return Refuse(c, "Particle 3D buffer word exceeds finite f32 range");
				out.Transform.Fields[i] = float(transform[i]);
				packed[i] = float(particle[i]);
			}
			out.Particle = std::bit_cast<ParticleRecord3D>(packed);
			if (!ValidParticleRecord3D(out.Particle) ||
				(out.Particle.Active != 0 && out.Particle.LifeMaximum <= 0))
				return Refuse(c, "Particle 3D stored lifetime or flags are not representable");
			if (!MeshFinite(start) || std::abs(start.X) > std::numeric_limits<float>::max() ||
				std::abs(start.Y) > std::numeric_limits<float>::max() ||
				std::abs(start.Z) > std::numeric_limits<float>::max())
				return Refuse(c, "Particle 3D start position exceeds finite f32 range");
			out.StartPosition = {float(start.X), float(start.Y), float(start.Z), 0};
			return true;
		}
		bool Step(
			NodeContext &context,
			const SourceParticle3DControls &c,
			int64_t frame,
			SourceParticle3DState &state
		) {
			auto &read = state.Buffers[state.BufferIndex];
			state.BufferIndex = !state.BufferIndex;
			auto &write = state.Buffers[state.BufferIndex];
			bool spawn = false;
			if (c.Spawn) switch (c.SpawnType) {
				case SourceParticle3DSpawnType::Stream:
					spawn = c.SpawnDelay == 0 || frame % c.SpawnDelay == 0;
					break;
				case SourceParticle3DSpawnType::Burst:
					spawn = frame >= c.SpawnDelay && frame - c.SpawnDelay < c.BurstDuration;
					break;
				case SourceParticle3DSpawnType::Trigger:
					spawn = c.Trigger;
					break;
				}
			SourceRandom frameRandom(c.Seed + uint32_t(frame));
			int64_t remaining = spawn ? frameRandom.IntRange(c.SpawnAmount.X, c.SpawnAmount.Y) : 0;
			const size_t visits =
				std::min<uint64_t>(c.PoolCapacity, state.MaximumBufferIndex + uint64_t(remaining) + 1);
			for (size_t i = 0; i < visits; ++i) {
				const auto &old = read[i];
				const auto &t = old.Transform.Fields;
				Vector3 p{t[0], t[1], t[2]}, r{t[4], t[5], t[6]}, scale{t[8], t[9], t[10]},
					normal{t[12], t[13], t[14]};
				Vector3 start{old.StartPosition[0], old.StartPosition[1], old.StartPosition[2]};
				Vector3 velocity{
					old.Particle.Velocity[0], old.Particle.Velocity[1], old.Particle.Velocity[2]
				};
				double active = old.Particle.Active, index = old.Particle.MeshIndex,
					   lifeMax = old.Particle.LifeMaximum, life = old.Particle.LifeTime,
					   flags = old.Particle.RenderFlags;
				std::array<double, 4> colour{1, 1, 1, 1};
				if (active == 0 && remaining) {
					if (state.SpawnIndex >= MAXIMUM_SPAWN_INDEX)
						return Refuse(context, "Particle 3D spawn counter exceeds bounded seed precision");
					index = double(state.SpawnIndex++);
					SourceRandom random(c.Seed + uint32_t(uint64_t(index * 78)));
					velocity = Range3(random, c.Velocity);
					const double shapeVelocity = random.Range(c.ShapeVelocity.X, c.ShapeVelocity.Y);
					if (!SpawnPosition(context, c, random, state, p, velocity, shapeVelocity)) return false;
					r = Range3(random, c.Rotation);
					start = p;
					lifeMax = random.Range(c.Lifespan.X, c.Lifespan.Y);
					life = 0;
					flags = double(int32_t(flags) | (c.Billboard ? 1 : 0));
					--remaining;
					active = 1;
					state.MaximumBufferIndex = std::max(state.MaximumBufferIndex, i);
				}
				if (active != 0) {
					SourceRandom random(c.Seed + uint32_t(uint64_t(index * 78)));
					const double ratio = life / lifeMax;
					const Vector3 acceleration = Range3(random, c.Acceleration);
					const double speed = Map(state.CurveMaps[0], ratio);
					velocity = Add(velocity, Times(acceleration, life * speed));
					if (c.Follow && c.FollowPath && c.FollowPath->Valid()) {
						const double a = random.Range(c.PathRange.X, c.PathRange.Y),
									 b = random.Range(c.PathRange.Z, c.PathRange.W),
									 progress = a + (b - a) * ratio;
						state.PathTemporary.Z = 0;
						state.PathTemporary =
							c.FollowPath->RatioInto(std::clamp(progress, 0., .999), 0, state.PathTemporary);
						const auto target =
							Add({state.PathTemporary.Position.X,
								 state.PathTemporary.Position.Y,
								 state.PathTemporary.Z.value_or(0)},
								Times(start, Map(state.CurveMaps[4], progress)));
						velocity = Sub(target, p);
					}
					if (c.Physics) velocity.Z -= random.Range(c.Gravity.X, c.Gravity.Y);
					if (c.Ground) {
						const double ground = random.Range(c.GroundOffset.X, c.GroundOffset.Y);
						if (p.Z + velocity.Z < ground) {
							p.Z = ground;
							velocity.Z = -velocity.Z * c.BounceAmount;
							if (std::abs(velocity.Z) < .1) {
								velocity.X *= c.BounceFriction;
								velocity.Y *= c.BounceFriction;
							}
						}
					}
					p = Add(p, Times(velocity, speed));
					r = Add(r, Times(Range3(random, c.RotationSpeed), Map(state.CurveMaps[1], ratio)));
					const double size = Map(state.CurveMaps[2], ratio) * random.Range(c.Size.X, c.Size.Y);
					scale = Times(Range3(random, c.Scale), size);
					if (c.FollowVelocity && life > 0) normal = velocity;
					++life;
					if (life > lifeMax) active = 0;
					const auto c0 = c.LifetimeColour ? SourceGradientAt(*c.LifetimeColour, ratio)
													 : std::optional<Colour>{{255, 255, 255, 255}};
					const double randomRatio = random.Unit();
					const auto c1 = c.RandomColour ? SourceGradientAt(*c.RandomColour, randomRatio)
												   : std::optional<Colour>{{255, 255, 255, 255}};
					if (!c0 || !c1) return Refuse(context, "Particle 3D gradient evaluation is nonfinite");
					size_t selected = size_t(index);
					if (c.PaletteSelection == 0 && !c.Palette.empty())
						selected %= c.Palette.size();
					else if (c.PaletteSelection == 1 && !c.Palette.empty()) {
						const size_t period = c.Palette.size() * 2 - 1, value = selected % period;
						selected = value >= c.Palette.size() ? period - value : value;
					} else if (c.PaletteSelection == 2) {
						if (c.Palette.empty()) {
							random.Unit();
							random.Unit();
						} else
							selected = random.Index(uint32_t(c.Palette.size()));
					}
					const Colour c2 =
						selected < c.Palette.size() ? c.Palette[selected] : Colour{255, 255, 255, 255};
					const Colour blend = Multiply(*c0, Multiply(*c1, c2));
					const double alpha = random.Range(c.Alpha.X, c.Alpha.Y) * Map(state.CurveMaps[3], ratio);
					colour = {
						blend.Red / 255., blend.Green / 255., blend.Blue / 255., blend.Alpha / 255. * alpha
					};
				}
				const std::array<double, 16> transform{
					p.X,
					p.Y,
					p.Z,
					0,
					r.X,
					r.Y,
					r.Z,
					0,
					scale.X,
					scale.Y,
					scale.Z,
					0,
					normal.X,
					normal.Y,
					normal.Z,
					0
				};
				const std::array<double, 16> particle{
					active,
					index,
					lifeMax,
					life,
					flags,
					0,
					0,
					0,
					colour[0],
					colour[1],
					colour[2],
					colour[3],
					velocity.X,
					velocity.Y,
					velocity.Z,
					0
				};
				if (!Store(context, transform, particle, start, write[i])) return false;
				core::Metrics::Count(
					"imagegraph.particle3d.slot_written_bytes", sizeof(SourceParticle3DSlot)
				);
				core::Metrics::Count("imagegraph.particle3d.slot_steps", 1);
			}
			return context.FailureCode == Status::Ok;
		}
		bool Copy(
			NodeContext &c, const SourceParticle3DState &old, SourceParticle3DState &out, uint32_t capacity
		) {
			if (!Allocate(c, capacity, out)) return false;
			out.Buffers = old.Buffers;
			core::Metrics::Count(
				"imagegraph.particle3d.buffer_copied_bytes",
				(old.Buffers[0].size() + old.Buffers[1].size()) * sizeof(SourceParticle3DSlot)
			);
			out.CurveMaps = old.CurveMaps;
			out.PathTemporary = old.PathTemporary;
			out.SpawnIndex = old.SpawnIndex;
			out.BufferIndex = old.BufferIndex;
			out.MaximumBufferIndex = old.MaximumBufferIndex;
			out.Frame = old.Frame;
			return true;
		}
	}
	bool InitializeSourceParticle3DState(
		NodeContext &context, const SourceParticle3DControls &controls, SourceParticle3DState &result
	) try {
		ENGINE_PROFILE("imagegraph.particle3d.initialize");
		if (!source_particle3d_state::Quote(context, controls, 0, true) ||
			!source_particle3d_state::ValidateControls(context, controls))
			return false;
		SourceParticle3DState prepared;
		if (!source_particle3d_state::Allocate(context, controls.PoolCapacity, prepared) ||
			!source_particle3d_state::Maps(context, controls, prepared))
			return false;
		result = std::move(prepared);
		return true;
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Particle 3D state allocation failed");
	} catch (const std::length_error &) {
		return context.Fail(Status::LimitExceeded, "Particle 3D state exceeds container bounds");
	}
	bool BeginSourceParticle3DState(
		NodeContext &context,
		const SourceParticle3DControls &controls,
		int64_t frame,
		SourceParticle3DState &result
	) try {
		ENGINE_PROFILE("imagegraph.particle3d.begin");
		if (frame < -int64_t(Limits::MaximumTick) || frame > int64_t(Limits::MaximumTick))
			return source_particle3d_state::Refuse(
				context, "Particle 3D first frame is outside timeline bounds"
			);
		const int64_t prerender =
			controls.Loop
				? (controls.PreRender == -1 ? controls.TotalFrames : std::max<int64_t>(controls.PreRender, 0))
				: 0;
		if (!source_particle3d_state::Quote(context, controls, uint64_t(prerender) + 1, true) ||
			!source_particle3d_state::ValidateControls(context, controls))
			return false;
		SourceParticle3DState prepared;
		if (!source_particle3d_state::Allocate(context, controls.PoolCapacity, prepared) ||
			!source_particle3d_state::Maps(context, controls, prepared))
			return false;
		for (int64_t i = int64_t(controls.TotalFrames) - prerender; i < int64_t(controls.TotalFrames); ++i)
			if (!source_particle3d_state::Step(context, controls, i, prepared)) return false;
		prepared.SpawnIndex = 0;
		if (!source_particle3d_state::Step(context, controls, frame, prepared)) return false;
		prepared.Frame = frame;
		result = std::move(prepared);
		return true;
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Particle 3D prerender allocation failed");
	} catch (const std::length_error &) {
		return context.Fail(Status::LimitExceeded, "Particle 3D prerender exceeds container bounds");
	}
	bool AdvanceSourceParticle3DState(
		NodeContext &context,
		const SourceParticle3DControls &controls,
		const SourceParticle3DState &previous,
		int64_t frame,
		SourceParticle3DState &result
	) try {
		ENGINE_PROFILE("imagegraph.particle3d.advance");
		if (!source_particle3d_state::Quote(context, controls, frame == previous.Frame ? 0 : 1, false) ||
			!source_particle3d_state::ValidateControls(context, controls) ||
			!source_particle3d_state::ValidateState(context, previous, controls.PoolCapacity))
			return false;
		if (frame != previous.Frame &&
			(previous.Frame == int64_t(Limits::MaximumTick) || frame != previous.Frame + 1))
			return context.Fail(
				Status::UnsupportedExecution, "Particle 3D frame gap requires explicit replay"
			);
		SourceParticle3DState prepared;
		if (!source_particle3d_state::Copy(context, previous, prepared, controls.PoolCapacity)) return false;
		if (frame != previous.Frame && !source_particle3d_state::Step(context, controls, frame, prepared))
			return false;
		prepared.Frame = frame;
		result = std::move(prepared);
		return true;
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Particle 3D step allocation failed");
	} catch (const std::length_error &) {
		return context.Fail(Status::LimitExceeded, "Particle 3D step exceeds container bounds");
	}
	bool ValidateSourceParticle3DState(NodeContext &context, const SourceParticle3DState &state) {
		SourceParticle3DControls controls;
		if (state.Buffers[0].size() > 1024)
			return context.Fail(Status::LimitExceeded, "Particle 3D decoded pool exceeds bounds");
		controls.PoolCapacity = static_cast<uint32_t>(state.Buffers[0].size());
		return source_particle3d_state::Quote(context, controls, 0, false) &&
			   source_particle3d_state::ValidateState(context, state, controls.PoolCapacity);
	}
	size_t SourceParticle3DDrawCount(const SourceParticle3DState &state) noexcept {
		return state.Initialized ? state.MaximumBufferIndex : 0;
	}
}
