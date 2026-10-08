#include "../SourceGradient.hpp"
#include "../SourceSafeDraw.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"
#include "SourceParticle2DRaster.hpp"
#include "SourceParticle2DRecipe.hpp"
#include "SourceParticle2DReplay.hpp"
#include "SourceRefractClean.hpp"

#include <new>
#include <stdexcept>

// Source algorithms: Copyright (c) 2023 Tanasart, MIT License.
// See docs/pixel-composer-m0/PixelComposer-LICENSE.txt.
namespace engine::imagegraph::detail {
	namespace particle2d_draw {
		constexpr uint64_t WORK_LIMIT = 64000000;
		struct Vertex {
			Vector2 Position, Uv;
		};
		struct Primitive {
			std::array<Vertex, 3> Vertices;
			const Image *Sprite = nullptr;
			Rgba Colour{1, 1, 1, 1};
		};
		double Cross(Vector2 a, Vector2 b, Vector2 p) {
			return (b.X - a.X) * (p.Y - a.Y) - (b.Y - a.Y) * (p.X - a.X);
		}
		bool Edge(Vector2 a, Vector2 b) {
			return b.Y < a.Y || (b.Y == a.Y && b.X > a.X);
		}
		std::array<uint32_t, 4> Bounds(const Primitive &p, uint32_t width, uint32_t height) {
			const auto &v = p.Vertices;
			return {
				uint32_t(
					std::clamp(
						std::floor(std::min({v[0].Position.X, v[1].Position.X, v[2].Position.X})),
						0.,
						double(width)
					)
				),
				uint32_t(
					std::clamp(
						std::floor(std::min({v[0].Position.Y, v[1].Position.Y, v[2].Position.Y})),
						0.,
						double(height)
					)
				),
				uint32_t(
					std::clamp(
						std::ceil(std::max({v[0].Position.X, v[1].Position.X, v[2].Position.X})),
						0.,
						double(width)
					)
				),
				uint32_t(
					std::clamp(
						std::ceil(std::max({v[0].Position.Y, v[1].Position.Y, v[2].Position.Y})),
						0.,
						double(height)
					)
				)
			};
		}
		bool Blend(Image &out, uint32_t x, uint32_t y, Rgba src, int mode) {
			const Rgba dest = ReadPixel(out, x, y);
			Rgba result{};
			for (size_t ch = 0; ch < 4; ++ch) {
				if (mode == 0)
					result[ch] = src[ch] * src[3] + dest[ch] * (1 - src[3]);
				else if (mode == 1)
					result[ch] = src[ch] + (ch == 3 ? dest[ch] : dest[ch] * (1 - src[3]));
				else if (mode == 2)
					result[ch] = src[ch] * src[3] + dest[ch];
				else if (mode == 3)
					result[ch] = std::max(src[ch], dest[ch]);
				else
					result[ch] =
						ch == 3 ? src[ch] * src[3] + dest[ch] * (1 - src[3]) : std::min(src[ch], dest[ch]);
			}
			return WritePixel(out, x, y, result);
		}
		Rgba ColourOf(uint32_t packed, double alpha) {
			return {
				(packed & 255) / 255., ((packed >> 8) & 255) / 255., ((packed >> 16) & 255) / 255., alpha
			};
		}
		bool Colour(NodeContext &c, SourceParticle2DState &state, SourceParticle2DSlot &slot) {
			const double ratio = std::clamp(1 - slot.Life / slot.LifeTotal, 0., 1.);
			auto colour = SourceCachedGradient(slot.LifetimeColour, ratio);
			if (!colour) return c.Fail(Status::InvalidValue, "Particle lifetime colour is nonfinite");
			const uint8_t channels[]{colour->Red, colour->Green, colour->Blue, colour->Alpha};
			uint32_t packed = 0;
			for (size_t ch = 0; ch < 4; ++ch) {
				const uint32_t product = ((slot.Blend >> (ch * 8)) & 255) * channels[ch];
				packed |= ((product + 1 + (product >> 8)) >> 8) << (ch * 8);
			}
			slot.Data.State.Blend = packed & 0xffffff;
			slot.Data.State.Alpha =
				slot.Alpha * SourceParticle2DCurveAt(state, 4, ratio) * ((packed >> 24) & 255) / 255.;
			const double scale = SourceParticle2DCurveAt(state, 3, ratio);
			slot.Data.State.Scale = {slot.DrawScale[0] * scale, slot.DrawScale[1] * scale};
			return (std::isfinite(slot.Data.State.Alpha) && std::isfinite(slot.Data.State.Scale[0]) &&
					std::isfinite(slot.Data.State.Scale[1])) ||
				   c.Fail(Status::InvalidValue, "Particle draw colour or scale is nonfinite");
		}
		bool Select(NodeContext &c, SourceParticle2DSlot &slot) {
			auto &part = slot.Data.State;
			const size_t count = slot.Data.Sprites.size();
			if (!count) {
				part.SpriteSlot.reset();
				return true;
			}
			if (slot.SpriteSelection == 2) {
				const double progress = slot.LifeTotal - slot.Life;
				const double value = slot.StretchAnimation
										 ? progress / slot.LifeTotal * slot.AnimationSpeed * (count - 1)
										 : progress * slot.AnimationSpeed;
				const double index = std::abs(SourceRoundEven(value));
				if (!std::isfinite(index) || index > double(Limits::MaximumTick))
					return c.Fail(Status::LimitExceeded, "Particle animation index exceeds bounds");
				const uint64_t i = uint64_t(index);
				if (slot.AnimationEnd == 2 && i >= count) {
					part.Active = false;
					return true;
				}
				if (slot.AnimationEnd == 1) {
					const size_t cycle = (count - 1) * 2 + 1, ping = size_t(i % cycle);
					part.SpriteSlot = uint32_t(ping >= count ? (count - 1) * 2 - ping : ping);
				} else
					part.SpriteSlot = uint32_t(i % count);
			} else if (slot.SpriteSelection == 3) {
				part.SpriteSlot = uint32_t(
					std::clamp(SourceRoundEven(std::min(part.Scale[0], part.Scale[1])), 0., double(count - 1))
				);
				part.Scale = {1, 1};
			}
			return !part.SpriteSlot || *part.SpriteSlot < count ||
				   c.Fail(Status::InvalidValue, "Particle sprite index is invalid");
		}
		bool Triangle(
			NodeContext &c,
			std::vector<Primitive> &list,
			Primitive primitive,
			uint32_t width,
			uint32_t height,
			uint64_t &work
		) {
			for (const auto &v : primitive.Vertices)
				for (double n : {v.Position.X, v.Position.Y})
					if (!std::isfinite(n))
						return c.Fail(Status::InvalidValue, "Particle draw geometry is nonfinite");
			if (!std::isfinite(Cross(
					primitive.Vertices[0].Position,
					primitive.Vertices[1].Position,
					primitive.Vertices[2].Position
				)))
				return c.Fail(Status::InvalidValue, "Particle triangle area is nonfinite");
			for (double channel : primitive.Colour)
				if (!std::isfinite(channel))
					return c.Fail(Status::InvalidValue, "Particle draw colour is nonfinite");
			const auto b = Bounds(primitive, width, height);
			const auto interpolation = ReadSampler(c).Interpolation;
			const uint64_t pixelWork =
				primitive.Sprite && (interpolation == 4 || interpolation == 6) ? 2048 : 128;
			const uint64_t cost = uint64_t(b[2] - b[0]) * (b[3] - b[1]) * pixelWork + 128;
			if (cost > WORK_LIMIT / std::max<size_t>(c.ProcessorCount, 1) - work)
				return c.Fail(Status::LimitExceeded, "Particle raster exceeds whole processor work budget");
			work += cost;
			list.push_back(std::move(primitive));
			return true;
		}
		bool Quad(
			NodeContext &c,
			std::vector<Primitive> &list,
			Vector2 a,
			Vector2 b,
			Vector2 d,
			Vector2 e,
			const Image *sprite,
			Rgba colour,
			uint32_t width,
			uint32_t height,
			uint64_t &work
		) {
			return Triangle(
					   c,
					   list,
					   {{Vertex{a, {0, 0}}, Vertex{b, {1, 0}}, Vertex{d, {0, 1}}}, sprite, colour},
					   width,
					   height,
					   work
				   ) &&
				   Triangle(
					   c,
					   list,
					   {{Vertex{b, {1, 0}}, Vertex{d, {0, 1}}, Vertex{e, {1, 1}}}, sprite, colour},
					   width,
					   height,
					   work
				   );
		}
		bool Prepare(
			NodeContext &c,
			const SourceParticle2DControls &controls,
			SourceParticle2DState &state,
			std::vector<Primitive> &list,
			uint32_t width,
			uint32_t height,
			uint64_t &work
		) {
			// Native equal-Y ordering uses source slot identity rather than library-dependent sort ties.
			if (controls.SortY)
				std::sort(state.Slots.begin(), state.Slots.end(), [](const auto &a, const auto &b) {
					if (a.Data.State.Position[1] == b.Data.State.Position[1])
						return a.Data.State.SourceSlot < b.Data.State.SourceSlot;
					return a.Data.State.Position[1] < b.Data.State.Position[1];
				});
			for (auto &slot : state.Slots) {
				if (controls.RenderType == 1) {
					const size_t end = std::min(slot.HistoryIndex, size_t(std::max(0., slot.LifeTotal))),
								 start = slot.TrailLife > size_t(controls.LineLife)
											 ? slot.TrailLife - size_t(controls.LineLife)
											 : 0;
					const auto &p = slot.Data.State;
					if (end > p.XHistory.size() || end > p.YHistory.size() ||
						end > slot.ScaleXHistory.size() || end > slot.AlphaHistory.size() ||
						end > slot.BlendHistory.size())
						return c.Fail(Status::InvalidValue, "Particle trail history is incomplete");
					for (size_t i = start + 1; i < end; ++i) {
						if (!p.XHistory[i - 1] || !p.YHistory[i - 1] || !p.XHistory[i] || !p.YHistory[i])
							return c.Fail(Status::InvalidValue, "Particle trail coordinate is undefined");
						const Vector2 a{*p.XHistory[i - 1], *p.YHistory[i - 1]},
							b{*p.XHistory[i], *p.YHistory[i]};
						const double len = std::hypot(b.X - a.X, b.Y - a.Y);
						if (len == 0) continue;
						const Vector2 n{-(b.Y - a.Y) / (2 * len), (b.X - a.X) / (2 * len)};
						const double s0 = slot.ScaleXHistory[i - 1], s1 = slot.ScaleXHistory[i];
						if (!Quad(
								c,
								list,
								{a.X + n.X * s0, a.Y + n.Y * s0},
								{b.X + n.X * s1, b.Y + n.Y * s1},
								{a.X - n.X * s0, a.Y - n.Y * s0},
								{b.X - n.X * s1, b.Y - n.Y * s1},
								nullptr,
								ColourOf(slot.BlendHistory[i], slot.AlphaHistory[i]),
								width,
								height,
								work
							))
							return false;
					}
					continue;
				}
				if (!slot.Data.State.Active || slot.LifeTotal <= 0) continue;
				if (!Colour(c, state, slot) || !Select(c, slot)) return false;
				if (!slot.Data.State.Active) continue;
				auto &part = slot.Data.State;
				const auto colour = ColourOf(part.Blend, part.Alpha);
				const double px = slot.DrawDefined ? slot.Draw[0] : 0,
							 py = slot.DrawDefined ? slot.Draw[1] : 0;
				if (!part.SpriteSlot) {
					const size_t first = list.size();
					const double rotation =
						slot.SnapRotation == 0
							? slot.DrawRotation
							: SourceRoundEven(slot.DrawRotation / slot.SnapRotation) * slot.SnapRotation;
					const double angle = -rotation * std::numbers::pi / 180, co = std::cos(angle),
								 si = std::sin(angle);
					Vector2 fallbackOrigin{
						px - (part.Scale[0] * co - part.Scale[1] * si) / 2,
						py - (part.Scale[0] * si + part.Scale[1] * co) / 2
					};
					if (controls.RoundPosition) {
						fallbackOrigin.X = SourceRoundEven(fallbackOrigin.X);
						fallbackOrigin.Y = SourceRoundEven(fallbackOrigin.Y);
					}

					const double diameter = SourceRoundEven(std::min(part.Scale[0], part.Scale[1]));
					if (diameter == 0) continue;
					const Vector2 center{
						controls.RoundPosition ? SourceRoundEven(px) : px,
						controls.RoundPosition ? SourceRoundEven(py) : py
					};
					const auto point = [&](double dx, double dy) {
						const double x = std::floor(center.X + dx), y = std::floor(center.Y + dy);
						return Quad(
							c,
							list,
							{x, y},
							{x + 1, y},
							{x, y + 1},
							{x + 1, y + 1},
							nullptr,
							colour,
							width,
							height,
							work
						);
					};
					if (diameter == 1) {
						if (!point(0, 0)) return false;
					} else if (diameter == 2) {
						for (int y = 0; y < 2; ++y)
							for (int x = 0; x < 2; ++x)
								if (!point(x, y)) return false;
					} else if (diameter == 3) {
						for (const Vector2 d :
							 std::array<Vector2, 5>{{{0, 0}, {-1, 0}, {1, 0}, {0, 1}, {0, -1}}})
							if (!point(d.X, d.Y)) return false;
					} else
						for (size_t i = 0; i < 32; ++i) {
							const double a = i * 2 * std::numbers::pi / 32,
										 b = (i + 1) * 2 * std::numbers::pi / 32;
							const double r = diameter / 2;
							if (!Triangle(
									c,
									list,
									{{Vertex{center, {}},
									  Vertex{{center.X + std::cos(a) * r, center.Y + std::sin(a) * r}, {}},
									  Vertex{{center.X + std::cos(b) * r, center.Y + std::sin(b) * r}, {}}},
									 nullptr,
									 colour},
									width,
									height,
									work
								))
								return false;
						}
					const size_t last = list.size();
					const auto copy = [&](double dx, double dy) {
						for (size_t i = first; i < last; ++i) {
							auto primitive = list[i];
							for (auto &vertex : primitive.Vertices) {
								vertex.Position.X += dx;
								vertex.Position.Y += dy;
							}
							if (!Triangle(c, list, std::move(primitive), width, height, work)) return false;
						}
						return true;
					};
					// The fallback branch uses the earlier transformed bounds but copies in the opposite
					// directions.
					if (slot.Wrap & 1) {
						if (fallbackOrigin.X - part.Scale[0] < 0 && !copy(-double(width), 0)) return false;
						if (fallbackOrigin.X + part.Scale[0] > width && !copy(width, 0)) return false;
					}
					if (slot.Wrap & 2) {
						if (fallbackOrigin.Y - part.Scale[1] < 0 && !copy(0, -double(height))) return false;
						if (fallbackOrigin.Y + part.Scale[1] > height && !copy(0, height)) return false;
					}

					continue;
				}
				const auto &sprite = slot.Data.Sprites[*part.SpriteSlot];
				const double rotation =
					slot.SnapRotation == 0
						? slot.DrawRotation
						: SourceRoundEven(slot.DrawRotation / slot.SnapRotation) * slot.SnapRotation;
				const double angle = -rotation * std::numbers::pi / 180, co = std::cos(angle),
							 si = std::sin(angle);
				const Vector2 u{sprite.Width * part.Scale[0] * co, sprite.Width * part.Scale[0] * si},
					v{-sprite.Height * part.Scale[1] * si, sprite.Height * part.Scale[1] * co};
				Vector2 origin{px - (u.X + v.X) / 2, py - (u.Y + v.Y) / 2};
				if (controls.RoundPosition) {
					origin.X = SourceRoundEven(origin.X);
					origin.Y = SourceRoundEven(origin.Y);
				}
				const auto draw = [&](double dx, double dy) {
					const Vector2 a{origin.X + dx, origin.Y + dy};
					return Quad(
						c,
						list,
						a,
						{a.X + u.X, a.Y + u.Y},
						{a.X + v.X, a.Y + v.Y},
						{a.X + u.X + v.X, a.Y + u.Y + v.Y},
						&sprite,
						colour,
						width,
						height,
						work
					);
				};
				if (!draw(0, 0)) return false;
				if (slot.Wrap & 1) {
					if (origin.X - sprite.Width * part.Scale[0] < 0 && !draw(width, 0)) return false;
					if (origin.X + sprite.Width * part.Scale[0] > width && !draw(-double(width), 0))
						return false;
				}
				if (slot.Wrap & 2) {
					if (origin.Y - sprite.Height * part.Scale[1] < 0 && !draw(0, height)) return false;
					if (origin.Y + sprite.Height * part.Scale[1] > height && !draw(0, -double(height)))
						return false;
				}
			}
			return true;
		}
		bool Render(
			NodeContext &c,
			const SourceParticle2DControls &controls,
			SourceParticle2DState &state,
			Image &out,
			const std::vector<Primitive> &primitives
		) {
			ENGINE_PROFILE("imagegraph.source.particle.raster");
			if (controls.BlendMode == 4)
				for (uint32_t y = 0; y < out.Height; ++y)
					for (uint32_t x = 0; x < out.Width; ++x)
						if (!WritePixel(out, x, y, {1, 1, 1, 0}))
							return c.Fail(Status::InvalidValue, "Particle minimum clear is nonfinite");
			if (controls.Background)
				for (uint32_t y = 0; y < out.Height; ++y)
					for (uint32_t x = 0; x < out.Width; ++x)
						if (!Blend(out, x, y, SourceSafeDrawPixel(*controls.Background, x, y), 0))
							return c.Fail(Status::InvalidValue, "Particle background blend is nonfinite");
			const auto sampler = ReadSampler(c);
			for (auto primitive : primitives) {
				auto &v = primitive.Vertices;
				double area = Cross(v[0].Position, v[1].Position, v[2].Position);
				if (!std::isfinite(area))
					return c.Fail(Status::InvalidValue, "Particle triangle area is nonfinite");
				if (area == 0) continue;
				if (area < 0) {
					std::swap(v[1], v[2]);
					area = -area;
				}
				const auto b = Bounds(primitive, out.Width, out.Height);
				for (uint32_t y = b[1]; y < b[3]; ++y)
					for (uint32_t x = b[0]; x < b[2]; ++x) {
						const Vector2 p{x + .5, y + .5};
						const double wa = Cross(v[1].Position, v[2].Position, p),
									 wb = Cross(v[2].Position, v[0].Position, p),
									 wc = Cross(v[0].Position, v[1].Position, p);
						if (wa < 0 || (wa == 0 && !Edge(v[1].Position, v[2].Position)) || wb < 0 ||
							(wb == 0 && !Edge(v[2].Position, v[0].Position)) || wc < 0 ||
							(wc == 0 && !Edge(v[0].Position, v[1].Position)))
							continue;
						Rgba colour = primitive.Colour;
						if (primitive.Sprite) {
							// sh_sample shares the extended sampler with sh_refract, including CleanEdge.
							auto texel = source_refract_clean::Sample(
								*primitive.Sprite,
								(wa * v[0].Uv.X + wb * v[1].Uv.X + wc * v[2].Uv.X) / area,
								(wa * v[0].Uv.Y + wb * v[1].Uv.Y + wc * v[2].Uv.Y) / area,
								sampler,
								{double(out.Width), double(out.Height)}
							);
							if ((primitive.Sprite->Format == SurfaceFormat::R8Unorm ||
								 primitive.Sprite->Format == SurfaceFormat::R16Float ||
								 primitive.Sprite->Format == SurfaceFormat::R32Float))
								texel = {texel[0], texel[0], texel[0], 1};
							for (size_t ch = 0; ch < 4; ++ch)
								colour[ch] *= texel[ch];
						}
						if (!Blend(out, x, y, colour, controls.BlendMode))
							return c.Fail(Status::InvalidValue, "Particle pixel blend is nonfinite");
					}
			}
			(void)state;
			return true;
		}
	}
	bool DrawSourceParticle2D(
		NodeContext &c, const SourceParticle2DControls &controls, SourceParticle2DState &state
	) {
		ENGINE_PROFILE("imagegraph.source.particle.admission");
		if (c.Request.RequireSourceGpuRasterCoverage)
			return c.Fail(
				Status::UnsupportedExecution,
				"Particle native raster requires source GPU observations for exact coverage"
			);
		const Image *inputSprite = controls.Sprites.empty() ? nullptr : &controls.Sprites.front();
		uint32_t width = 0, height = 0;
		if (controls.Background) {
			width = controls.Background->Width;
			height = controls.Background->Height;
		} else if (!source2d::ResolveGeneratorDimensions(c, inputSprite, width, height))
			return false;
		const auto format = ResolveProcessorSurfaceFormat(c, inputSprite);
		if (!format) return false;
		const auto layout = CheckedSurfaceLayout(width, height, *format, Limits::MaximumOutputBytes);
		if (!layout) return c.Fail(Status::LimitExceeded, "Particle output dimensions exceed bounds");
		uint64_t primitiveCount = 0;
		for (const auto &slot : state.Slots)
			primitiveCount +=
				controls.RenderType == 1 ? slot.HistoryIndex * 2 : (slot.Data.State.Active ? 160 : 0);
		if (primitiveCount > Limits::MaximumArrayElements * 64)
			return c.Fail(Status::LimitExceeded, "Particle primitive inventory exceeds bounds");
		auto charge = c.ReserveWorkspace(primitiveCount * sizeof(particle2d_draw::Primitive));
		if (!charge) return false;
		std::vector<particle2d_draw::Primitive> list;
		list.reserve(size_t(primitiveCount));
		uint64_t work = uint64_t(width) * height * 32;
		if (work > particle2d_draw::WORK_LIMIT / std::max<size_t>(c.ProcessorCount, 1))
			return c.Fail(Status::LimitExceeded, "Particle background exceeds whole processor work budget");
		if (!particle2d_draw::Prepare(c, controls, state, list, width, height, work)) return false;
		if (layout->Bytes > c.AvailableBytes() / std::max<size_t>(c.ProcessorCount, 1) / 2)
			return c.Fail(
				Status::LimitExceeded,
				"Particle output and retained surface exceed whole processor byte budget"
			);
		auto retention = c.ReserveWorkspace(layout->Bytes);
		if (!retention) return false;
		auto *out = c.NewImage("surface_out", width, height, *format);
		if (!out) return false;
		if (!particle2d_draw::Render(c, controls, state, *out, list)) return false;
		state.LastSurface = *out;
		state.LastSurfaceCharge = std::move(*retention);
		core::Metrics::Count("imagegraph.particle.raster_work", work);
		core::Metrics::Count("imagegraph.particle.primitives", list.size());
		return true;
	}
	namespace {
		bool ExecuteParticle2DImpl(NodeContext &c) try {
			ENGINE_PROFILE("imagegraph.source.particle");
			if (c.Request.Subframe != 0 || c.Request.Tick > Limits::MaximumTick)
				return c.Fail(
					Status::UnsupportedExecution, "Particle requires bounded integer timeline frames"
				);
			const int64_t frame =
				c.Request.NegativeFrame ? -int64_t(c.Request.Tick) : int64_t(c.Request.Tick);
			SourceParticle2DPrepared prepared;
			if (!PrepareSourceParticle2DControls(c, prepared)) return false;
			SourceParticle2DState state;
			const DataReplayState *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			const DataReplayEntry *previous = nullptr;
			if (owner) {
				if (owner->Entries.size() > Limits::MaximumArrayElements)
					return c.Fail(Status::LimitExceeded, "Particle replay row count exceeds bounds");
				for (const auto &entry : owner->Entries)
					if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) {
						if (previous)
							return c.Fail(Status::InvalidValue, "Particle replay row is duplicated");
						previous = &entry;
					}
			}
			if (previous) {
				if (!previous->Initialized || previous->Subframe != 0 ||
					previous->Tick > Limits::MaximumTick || previous->Values.size() != 1 ||
					previous->Values[0].Frame != previous->Tick)
					return c.Fail(Status::InvalidValue, "Particle replay row is malformed");
				const auto *receipt = std::get_if<StructValue>(&previous->Values[0].Data);
				if (!receipt || !DecodeSourceParticle2DReceipt(c, *receipt, state))
					return c.FailureCode == Status::Ok
							   ? c.Fail(Status::InvalidValue, "Particle replay requires constructor receipt")
							   : false;
				if (state.Frame !=
					(previous->NegativeFrame ? -int64_t(previous->Tick) : int64_t(previous->Tick)))
					return c.Fail(Status::InvalidValue, "Particle replay frame identity differs");
			} else if (!BeginSourceParticle2DState(c, prepared.Controls, state))
				return false;
			if (!AdvanceSourceParticle2DState(c, prepared.Controls, state, frame)) return false;
			if (prepared.Controls.Render) {
				if (!DrawSourceParticle2D(c, prepared.Controls, state)) return false;
			} else if (state.LastSurface)
				c.SetValue("surface_out", SurfaceValue{*state.LastSurface});
			else
				c.SetValue("surface_out", int64_t{-4});
			if (c.FailureCode != Status::Ok) return false;
			uint64_t dataBytes = sizeof(ArrayValue) + state.Slots.size() * sizeof(ElementValue);
			for (const auto &slot : state.Slots) {
				const auto bytes = ParticleDataBytes<true>(slot.Data);
				if (bytes > Limits::MaximumEvaluationBytes - dataBytes)
					return c.Fail(Status::LimitExceeded, "Particle publication exceeds payload bounds");
				dataBytes += bytes;
			}
			auto charge = c.ReserveWorkspace(dataBytes);
			if (!charge) return false;
			ArrayValue data;
			data.ElementType = ValueType::Particle;
			data.Elements.reserve(state.Slots.size());
			for (const auto &slot : state.Slots) {
				ParticleValue part;
				part.Data.emplace(slot.Data);
				data.Elements.emplace_back(std::move(part));
			}
			StructValue encoded;
			AllocationReservation receiptCharge;
			if (!EncodeSourceParticle2DReceipt(c, state, encoded, receiptCharge)) return false;
			auto historyCharge = c.ReserveWorkspace(
				sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) + c.Authored.Id.size() + 64
			);
			if (!historyCharge) return false;
			DataReplayEntry update;
			update.NodeId = c.Authored.Id;
			update.ProcessorRow = c.ProcessorRow;
			update.Tick = c.Request.Tick;
			update.NegativeFrame = c.Request.NegativeFrame;
			update.Initialized = true;
			update.Values.push_back({c.Request.Tick, Value{std::move(encoded)}});
			if (!c.ReserveOutput(RetainedDataReplayEntryBytes(update) + sizeof(DataReplayEntry)))
				return false;
			c.SetValue("data", std::move(data));
			if (c.FailureCode != Status::Ok) return false;
			c.DataUpdates.push_back(std::move(update));
			return true;
		} catch (const std::bad_alloc &) {
			c.ClearOutputs();
			return c.Fail(Status::LimitExceeded, "Particle executor allocation failed");
		} catch (const std::length_error &) {
			c.ClearOutputs();
			return c.Fail(Status::LimitExceeded, "Particle executor container bounds exceeded");
		}
	}
	bool ExecuteParticle2D(NodeContext &c) {
		if (ExecuteParticle2DImpl(c)) return true;
		c.ClearOutputs();
		c.DataUpdates.clear();
		return false;
	}
	std::span<const ExecutorEntry> SourceParticle2DExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.particle", ExecuteParticle2D, true}};
		return entries;
	}
}
