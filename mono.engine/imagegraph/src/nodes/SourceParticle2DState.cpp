#include "SourceParticle2DState.hpp"

#include "../NodeExecutors.hpp"
#include "../ParticlePayload.hpp"
#include "../SourceGradient.hpp"
#include "../SourceRandom.hpp"
#include "../ValuePayload.hpp"
#include "Curve.hpp"
#include "Path.hpp"
#include "Sampler.hpp"

#include <bit>
#include <cmath>
#include <new>
#include <numbers>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace source_particle2d_state {
		constexpr double DEG = std::numbers::pi / 180;
		constexpr uint64_t MAX_WORK = 64000000;
		struct Random {
			uint32_t Seed;
			SourceRandom Generator;
			explicit Random(uint32_t seed) : Seed(seed), Generator(seed) {}
			void Reset(uint32_t seed) {
				Seed = seed;
				Generator = SourceRandom(seed);
			}
			double Unit() {
				return Generator.Unit();
			}
			double Range(double a, double b) {
				return Generator.Range(a, b);
			}
			int64_t Int(double a, double b) {
				return Generator.IntRange(a, b);
			}
			double SeedRange(double a, double b, double seed) {
				Reset(uint32_t(int64_t(std::floor(seed))));
				const double first = Unit();
				Reset(uint32_t(int64_t(std::floor(seed)) + 1));
				const double second = Unit();
				return a + (b - a) * (first + (second - first) * (seed - std::floor(seed)));
			}
		};
		uint32_t Packed(Colour colour) {
			return colour.Red | (uint32_t(colour.Green) << 8) | (uint32_t(colour.Blue) << 16) |
				   (uint32_t(colour.Alpha) << 24);
		}
		uint32_t Multiply(uint32_t a, uint32_t b) {
			uint32_t result = 0;
			for (unsigned shift = 0; shift < 32; shift += 8)
				result |= uint32_t(double((a >> shift) & 255) * ((b >> shift) & 255) / 255) << shift;
			return result;
		}
		double Direction(double x, double y) {
			double angle = std::atan2(-y, x) / DEG;
			return angle < 0 ? angle + 360 : angle;
		}
		double Rotation(
			Random &random, const SourceParticle2DRotation &rotation, size_t index, double uniform = -1
		) {
			const auto &v = rotation.Values;
			if (rotation.Count == 1) return v[0];
			if (rotation.Count == 2) return random.SeedRange(v[0], v[1], random.Seed);
			const double seed = random.Seed;
			const auto range = [&](double a, double b) { return random.SeedRange(a, b, seed); };
			if (uniform >= 0) {
				if (v[0] == 0) return v[1] + (v[2] - v[1]) * uniform;
				if (v[0] == 1) return v[1] - v[2] + 2 * v[2] * uniform;
				if (v[0] == 2) {
					const double a = std::abs(v[1] - v[2]), b = std::abs(v[3] - v[4]);
					const double ratio = a / (a + b);
					return uniform < ratio ? v[1] + (v[2] - v[1]) * uniform / ratio
										   : v[3] + (v[4] - v[3]) * (uniform - ratio) / (1 - ratio);
				}
				return uniform < .5 ? v[1] - v[3] + 4 * v[3] * uniform
									: v[2] - v[3] + 4 * v[3] * (uniform - .5);
			}
			if (v[0] == 0) return range(v[1], v[2]);
			if (v[0] == 1) return range(v[1] - v[2], v[1] + v[2]);
			const bool alternate = rotation.Count > 5 && v[5] == 1;
			const double first = v[0] == 2 ? range(v[1], v[2]) : range(v[1] - v[3], v[1] + v[3]);
			if (alternate && index % 2) return first;
			const double second = v[0] == 2 ? range(v[3], v[4]) : range(v[2] - v[3], v[2] + v[3]);
			if (alternate) return second;
			return random.Int(0, 1) == 0 ? first : second;
		}
		Vector2 AreaPoint(Random &r, const SourceParticle2DControls &c, size_t index, double total) {
			const auto &a = c.SpawnArea;
			if (total == 0) return {};
			const double i = std::fmod(double(index), total);
			if (c.SpawnSource == 0 && c.Distribution == 2 && !c.PoissonPoints.empty())
				return c.PoissonPoints[size_t(r.Int(0, c.PoissonPoints.size() - 1))];
			if (c.Distribution == 0) {
				if (c.SpawnSource == 0 && a.Shape == 0) {
					const double columns = std::ceil(std::sqrt(total)), rows = std::ceil(total / columns);
					return {
						a.CenterX - a.HalfWidth + (std::fmod(i, columns) + .5) * a.HalfWidth * 2 / columns,
						a.CenterY - a.HalfHeight + (std::floor(i / columns) + .5) * a.HalfHeight * 2 / rows
					};
				}
				if (c.SpawnSource == 0) {
					if (i == 0) return {a.CenterX, a.CenterY};
					const double j = i - 1, n = std::floor((-3 + std::sqrt(9 + 12 * j)) / 6),
								 tn = std::floor((-3 + std::sqrt(9 + 12 * (total - 2))) / 6) + 1,
								 previous = std::floor(n * (n + 1) / 2 * 6),
								 row = n + 1 >= tn ? total - 2 - previous + 1 : (n + 1) * 6,
								 radius = (n + 1) * a.HalfWidth / tn,
								 angle = (j - previous) / row * 360 * DEG;
					return {
						a.CenterX + radius * std::cos(angle),
						a.CenterY - radius * std::sin(angle) / (a.HalfWidth / a.HalfHeight)
					};
				}
				if (a.Shape != 0)
					return {
						a.CenterX + a.HalfWidth * std::cos(360 * i / total * DEG),
						a.CenterY - a.HalfHeight * std::sin(360 * i / total * DEG)
					};
				double distance = (a.HalfWidth + a.HalfHeight) * 4 * i / total;
				if (distance <= a.HalfWidth * 2)
					return {a.CenterX - a.HalfWidth + distance, a.CenterY - a.HalfHeight};
				distance -= a.HalfWidth * 2;
				if (distance <= a.HalfHeight * 2)
					return {a.CenterX + a.HalfWidth, a.CenterY - a.HalfHeight + distance};
				distance -= a.HalfHeight * 2;
				if (distance <= a.HalfWidth * 2)
					return {a.CenterX + a.HalfWidth - distance, a.CenterY + a.HalfHeight};
				return {a.CenterX - a.HalfWidth, a.CenterY + a.HalfHeight - (distance - a.HalfWidth * 2)};
			}
			uint32_t seed = r.Seed;
			if (c.SpawnSource == 0 && a.Shape == 0)
				return {
					a.CenterX + r.SeedRange(-a.HalfWidth, a.HalfWidth, seed),
					a.CenterY + r.SeedRange(-a.HalfHeight, a.HalfHeight, seed + 1)
				};
			if (c.SpawnSource == 0) {
				const double angle = r.Unit() * 360 * DEG;
				const double x = r.SeedRange(0, a.HalfWidth, seed),
							 y = r.SeedRange(0, a.HalfHeight, seed + 1);
				return {a.CenterX + std::cos(angle) * x, a.CenterY - std::sin(angle) * y};
			}
			if (a.Shape != 0) {
				const double angle = r.SeedRange(0, 360, seed) * DEG;
				return {
					a.CenterX + a.HalfWidth * std::cos(angle), a.CenterY - a.HalfHeight * std::sin(angle)
				};
			}
			const double choice = r.SeedRange(0, (a.HalfWidth + a.HalfHeight) * 2, seed);
			if (choice < a.HalfWidth)
				return {
					a.CenterX + r.SeedRange(-a.HalfWidth, a.HalfWidth, seed + 1), a.CenterY - a.HalfHeight
				};
			if (choice < a.HalfWidth + a.HalfHeight)
				return {
					a.CenterX - a.HalfWidth, a.CenterY + r.SeedRange(-a.HalfHeight, a.HalfHeight, seed + 1)
				};
			if (choice < a.HalfWidth * 2 + a.HalfHeight)
				return {
					a.CenterX + r.SeedRange(-a.HalfWidth, a.HalfWidth, seed + 1), a.CenterY + a.HalfHeight
				};
			return {a.CenterX + a.HalfWidth, a.CenterY + r.SeedRange(-a.HalfHeight, a.HalfHeight, seed + 1)};
		}
		bool Fail(NodeContext &c, std::string_view text) {
			return c.Fail(Status::InvalidValue, std::string(text), "data");
		}
		std::optional<Vector2>
		MapPoint(const SourceParticle2DControls &c, int64_t index, int64_t amount, uint32_t seed) {
			if (!c.DistributionMap) return std::nullopt;
			const float coordinate = (float(index) + .5f) / float(amount);
			const auto noise = [](float x, float y, float seedValue) {
				const float value =
					std::sin((x * 12.9898f + y * 78.233f) * std::fmod(seedValue, 32.156f) * 12.588f) *
					43758.5453123f;
				return value - std::floor(value);
			};
			float maximum = 0;
			std::optional<Vector2> point;
			for (int attempt = 0; attempt < 8; ++attempt) {
				const float x = noise(float(attempt) + coordinate, coordinate, 132.54664f + float(seed)),
							y = noise(coordinate, float(attempt) + coordinate, 78.29131f + float(seed)),
							weight = noise(
								float(attempt) + coordinate,
								float(attempt) + coordinate,
								8.10684f + float(seed)
							);
				const auto colour = Texture(*c.DistributionMap, x, y, false);
				const float gray = (float(colour[0]) + float(colour[1]) + float(colour[2])) / 3.f;
				const float brightness = gray * float(colour[3]) * weight;
				if (brightness > maximum) {
					maximum = brightness;
					point = Vector2{SourceColorByte(x * 255) / 255., SourceColorByte(y * 255) / 255.};
				}
			}
			return point;
		}
		bool Spawn(NodeContext &context, const SourceParticle2DControls &c, SourceParticle2DState &s) {
			if (c.SpriteEmptyArray) return true;
			Random r(s.Seed);
			s.Seed += 1000;
			int64_t amount = r.Int(c.SpawnAmount.X, c.SpawnAmount.Y);
			if (amount < 0 || amount > int64_t(Limits::MaximumArrayElements))
				return Fail(context, "Particle spawn amount exceeds bounds");
			if (uint64_t(amount) >
				MAX_WORK / std::max<size_t>(context.ProcessorCount, 1) / (512 + c.PathSampleWork * 2))
				return context.Fail(
					Status::LimitExceeded, "Particle spawn exceeds whole processor work bounds", "data"
				);
			if (c.SpawnSource == 4) amount = std::min<int64_t>(amount, c.SpawnData.size());
			const double period = c.UniformPeriod <= 0 ? double(amount) : c.UniformPeriod;
			if (c.SpawnSource == 2 && context.Request.RequireSourceGpuRasterCoverage)
				return context.Fail(
					Status::UnsupportedExecution,
					"Particle distribution map requires captured source GPU shader coverage",
					"distribution_map"
				);
			for (int64_t i = 0; i < amount; ++i) {
				auto &p = s.Slots[s.Runner];
				const size_t spriteIndex = c.Sprites.empty()		? 0
										   : !c.SpriteArray			? 0
										   : c.SpriteSelection == 0 ? size_t(r.Int(0, c.Sprites.size() - 1))
										   : c.SpriteSelection == 1 ? s.SpawnIndex % c.Sprites.size()
																	: 0;
				if ((!c.Rotations.empty() && size_t(i) >= c.Rotations.size()) ||
					(!c.Directions.empty() && size_t(i) >= c.Directions.size()))
					return Fail(context, "Particle per-spawn rotation array has too few rows");
				Vector2 position;
				if (spriteIndex < c.SpriteAtlasRects.size() && c.SpriteAtlasRects[spriteIndex]) {
					p.AtlasRect = c.SpriteAtlasRects[spriteIndex];
					position = {
						c.SpawnArea.CenterX + p.AtlasRect->X + p.AtlasRect->Z / 2,
						c.SpawnArea.CenterY + p.AtlasRect->Y + p.AtlasRect->W / 2
					};
				} else if (c.SpawnSource < 2)
					position = AreaPoint(r, c, s.SpawnIndex, period);
				else if (c.SpawnSource == 2) {
					const auto point = MapPoint(c, i, amount, s.Seed);
					if (!point) continue;
					position = {
						c.SpawnArea.CenterX + c.SpawnArea.HalfWidth * (point->X * 2 - 1),
						c.SpawnArea.CenterY + c.SpawnArea.HalfHeight * (point->Y * 2 - 1)
					};
				} else if (c.SpawnSource == 3) {
					if (!c.SpawnPath) continue;
					const double progress = c.Distribution == 0
												? std::fmod(double(s.SpawnIndex), period) / (period - 1)
												: r.Unit();
					const auto point = c.SpawnPath->PointRatio(progress);
					position = {point.X, point.Y};
				} else {
					if (c.SpawnData.empty()) continue;
					const size_t index = c.Distribution == 0 ? s.SpawnTotal % c.SpawnData.size()
															 : size_t(r.Int(0, c.SpawnData.size() - 1));
					position = c.SpawnData[index];
				}
				const int64_t life = r.Int(c.Lifespan.X, c.Lifespan.Y);
				if (life < 0 || life > int64_t(Limits::MaximumRangeFrames))
					return Fail(context, "Particle lifespan exceeds history bounds");
				uint64_t bytes = std::max<size_t>(size_t(life), p.Data.State.XHistory.capacity()) * 2 *
									 sizeof(std::optional<double>) +
								 (std::max<size_t>(size_t(life), p.ScaleXHistory.capacity()) +
								  std::max<size_t>(size_t(life), p.ScaleYHistory.capacity()) +
								  std::max<size_t>(size_t(life), p.AlphaHistory.capacity())) *
									 sizeof(double) +
								 std::max<size_t>(size_t(life), p.BlendHistory.capacity()) * sizeof(uint32_t);
				for (const auto &sprite : c.Sprites)
					bytes += sizeof(Image) + sprite.Pixels.size();
				bytes += c.LifetimeColour ? c.LifetimeColour->Keys.size() * sizeof(GradientKey) : 0;
				if (c.PathPayload) bytes += PayloadOwnedBytes(*c.PathPayload);
				auto charge = context.ReserveWorkspace(bytes, "data");
				if (!charge) return false;
				p.Data.State.Position = {position.X, position.Y};
				p.Start = p.Data.State.Position;
				p.PreviousDefined = p.DrawDefined = false;
				p.Data.State.Active = true;
				p.Life = p.LifeTotal = double(life);
				p.HistoryIndex = p.TrailLife = 0;
				p.Data.State.XHistory.assign(size_t(life), 0.);
				p.Data.State.YHistory.assign(size_t(life), 0.);
				p.ScaleXHistory.assign(size_t(life), 0);
				p.ScaleYHistory.assign(size_t(life), 0);
				p.AlphaHistory.assign(size_t(life), 0);
				p.BlendHistory.assign(size_t(life), 0);
				p.Data.Sprites.assign(c.Sprites.begin(), c.Sprites.end());
				p.Data.State.SpriteSlot =
					c.Sprites.empty() ? std::optional<uint32_t>{} : uint32_t(spriteIndex);
				p.Seed = uint32_t(r.Int(100000, 999999));
				p.AnimationSpeed = r.Range(c.AnimationSpeed.X, c.AnimationSpeed.Y);
				p.StretchAnimation = c.StretchAnimation;
				p.AnimationEnd = c.AnimationEnd;
				p.SpriteSelection = c.SpriteSelection;
				const auto &rotation = c.Rotations.empty() ? c.Rotation : c.Rotations[size_t(i)];
				p.BaseRotation = p.Data.State.RotationDegrees = Rotation(r, rotation, s.SpawnIndex);
				p.RotationType = c.RotationType;
				if (c.RotationType == 0)
					p.RotationSpeed = Rotation(r, c.RotationSpeed, s.SpawnIndex);
				else
					p.TargetAngle =
						Rotation(r, c.TargetAngle, s.SpawnIndex) + (c.RotationType == 1 ? p.BaseRotation : 0);
				p.SnapRotation = c.SnapRotation;
				p.RotateByDirection = c.RotateByDirection;
				const double size = r.Range(c.Size.X, c.Size.Y);
				p.BaseScale = {r.Range(c.Scale.X, c.Scale.Y) * size, r.Range(c.Scale.Z, c.Scale.W) * size};
				p.Alpha = r.Range(c.Alpha.CenterX, c.Alpha.CenterY);
				const auto spawnColour = c.SpawnColour ? SourceGradientAt(*c.SpawnColour, r.Unit())
													   : std::optional<Colour>{Colour{255, 255, 255, 255}};
				if (!spawnColour) return Fail(context, "Particle spawn gradient is undefined");
				size_t paletteIndex = s.SpawnIndex;
				if (!c.Palette.empty()) {
					if (c.PaletteSelection == 0)
						paletteIndex %= c.Palette.size();
					else if (c.PaletteSelection == 1) {
						const size_t span = c.Palette.size() * 2 - 1;
						paletteIndex = span ? paletteIndex % span : 0;
						if (paletteIndex >= c.Palette.size()) paletteIndex = span - paletteIndex;
					} else
						paletteIndex = size_t(r.Int(0, c.Palette.size() - 1));
				}
				p.Blend = Multiply(
					Packed(*spawnColour),
					Packed(
						paletteIndex < c.Palette.size() ? c.Palette[paletteIndex] : Colour{255, 255, 255, 255}
					)
				);
				p.Data.State.Blend = p.Blend & 0x00ffffff;
				p.Data.State.Alpha = p.Alpha;
				if (c.LifetimeColour)
					p.LifetimeColour = *c.LifetimeColour;
				else
					p.LifetimeColour = Gradient{};
				if (c.SampleSurface) {
					const auto pixel = Texture(
						*c.SampleSurface,
						position.X / c.SampleSurface->Width,
						position.Y / c.SampleSurface->Height,
						false
					);
					p.Blend = Multiply(
						p.Blend,
						Packed(
							{SourceColorByte(pixel[0] * 255),
							 SourceColorByte(pixel[1] * 255),
							 SourceColorByte(pixel[2] * 255),
							 SourceColorByte(pixel[3] * 255)}
						)
					);
					p.Data.State.Blend = p.Blend & 0x00ffffff;
				}
				const double shift = r.Range(c.RangeShift.X, c.RangeShift.Y);
				p.PathRange = {
					r.Range(c.PathRange.X, c.PathRange.Y) + shift,
					r.Range(c.PathRange.Z, c.PathRange.W) + shift
				};
				p.FollowPath = c.PathPayload ? std::optional<Path2D>{*c.PathPayload} : std::nullopt;
				p.PathDeviation = c.Deviation;
				p.PathLoop = c.PathLoop;
				const auto &directions = c.Directions.empty() ? c.Direction : c.Directions[size_t(i)];
				if (c.DirectionDistribution == 1 && amount == 1)
					return Fail(context, "Particle uniform direction has an undefined one-particle ratio");
				double direction = Rotation(
					r, directions, s.SpawnIndex, c.DirectionDistribution == 1 ? double(i) / (amount - 1) : -1
				);
				if (c.DirectedFromCenter)
					direction +=
						c.AngleRange.X +
						(c.AngleRange.Y - c.AngleRange.X) *
							Direction(position.X - c.SpawnArea.CenterX, position.Y - c.SpawnArea.CenterY) /
							360;
				const double speed = r.Range(c.Speed.X, c.Speed.Y);
				p.Velocity = p.InitialVelocity = {
					speed * std::cos(direction * DEG), -speed * std::sin(direction * DEG)
				};
				p.Direction = Direction(p.Velocity[0], p.Velocity[1]);
				p.DirectionSpeed = std::hypot(p.Velocity[0], p.Velocity[1]);
				p.Acceleration = r.Range(c.Acceleration.X, c.Acceleration.Y);
				p.Friction = r.Range(c.Friction.X, c.Friction.Y);
				const double gravity = r.Range(c.Gravity.X, c.Gravity.Y);
				p.GravityX = gravity * std::cos(c.GravityDirection * DEG);
				p.GravityY = -gravity * std::sin(c.GravityDirection * DEG);
				p.Turning =
					r.Range(c.Turning.X, c.Turning.Y) * (c.TurnBothDirections ? (r.Int(0, 1) ? 1 : -1) : 1);
				p.TurnScale = c.TurnScaleWithSpeed;
				p.Physics = c.Physics;
				p.Ground = c.Ground;
				p.GroundY =
					r.Range(c.GroundOffset.X, c.GroundOffset.Y) + (c.GroundOffsetType == 0 ? position.Y : 0);
				p.Bounce = c.BounceAmount;
				p.GroundFriction = std::clamp(1 - c.BounceFriction, 0., 1.);
				p.Wiggles = c.Wiggles;
				p.Wrap = c.Wrap;
				p.RenderType = c.RenderType;
				p.Charge = std::move(*charge);
				s.SpawnIndex = (s.SpawnIndex + 1) % s.Slots.size();
				s.Runner = (s.Runner + 1) % s.Slots.size();
				++s.SpawnTotal;
			}
			return true;
		}
		double Wiggle(const SourceParticle2DState &s, size_t map, double index) {
			return s.WiggleMaps[map][size_t(std::abs(index)) % 1000] * s.WiggleAmplitudes[map];
		}
		bool Step(NodeContext &context, SourceParticle2DState &s, SourceParticle2DSlot &p) {
			if (p.LifeTotal <= 0) return true;
			const double ratio = std::clamp(1 - p.Life / p.LifeTotal, 0., 1.);
			const auto old = p.Data.State.Position;
			p.Data.State.Position[0] += p.Velocity[0] * SourceParticle2DCurveAt(s, 0, ratio);
			if (p.Ground && p.Data.State.Position[1] + p.Velocity[1] > p.GroundY) {
				p.Data.State.Position[1] = p.GroundY;
				p.Velocity[1] = -p.Velocity[1] * p.Bounce;
				if (std::abs(p.Velocity[1]) < .1) p.Velocity[0] *= p.GroundFriction;
			} else
				p.Data.State.Position[1] += p.Velocity[1] * SourceParticle2DCurveAt(s, 0, ratio);
			double direction = Direction(p.Velocity[0], p.Velocity[1]),
				   speed = std::hypot(p.Velocity[0], p.Velocity[1]);
			if (p.Physics) {
				speed = std::max(0., speed + p.Acceleration) * (1 - p.Friction);
				if (p.Turning != 0 && (p.Velocity[0] != 0 || p.Velocity[1] != 0))
					direction += p.Turning * (p.TurnScale > 0	? speed * p.TurnScale
											  : p.TurnScale < 0 ? p.TurnScale / speed
																: 1);
			}
			if (p.Wiggles && speed != 0) direction += Wiggle(s, 5, p.Seed + p.Life);
			p.Velocity = {speed * std::cos(direction * DEG), -speed * std::sin(direction * DEG)};
			if (p.Physics) {
				p.Velocity[0] += p.GravityX;
				p.Velocity[1] += p.GravityY;
			}
			if (p.RotationType == 0) {
				p.BaseRotation += p.RotationSpeed * SourceParticle2DCurveAt(s, 1, ratio);
				p.Data.State.RotationDegrees = p.BaseRotation + (p.RotateByDirection ? p.Direction : 0);
			} else {
				const double difference =
					std::fmod(std::fmod(p.TargetAngle - p.BaseRotation + 180, 360) + 360, 360) - 180;
				p.Data.State.RotationDegrees =
					p.BaseRotation + difference * SourceParticle2DCurveAt(s, 2, ratio);
			}
			if (p.Life-- < 0) p.Data.State.Active = false;
			if (p.PreviousDefined && p.DrawDefined) {
				p.DirectionSpeed = std::hypot(p.Draw[0] - p.Previous[0], p.Draw[1] - p.Previous[1]);
				if (p.DirectionSpeed > 1)
					p.Direction = Direction(p.Draw[0] - p.Previous[0], p.Draw[1] - p.Previous[1]);
			}
			if (p.DrawDefined) {
				if (p.HistoryIndex >= Limits::MaximumRangeFrames)
					return Fail(context, "Particle history index exceeds bounds");
				if (p.HistoryIndex >= p.Data.State.XHistory.size()) {
					auto charge = context.ReserveWorkspace(
						2 * sizeof(std::optional<double>) + 3 * sizeof(double) + sizeof(uint32_t), "data"
					);
					if (!charge || !p.Charge.Merge(std::move(*charge))) return false;
					p.Data.State.XHistory.resize(p.HistoryIndex + 1);
					p.Data.State.YHistory.resize(p.HistoryIndex + 1);
					p.ScaleXHistory.resize(p.HistoryIndex + 1);
					p.ScaleYHistory.resize(p.HistoryIndex + 1);
					p.AlphaHistory.resize(p.HistoryIndex + 1);
					p.BlendHistory.resize(p.HistoryIndex + 1);
				}
				p.Data.State.XHistory[p.HistoryIndex] = p.Draw[0];
				p.Data.State.YHistory[p.HistoryIndex++] = p.Draw[1];
			}
			p.Previous = old;
			p.PreviousDefined = p.DrawDefined = true;
			p.Draw = p.Data.State.Position;
			p.DrawScale = p.BaseScale;
			p.DrawRotation = p.Data.State.RotationDegrees;
			if (p.Wiggles) {
				p.Draw[0] += Wiggle(s, 0, p.Seed + p.Life);
				p.Draw[1] += Wiggle(s, 1, p.Seed + p.Life);
				p.DrawRotation += Wiggle(s, 4, p.Seed + p.Life);
				p.DrawScale[0] += Wiggle(s, 3, p.Seed + p.Life);
				p.DrawScale[1] += Wiggle(s, 3, p.Seed + p.Life);
			}
			if (p.FollowPath) {
				PathRuntime path;
				if (!path.Init(context, *p.FollowPath)) return false;
				double progress =
					p.PathRange[0] + (p.PathRange[1] - p.PathRange[0]) * SourceParticle2DCurveAt(s, 5, ratio);
				progress = p.PathLoop ? progress - std::trunc(progress) : std::clamp(progress, 0., .999);
				const double deviation =
					p.PathDeviation == 0 ? 0 : SourceParticle2DCurveAt(s, 6, progress) * p.PathDeviation;
				const auto point = path.PointRatio(progress);
				p.Draw = {point.X + p.Draw[0] * deviation, point.Y + p.Draw[1] * deviation};
			}
			if (p.RenderType == 1) {
				const auto colour = p.LifetimeColour.Keys.empty()
										? std::optional<Colour>{Colour{0, 0, 0, 255}}
										: SourceCachedGradient(p.LifetimeColour, ratio);
				if (!colour) return Fail(context, "Particle line gradient is undefined");
				const uint32_t packed = Multiply(p.Blend, Packed(*colour));
				p.Data.State.Alpha = p.Alpha * SourceParticle2DCurveAt(s, 4, ratio) * ((packed >> 24) / 255.);
				p.Data.State.Blend = packed & 0x00ffffff;
				const double scale = SourceParticle2DCurveAt(s, 3, ratio);
				p.Data.State.Scale = {p.DrawScale[0] * scale, p.DrawScale[1] * scale};
				if (p.HistoryIndex) {
					const size_t i = p.HistoryIndex - 1;
					p.BlendHistory[i] = packed;
					p.AlphaHistory[i] = p.Data.State.Alpha;
					p.ScaleXHistory[i] = p.Data.State.Scale[0];
					p.ScaleYHistory[i] = p.Data.State.Scale[1];
				}
			}
			return true;
		}
		bool Run(
			NodeContext &context, const SourceParticle2DControls &c, SourceParticle2DState &s, int64_t frame
		) {
			bool spawn = c.SpawnType == 0
							 ? c.SpawnDelay != 0 && std::fmod(double(frame), double(c.SpawnDelay)) == 0
						 : c.SpawnType == 1 ? frame >= c.SpawnDelay && frame < c.SpawnDelay + c.BurstDuration
											: c.Trigger;
			if (c.Spawn && spawn && !Spawn(context, c, s)) return false;
			for (auto &p : s.Slots) {
				if (p.Data.State.Active && !Step(context, s, p)) return false;
				++p.TrailLife;
			}
			return true;
		}
	}
	double SourceParticle2DCurveAt(const SourceParticle2DState &s, size_t curve, double ratio) {
		if (curve >= s.Curves.size() || s.Curves[curve].empty() || !std::isfinite(ratio)) return 0;
		const auto &map = s.Curves[curve];
		const double index = std::clamp(ratio, 0., 1.) * double(map.size() - 1);
		return map[size_t(index)];
	}
	bool BeginSourceParticle2DState(
		NodeContext &context, const SourceParticle2DControls &c, SourceParticle2DState &s
	) try {
		using namespace source_particle2d_state;
		ENGINE_PROFILE("imagegraph.particle2d.begin");
		if (!c.PoolCapacity || c.PoolCapacity > 4096 || !c.TotalFrames ||
			c.TotalFrames > Limits::MaximumRangeFrames)
			return context.Fail(
				Status::LimitExceeded, "Particle pool or curve frame count exceeds bounds", "data"
			);
		const uint64_t prerender =
			c.Loop ? c.PreRender == -1 ? c.TotalFrames : uint64_t(std::max<int64_t>(0, c.PreRender)) : 0;
		if (prerender > MAX_WORK / std::max<size_t>(context.ProcessorCount, 1) / c.PoolCapacity / 512)
			return context.Fail(
				Status::LimitExceeded, "Particle prerender exceeds whole processor work bounds", "data"
			);
		auto charge = context.ReserveWorkspace(
			c.PoolCapacity * (sizeof(SourceParticle2DSlot) +
							  std::max(context.Authored.Id.size(), std::string{}.capacity())) +
				7 * (c.TotalFrames + 1) * sizeof(double),
			"data"
		);
		if (!charge) return false;
		s.Charge = std::move(*charge);
		s.Slots.resize(c.PoolCapacity);
		for (size_t i = 0; i < s.Slots.size(); ++i) {
			s.Slots[i].Data.OriginNodeId = context.Authored.Id;
			s.Slots[i].Data.OriginProcessorRow = context.ProcessorRow;
			s.Slots[i].Data.State.SourceSlot = i;
		}
		const std::array<const Curve *, 7> curves{
			c.SpeedCurve,
			c.RotationCurve,
			c.TargetCurve,
			c.ScaleCurve,
			c.AlphaCurve,
			c.PathSpeedCurve,
			c.PathDeviationCurve
		};
		for (size_t map = 0; map < curves.size(); ++map) {
			s.Curves[map].resize(c.TotalFrames + 1);
			for (size_t i = 0; i <= c.TotalFrames; ++i) {
				const double ratio = double(i) / c.TotalFrames;
				s.Curves[map][i] = curves[map] ? EvalCurveX(*curves[map], ratio, .00001)
											   : (map == 2 || map == 5 ? ratio : 1);
			}
		}
		const std::array<Vector2, 6> wiggles{
			c.PositionWiggle,
			c.PositionWiggle,
			c.ScaleWiggle,
			c.ScaleWiggle,
			c.RotationWiggle,
			c.DirectionWiggle
		};
		for (size_t map = 0; map < wiggles.size(); ++map) {
			s.WiggleAmplitudes[map] = wiggles[map].X;
			for (size_t i = 0; i < 1000; ++i) {
				const double x = i * wiggles[map].Y + uint32_t(c.Seed + (map + 1) * 10);
				Random r(uint32_t(int64_t(std::floor(x))));
				const double a = r.Unit(), b = r.Unit(), fraction = x - std::floor(x);
				s.WiggleMaps[map][i] = (a + (b - a) * fraction * fraction * (3 - 2 * fraction)) * 2 - 1;
			}
		}
		s.Seed = c.Seed;
		s.Initialized = true;
		for (int64_t frame = int64_t(c.TotalFrames) - int64_t(prerender); frame < int64_t(c.TotalFrames);
			 ++frame)
			if (!Run(context, c, s, frame)) return false;
		s.Seed = c.Seed;
		s.Frame = -1;
		return ValidateSourceParticle2DState(context, s);
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Particle initialization allocation failed", "data");
	}
	bool AdvanceSourceParticle2DState(
		NodeContext &context, const SourceParticle2DControls &c, SourceParticle2DState &s, int64_t frame
	) try {
		using namespace source_particle2d_state;
		ENGINE_PROFILE("imagegraph.particle2d.step");
		if (!s.Initialized || frame < 0 || frame > int64_t(Limits::MaximumTick))
			return Fail(context, "Particle timeline frame is invalid");
		if (frame == s.Frame) return true;
		if (frame != s.Frame + 1)
			return context.Fail(
				Status::UnsupportedExecution, "Particle requires consecutive owned timeline frames", "data"
			);
		if (s.Slots.size() != c.PoolCapacity)
			return context.Fail(
				Status::UnsupportedExecution, "Particle pool resize requires a reset", "data"
			);
		if (c.PoolCapacity * (512 + c.PathSampleWork * 2) >
			MAX_WORK / std::max<size_t>(context.ProcessorCount, 1))
			return context.Fail(
				Status::LimitExceeded, "Particle step exceeds whole processor work bounds", "data"
			);
		if (!Run(context, c, s, frame)) return false;
		s.Frame = frame;
		return ValidateSourceParticle2DState(context, s);
	} catch (const std::bad_alloc &) {
		return context.Fail(Status::LimitExceeded, "Particle step allocation failed", "data");
	}
	bool ValidateSourceParticle2DState(NodeContext &context, const SourceParticle2DState &s) {
		using namespace source_particle2d_state;
		if (!s.Initialized || s.Frame < -1 || s.Frame > int64_t(Limits::MaximumTick) || s.Slots.empty() ||
			s.Slots.size() > 4096 || s.Runner >= s.Slots.size() || s.SpawnIndex >= s.Slots.size())
			return Fail(context, "Particle replay pool is malformed");
		for (const auto &map : s.Curves) {
			if (map.empty() || map.size() > Limits::MaximumRangeFrames + 1)
				return Fail(context, "Particle replay curve map is malformed");
			for (double value : map)
				if (!std::isfinite(value)) return Fail(context, "Particle replay curve sample is nonfinite");
		}
		std::array<bool, 4096> sourceSlots{};
		for (const auto &p : s.Slots) {
			if (p.Data.State.SourceSlot >= s.Slots.size() || sourceSlots[p.Data.State.SourceSlot] ||
				p.Data.OriginNodeId != context.Authored.Id ||
				p.Data.OriginProcessorRow != context.ProcessorRow)
				return Fail(context, "Particle replay source slot identity is invalid");
			sourceSlots[p.Data.State.SourceSlot] = true;
			if (!std::isfinite(p.Life) || !std::isfinite(p.LifeTotal) || p.Life < -2 ||
				p.Life > p.LifeTotal || p.HistoryIndex > p.Data.State.XHistory.size() ||
				p.ScaleXHistory.size() != p.Data.State.XHistory.size() ||
				p.ScaleYHistory.size() != p.ScaleXHistory.size() ||
				p.BlendHistory.size() != p.ScaleXHistory.size() ||
				p.AlphaHistory.size() != p.ScaleXHistory.size())
				return Fail(context, "Particle replay slot history is malformed");
			for (double v : p.Data.State.Position)
				if (!std::isfinite(v)) return Fail(context, "Particle position is nonfinite");
			for (double v : p.Velocity)
				if (!std::isfinite(v)) return Fail(context, "Particle velocity is nonfinite");
			for (const auto *values :
				 {&p.Start,
				  &p.Previous,
				  &p.Draw,
				  &p.InitialVelocity,
				  &p.BaseScale,
				  &p.DrawScale,
				  &p.PathRange,
				  &p.Data.State.Scale})
				for (double v : *values)
					if (!std::isfinite(v)) return Fail(context, "Particle replay transform is nonfinite");
			for (double v :
				 {p.BaseRotation,	 p.DrawRotation, p.RotationSpeed,  p.TargetAngle,
				  p.SnapRotation,	 p.Alpha,		 p.Acceleration,   p.Friction,
				  p.GravityX,		 p.GravityY,	 p.Turning,		   p.TurnScale,
				  p.GroundY,		 p.Bounce,		 p.GroundFriction, p.AnimationSpeed,
				  p.PathDeviation,	 p.Direction,	 p.DirectionSpeed, p.Data.State.RotationDegrees,
				  p.Data.State.Alpha})
				if (!std::isfinite(v)) return Fail(context, "Particle replay scalar is nonfinite");
			if (p.SpriteSelection < 0 || p.SpriteSelection > 3 || p.AnimationEnd < 0 || p.AnimationEnd > 2 ||
				p.RotationType < 0 || p.RotationType > 2 || p.RenderType < 0 || p.RenderType > 1 ||
				p.Wrap < 0 || p.Wrap > 3 || p.LifeTotal < 0 || p.LifeTotal > Limits::MaximumRangeFrames ||
				p.Data.State.Blend > 0xffffff ||
				(p.Data.State.SpriteSlot && *p.Data.State.SpriteSlot >= p.Data.Sprites.size()))
				return Fail(context, "Particle replay selector or lifespan is invalid");
			for (const auto *values : {&p.ScaleXHistory, &p.ScaleYHistory, &p.AlphaHistory})
				for (double v : *values)
					if (!std::isfinite(v)) return Fail(context, "Particle replay history is nonfinite");
			for (const auto *history : {&p.Data.State.XHistory, &p.Data.State.YHistory})
				for (const auto &v : *history)
					if (v && !std::isfinite(*v))
						return Fail(context, "Particle replay coordinate history is nonfinite");
			for (const auto &sprite : p.Data.Sprites)
				if (!ValidSurfaceLayout(sprite, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
					!FiniteSurfaceSamples(sprite))
					return Fail(context, "Particle replay sprite is malformed");
			for (const auto &key : p.LifetimeColour.Keys)
				if (!std::isfinite(key.Time)) return Fail(context, "Particle replay gradient is malformed");
			if (p.AtlasRect)
				for (double v : {p.AtlasRect->X, p.AtlasRect->Y, p.AtlasRect->Z, p.AtlasRect->W})
					if (!std::isfinite(v))
						return Fail(context, "Particle replay atlas rectangle is nonfinite");
		}
		for (const auto &map : s.WiggleMaps)
			for (double v : map)
				if (!std::isfinite(v)) return Fail(context, "Particle wiggle map is nonfinite");
		for (double v : s.WiggleAmplitudes)
			if (!std::isfinite(v)) return Fail(context, "Particle wiggle amplitude is nonfinite");
		if (s.LastSurface &&
			(!ValidSurfaceLayout(*s.LastSurface, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
			 !FiniteSurfaceSamples(*s.LastSurface)))
			return Fail(context, "Particle retained surface is malformed");
		return true;
	}
}
