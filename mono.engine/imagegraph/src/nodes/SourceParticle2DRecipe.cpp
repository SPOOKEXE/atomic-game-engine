#include "SourceParticle2DRecipe.hpp"

#include "../NodeExecutors.hpp"
#include "../SourceRandom.hpp"
#include "Path.hpp"
#include "Processor.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t MAXIMUM_WORK = 64000000;
		constexpr size_t MAXIMUM_SPRITES = 4096;
		constexpr size_t MAXIMUM_POISSON_POINTS = 1000000;

		bool Charge(NodeContext &c, SourceParticle2DPrepared &p, uint64_t bytes, std::string_view port) {
			auto lease = c.ReserveWorkspace(bytes, port);
			return lease && p.Charge.Merge(std::move(*lease));
		}

		double Number(const ElementValue &value, bool &ok) {
			if (const auto *v = std::get_if<double>(&value)) return *v;
			if (const auto *v = std::get_if<int64_t>(&value)) return double(*v);
			ok = false;
			return 0;
		}

		bool Choice(NodeContext &c, std::string_view id, int maximum, int &result) {
			const double value = c.SourceChoice(id);
			if (c.FailureCode != Status::Ok) return false;
			if (!std::isfinite(value) || value < 0 || value > maximum || std::trunc(value) != value)
				return c.Fail(Status::InvalidValue, "Particle 2D selector is outside its source choices", id);
			result = int(value);
			return true;
		}

		bool Rotation(NodeContext &c, std::string_view id, SourceParticle2DRotation &out) {
			const Value *value = c.Find(id);
			if (!value) return true;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array || !array->Nested.empty() || !array->Items.empty() ||
				(array->Elements.size() != 5 && array->Elements.size() != 6))
				return c.Fail(Status::TypeMismatch, "Particle 2D rotation requires five or six values", id);
			out.Count = array->Elements.size();
			for (size_t i = 0; i < out.Count; ++i) {
				bool valid = true;
				out.Values[i] = Number(array->Elements[i], valid);
				if (!valid || !std::isfinite(out.Values[i]))
					return c.Fail(Status::InvalidValue, "Particle 2D rotation values must be finite", id);
			}
			return true;
		}

		bool RotationRows(
			NodeContext &c,
			SourceParticle2DPrepared &p,
			std::string_view id,
			std::vector<SourceParticle2DRotation> &storage,
			SourceParticle2DRotation &first,
			std::span<const SourceParticle2DRotation> &rows
		) {
			const Value *value = c.Find(id);
			if (!value) return true;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array) return Rotation(c, id, first);
			if (!array->Items.empty()) {
				if (!array->Elements.empty() || !array->Nested.empty() ||
					array->Items.size() > Limits::MaximumArrayElements)
					return c.Fail(Status::LimitExceeded, "Particle rotation rows exceed bounds", id);
				if (!Charge(c, p, array->Items.size() * sizeof(SourceParticle2DRotation), id)) return false;
				storage.reserve(array->Items.size());
				for (const auto &item : array->Items) {
					const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
					if (!row || (row->size() != 5 && row->size() != 6))
						return c.Fail(
							Status::TypeMismatch, "Particle rotation row requires five or six values", id
						);
					SourceParticle2DRotation rotation;
					rotation.Count = row->size();
					for (size_t i = 0; i < rotation.Count; ++i) {
						const auto *leaf = std::get_if<ElementValue>(&(*row)[i].Data);
						bool valid = leaf != nullptr;
						if (leaf) rotation.Values[i] = Number(*leaf, valid);
						if (!valid || !std::isfinite(rotation.Values[i]))
							return c.Fail(Status::InvalidValue, "Particle rotation value must be finite", id);
					}
					storage.push_back(rotation);
				}
				first = storage.front();
				rows = storage;
				return true;
			}
			if (!array->Nested.empty()) {
				if (!array->Elements.empty() || array->Nested.size() > Limits::MaximumArrayElements)
					return c.Fail(Status::LimitExceeded, "Particle rotation rows exceed bounds", id);
				if (!Charge(c, p, array->Nested.size() * sizeof(SourceParticle2DRotation), id)) return false;
				storage.reserve(array->Nested.size());
				for (const auto &row : array->Nested) {
					if (row.size() != 5 && row.size() != 6)
						return c.Fail(
							Status::TypeMismatch, "Particle rotation row requires five or six values", id
						);
					SourceParticle2DRotation rotation;
					rotation.Count = row.size();
					for (size_t i = 0; i < rotation.Count; ++i) {
						bool valid = true;
						rotation.Values[i] = Number(row[i], valid);
						if (!valid || !std::isfinite(rotation.Values[i]))
							return c.Fail(Status::InvalidValue, "Particle rotation value must be finite", id);
					}
					storage.push_back(rotation);
				}
				first = storage.front();
				rows = storage;
				return true;
			}
			return Rotation(c, id, first);
		}

		bool CurveInput(NodeContext &c, std::string_view id, const Curve *&out) {
			const Value *value = c.Find(id);
			if (!value) return true;
			out = std::get_if<Curve>(value);
			return out || c.Fail(Status::TypeMismatch, "Particle curve input requires a curve", id);
		}
		bool GradientInput(NodeContext &c, std::string_view id, const Gradient *&out) {
			const Value *value = c.Find(id);
			if (!value) return true;
			out = std::get_if<Gradient>(value);
			return out || c.Fail(Status::TypeMismatch, "Particle colour input requires a gradient", id);
		}

		bool Vec2Range(NodeContext &c, std::string_view id, Vector2 &out) {
			const Value *value = c.Find(id);
			if (!value) return true;
			if (const auto *v = std::get_if<Vector2>(value))
				out = *v;
			else if (const auto *a = std::get_if<ArrayValue>(value);
					 a && a->Elements.size() == 2 && a->Nested.empty() && a->Items.empty()) {
				bool valid = true;
				out.X = Number(a->Elements[0], valid);
				out.Y = Number(a->Elements[1], valid);
				if (!valid) return c.Fail(Status::TypeMismatch, "Particle range requires two numbers", id);
			} else
				return c.Fail(Status::TypeMismatch, "Particle range requires two numbers", id);
			return std::isfinite(out.X) && std::isfinite(out.Y) ||
				   c.Fail(Status::InvalidValue, "Particle range values must be finite", id);
		}

		bool Vec2RangeAlias(
			NodeContext &c, std::string_view sourceId, std::string_view catalogueId, Vector2 &out
		) {
			return Vec2Range(c, c.Find(sourceId) ? sourceId : catalogueId, out);
		}

		bool Vec4Range(NodeContext &c, std::string_view id, Vector4 &out) {
			const Value *value = c.Find(id);
			if (!value) return true;
			if (const auto *v = std::get_if<Vector4>(value))
				out = *v;
			else if (const auto *a = std::get_if<ArrayValue>(value);
					 a && a->Elements.size() == 4 && a->Nested.empty() && a->Items.empty()) {
				double *dst[]{&out.X, &out.Y, &out.Z, &out.W};
				for (size_t i = 0; i < 4; ++i) {
					bool valid = true;
					*dst[i] = Number(a->Elements[i], valid);
					if (!valid)
						return c.Fail(Status::TypeMismatch, "Particle scale requires four numbers", id);
				}
			} else
				return c.Fail(Status::TypeMismatch, "Particle scale requires four numbers", id);
			return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z) &&
					   std::isfinite(out.W) ||
				   c.Fail(Status::InvalidValue, "Particle scale values must be finite", id);
		}

		bool PointItem(NodeContext &c, const SourceArrayItem &item, Vector2 &point, std::string_view id) {
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
				const auto *v = std::get_if<Vector2>(leaf);
				if (!v) return c.Fail(Status::TypeMismatch, "Particle spawn data needs vector2 points", id);
				point = *v;
			} else {
				const auto *row = std::get_if<std::vector<SourceArrayItem>>(&item.Data);
				if (!row || row->size() != 2)
					return c.Fail(Status::TypeMismatch, "Particle point row needs two numbers", id);
				double *dst[]{&point.X, &point.Y};
				for (size_t i = 0; i < 2; ++i) {
					const auto *leaf = std::get_if<ElementValue>(&(*row)[i].Data);
					bool valid = leaf != nullptr;
					if (leaf) *dst[i] = Number(*leaf, valid);
					if (!valid)
						return c.Fail(Status::TypeMismatch, "Particle point coordinate must be numeric", id);
				}
			}
			return std::isfinite(point.X) && std::isfinite(point.Y) ||
				   c.Fail(Status::InvalidValue, "Particle point must be finite", id);
		}

		bool CopyImage(NodeContext &c, SourceParticle2DPrepared &p, const Image &image, std::string_view id) {
			if (p.Sprites.size() >= MAXIMUM_SPRITES)
				return c.Fail(Status::LimitExceeded, "Particle sprite inventory exceeds image bounds", id);
			if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
				return c.Fail(Status::InvalidValue, "Particle sprite image is invalid", id);
			if (!Charge(c, p, sizeof(Image) + image.Pixels.capacity(), id)) return false;
			p.Sprites.push_back(image);
			p.SpriteAtlasRects.emplace_back();
			return true;
		}

		bool
		CopyAtlas(NodeContext &c, SourceParticle2DPrepared &p, const AtlasValue &atlas, std::string_view id) {
			if (!atlas.Data) return c.Fail(Status::InvalidValue, "Particle atlas has no surface", id);
			if (!CopyImage(c, p, atlas.Data->Surface.Data, id)) return false;
			if (atlas.Data->Kind == AtlasKind::SurfaceAtlas)
				p.SpriteAtlasRects.back() = Vector4{
					atlas.Data->Position.X,
					atlas.Data->Position.Y,
					atlas.Data->Dimension.X,
					atlas.Data->Dimension.Y
				};
			return true;
		}

		bool AppendSpriteItem(
			NodeContext &c, SourceParticle2DPrepared &p, const SourceArrayItem &item, size_t depth
		) {
			if (depth > Limits::MaximumArrayDepth)
				return c.Fail(
					Status::LimitExceeded, "Particle sprite array nesting exceeds bounds", "particle_sprite"
				);
			if (const auto *image = std::get_if<Image>(&item.Data))
				return CopyImage(c, p, *image, "particle_sprite");
			if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
				if (const auto *surface = std::get_if<SurfaceValue>(leaf))
					return CopyImage(c, p, surface->Data, "particle_sprite");
				if (const auto *atlas = std::get_if<AtlasValue>(leaf))
					return CopyAtlas(c, p, *atlas, "particle_sprite");
				if (std::holds_alternative<DynamicSurfaceValue>(*leaf))
					return c.Fail(
						Status::UnsupportedExecution,
						"Dynamic particle sprite parameters are not represented by this executor",
						"particle_sprite"
					);
				return c.Fail(
					Status::TypeMismatch, "Particle sprite array leaf is not a surface", "particle_sprite"
				);
			}
			for (const auto &child : std::get<std::vector<SourceArrayItem>>(item.Data))
				if (!AppendSpriteItem(c, p, child, depth + 1)) return false;
			return true;
		}

		bool AppendSpriteElement(NodeContext &c, SourceParticle2DPrepared &p, const ElementValue &leaf) {
			if (const auto *surface = std::get_if<SurfaceValue>(&leaf))
				return CopyImage(c, p, surface->Data, "particle_sprite");
			if (const auto *atlas = std::get_if<AtlasValue>(&leaf))
				return CopyAtlas(c, p, *atlas, "particle_sprite");
			if (std::holds_alternative<DynamicSurfaceValue>(leaf))
				return c.Fail(
					Status::UnsupportedExecution,
					"Dynamic particle sprite parameters are not represented by this executor",
					"particle_sprite"
				);
			return c.Fail(
				Status::TypeMismatch, "Particle sprite array leaf is not a surface", "particle_sprite"
			);
		}

		bool ReadSprites(NodeContext &c, SourceParticle2DPrepared &p) {
			auto &controls = p.Controls;
			const Value *value = c.Find("particle_sprite");
			if (value && std::holds_alternative<DynamicSurfaceValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution,
					"Dynamic particle sprite parameters are not represented by this executor",
					"particle_sprite"
				);
			const ImageArray *images = nullptr;
			for (const auto &[port, array] : c.ImageArrays)
				if (port == "particle_sprite") images = array;
			if (const auto *array = images) {
				controls.SpriteArray = true;
				controls.SpriteEmptyArray = array->Images.empty();
				if (array->Images.size() > MAXIMUM_SPRITES)
					return c.Fail(
						Status::LimitExceeded, "Particle sprite array exceeds image bounds", "particle_sprite"
					);
				std::function<bool(const ImageArrayItem &, size_t)> append = [&](const ImageArrayItem &item,
																				 size_t depth) {
					if (depth > Limits::MaximumArrayDepth)
						return c.Fail(
							Status::LimitExceeded,
							"Particle sprite array nesting exceeds bounds",
							"particle_sprite"
						);
					if (const auto *index = std::get_if<size_t>(&item.Data)) {
						if (*index >= array->Images.size())
							return c.Fail(
								Status::InvalidValue,
								"Particle sprite array index is invalid",
								"particle_sprite"
							);
						return CopyImage(c, p, array->Images[*index], "particle_sprite");
					}
					for (const auto &child : std::get<std::vector<ImageArrayItem>>(item.Data))
						if (!append(child, depth + 1)) return false;
					return true;
				};
				if (array->Items.empty()) {
					for (const auto &image : array->Images)
						if (!CopyImage(c, p, image, "particle_sprite")) return false;
				} else
					for (const auto &item : array->Items)
						if (!append(item, 0)) return false;
			} else if (const auto *atlas = value ? std::get_if<AtlasValue>(value) : nullptr;
					   atlas && atlas->Data) {
				controls.SpriteArray = false;
				if (!CopyAtlas(c, p, *atlas, "particle_sprite")) return false;
			} else if (const Image *image = c.Input("particle_sprite")) {
				controls.SpriteArray = false;
				if (!CopyImage(c, p, *image, "particle_sprite")) return false;
			} else if (const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr) {
				controls.SpriteArray = true;
				controls.SpriteEmptyArray =
					array->Items.empty() && array->Elements.empty() && array->Nested.empty();
				if (!array->Items.empty()) {
					if (!array->Elements.empty() || !array->Nested.empty())
						return c.Fail(
							Status::InvalidValue, "Particle sprite array has mixed shapes", "particle_sprite"
						);
					for (const auto &item : array->Items)
						if (!AppendSpriteItem(c, p, item, 0)) return false;
				} else if (!array->Nested.empty()) {
					if (!array->Elements.empty())
						return c.Fail(
							Status::InvalidValue, "Particle sprite array has mixed shapes", "particle_sprite"
						);
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							if (!AppendSpriteElement(c, p, leaf)) return false;
				} else
					for (const auto &leaf : array->Elements)
						if (!AppendSpriteElement(c, p, leaf)) return false;
			}
			controls.Sprites = p.Sprites;
			controls.SpriteAtlasRects = p.SpriteAtlasRects;
			return true;
		}

		bool ReadPalette(NodeContext &c, SourceParticle2DPrepared &p) {
			const Value *value = c.Find("color_by_index");
			if (!value) {
				p.Controls.Palette = {};
				return true;
			}
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array || !array->Nested.empty() || !array->Items.empty() ||
				array->Elements.size() > Limits::MaximumArrayElements)
				return c.Fail(
					Status::TypeMismatch, "Particle palette requires colour leaves", "color_by_index"
				);
			if (!Charge(c, p, array->Elements.size() * sizeof(Colour), "color_by_index")) return false;
			p.Palette.reserve(array->Elements.size());
			for (const auto &leaf : array->Elements) {
				const auto *colour = std::get_if<Colour>(&leaf);
				if (!colour)
					return c.Fail(
						Status::TypeMismatch, "Particle palette item must be a colour", "color_by_index"
					);
				p.Palette.push_back(*colour);
			}
			p.Controls.Palette = p.Palette;
			return true;
		}

		bool ReadSpawnData(NodeContext &c, SourceParticle2DPrepared &p) {
			const Value *value = c.Find("spawn_data");
			if (!value) return true;
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array)
				return c.Fail(Status::TypeMismatch, "Particle spawn data requires an array", "spawn_data");
			const size_t count = !array->Items.empty()	 ? array->Items.size()
								 : array->Nested.empty() ? array->Elements.size()
														 : array->Nested.size();
			if (count > Limits::MaximumArrayElements ||
				count > MAXIMUM_WORK / 2 / std::max<size_t>(c.ProcessorCount, 1))
				return c.Fail(Status::LimitExceeded, "Particle spawn data exceeds bounds", "spawn_data");
			if (!Charge(c, p, count * sizeof(Vector2), "spawn_data")) return false;
			p.SpawnData.reserve(count);
			if (!array->Items.empty()) {
				for (const auto &item : array->Items) {
					Vector2 point;
					if (!PointItem(c, item, point, "spawn_data")) return false;
					p.SpawnData.push_back(point);
				}
			} else if (!array->Nested.empty()) {
				for (const auto &row : array->Nested) {
					if (row.size() != 2)
						return c.Fail(
							Status::TypeMismatch, "Particle spawn point needs two coordinates", "spawn_data"
						);
					Vector2 point;
					bool valid = true;
					point.X = Number(row[0], valid);
					point.Y = Number(row[1], valid);
					if (!valid || !std::isfinite(point.X) || !std::isfinite(point.Y))
						return c.Fail(Status::InvalidValue, "Particle spawn point is invalid", "spawn_data");
					p.SpawnData.push_back(point);
				}
			} else {
				for (const auto &leaf : array->Elements) {
					const auto *point = std::get_if<Vector2>(&leaf);
					if (!point || !std::isfinite(point->X) || !std::isfinite(point->Y))
						return c.Fail(
							Status::TypeMismatch,
							"Particle spawn data needs finite vector2 points",
							"spawn_data"
						);
					p.SpawnData.push_back(*point);
				}
			}
			p.Controls.SpawnData = p.SpawnData;
			return true;
		}

		bool GeneratePoisson(NodeContext &c, SourceParticle2DPrepared &p) {
			auto &controls = p.Controls;
			if (controls.Distribution != 2 || controls.SpawnSource != 0) return true;
			// This is a deterministic native profile. The pinned extension uses libc rand(), so parity is
			// unclaimed.
			const auto &area = controls.SpawnArea;
			const double distance = std::max(controls.Distance, 2.0);
			if (!std::isfinite(distance))
				return c.Fail(Status::InvalidValue, "Poisson distance is invalid", "distance");
			const double cell = std::floor(distance / std::sqrt(2.0));
			if (!std::isfinite(cell) || cell < 1)
				return c.Fail(Status::InvalidValue, "Poisson cell size is invalid", "distance");
			const double wd = std::ceil(area.HalfWidth * 2 / cell) + 1,
						 hd = std::ceil(area.HalfHeight * 2 / cell) + 1;
			if (!std::isfinite(wd) || !std::isfinite(hd) || wd < 1 || hd < 1 || wd > 100000 || hd > 100000 ||
				wd * hd > MAXIMUM_WORK / 4)
				return c.Fail(
					Status::LimitExceeded, "Particle Poisson grid exceeds work bounds", "spawn_area"
				);
			const size_t width = size_t(wd), height = size_t(hd);
			const uint64_t gridCount = uint64_t(width) * height;
			const uint64_t workCap = MAXIMUM_WORK / std::max<size_t>(c.ProcessorCount, 1);
			if (gridCount >= workCap)
				return c.Fail(
					Status::LimitExceeded, "Particle Poisson grid leaves no search budget", "spawn_area"
				);
			const size_t pointCap =
				size_t(std::min<uint64_t>(MAXIMUM_POISSON_POINTS, (workCap - gridCount) / (32 * 9)));
			if (!pointCap)
				return c.Fail(
					Status::LimitExceeded, "Particle Poisson point search exceeds work bounds", "distance"
				);
			if (!Charge(c, p, gridCount * sizeof(int64_t), "spawn_area")) return false;
			std::vector<int64_t> grid(size_t(gridCount), -1);
			std::vector<Vector2> points, active;
			if (!Charge(c, p, pointCap * sizeof(Vector2) * 2, "spawn_area")) return false;
			points.reserve(pointCap);
			active.reserve(pointCap);
			SourceRandom random(controls.Seed);
			const auto inside = [&](Vector2 q) {
				if (area.Shape == 0)
					return q.X >= area.CenterX - area.HalfWidth && q.X <= area.CenterX + area.HalfWidth &&
						   q.Y >= area.CenterY - area.HalfHeight && q.Y <= area.CenterY + area.HalfHeight;
				if (area.HalfWidth == 0 || area.HalfHeight == 0)
					return q.X == area.CenterX && q.Y == area.CenterY;
				const double x = (q.X - area.CenterX) / area.HalfWidth,
							 y = (q.Y - area.CenterY) / area.HalfHeight;
				return x * x + y * y <= 1;
			};
			Vector2 first{
				area.CenterX + (random.Unit() * 2 - 1) * area.HalfWidth,
				area.CenterY + (random.Unit() * 2 - 1) * area.HalfHeight
			};
			if (!inside(first)) first = {area.CenterX, area.CenterY};
			points.push_back(first);
			active.push_back(first);
			auto gridIndex = [&](Vector2 q) {
				const size_t x = std::min(
					width - 1,
					size_t(std::max(0., std::floor((q.X - (area.CenterX - area.HalfWidth)) / cell)))
				);
				const size_t y = std::min(
					height - 1,
					size_t(std::max(0., std::floor((q.Y - (area.CenterY - area.HalfHeight)) / cell)))
				);
				return y * width + x;
			};
			grid[gridIndex(first)] = 0;
			uint64_t work = gridCount;
			while (!active.empty()) {
				if (work > workCap - 32 * 9)
					return c.Fail(
						Status::LimitExceeded, "Particle Poisson search exceeds work bounds", "distance"
					);
				work += 32 * 9;
				const size_t activeIndex = random.Index(uint32_t(active.size()));
				const Vector2 origin = active[activeIndex];
				bool accepted = false;
				for (size_t attempt = 0; attempt < 32; ++attempt) {
					const double radius = distance * (1 + random.Unit()), angle = random.Unit() * 360;
					Vector2 candidate{
						origin.X + radius * std::cos(angle), origin.Y + radius * std::sin(angle)
					};
					if (!inside(candidate)) continue;
					const size_t gi = gridIndex(candidate), gx = gi % width, gy = gi / width;
					bool clear = true;
					for (int dy = -1; dy <= 1 && clear; ++dy)
						for (int dx = -1; dx <= 1; ++dx) {
							const int64_t x = int64_t(gx) + dx, y = int64_t(gy) + dy;
							if (x < 0 || y < 0 || x >= int64_t(width) || y >= int64_t(height)) continue;
							const int64_t found = grid[size_t(y) * width + size_t(x)];
							if (found >= 0) {
								const auto &other = points[size_t(found)];
								const double vx = other.X - candidate.X, vy = other.Y - candidate.Y;
								if (vx * vx + vy * vy < distance * distance) {
									clear = false;
									break;
								}
							}
						}
					if (!clear) continue;
					if (points.size() >= pointCap)
						return c.Fail(
							Status::LimitExceeded,
							"Particle Poisson point cloud exceeds native work bound",
							"distance"
						);
					grid[gi] = int64_t(points.size());
					points.push_back(candidate);
					active.push_back(candidate);
					accepted = true;
					break;
				}
				if (!accepted) {
					active[activeIndex] = active.back();
					active.pop_back();
				}
			}
			const size_t count = std::min<size_t>(points.size(), 4096);
			if (!Charge(c, p, count * sizeof(Vector2), "distance")) return false;
			p.PoissonPoints.assign(points.begin(), points.begin() + count);
			controls.PoissonPoints = p.PoissonPoints;
			return true;
		}

		bool PreparePath(
			NodeContext &c,
			SourceParticle2DPrepared &p,
			std::string_view id,
			std::unique_ptr<PathRuntime> &runtime,
			const PathRuntime *&borrowed
		) {
			const Value *value = c.Find(id);
			if (!value || c.IsCatalogueDefault(id) == true) return true;
			const auto *path = std::get_if<Path2D>(value);
			if (!path) return c.Fail(Status::TypeMismatch, "Particle path input requires a 2D path", id);
			const auto work = SourceWeightRuntimeWork(path, nullptr);
			if (!work || *work > MAXIMUM_WORK / std::max<size_t>(c.ProcessorCount, 1))
				return c.Fail(Status::LimitExceeded, "Particle path exceeds work bounds", id);
			if (!PathRuntime::StorageBytes(*path))
				return c.Fail(Status::LimitExceeded, "Particle path exceeds storage bounds", id);
			const uint64_t workCap = MAXIMUM_WORK / std::max<size_t>(c.ProcessorCount, 1);
			if (p.Controls.PathSampleWork > workCap || *work > workCap - p.Controls.PathSampleWork)
				return c.Fail(Status::LimitExceeded, "Combined particle path work exceeds bounds", id);
			runtime = std::make_unique<PathRuntime>();
			if (!runtime->Init(c, *path)) return false;
			borrowed = runtime.get();
			p.Controls.PathSampleWork += *work;
			return true;
		}
	}

	SourceParticle2DPrepared::SourceParticle2DPrepared() = default;
	SourceParticle2DPrepared::~SourceParticle2DPrepared() = default;

	bool PrepareSourceParticle2DControls(NodeContext &context, SourceParticle2DPrepared &prepared) {
		auto &c = prepared.Controls;
		const double seed = context.Scalar("seed", 0);
		if (!std::isfinite(seed) || seed < 0 || seed > UINT32_MAX || std::trunc(seed) != seed)
			return context.Fail(Status::InvalidValue, "Particle seed must be uint32", "seed");
		c.Seed = uint32_t(seed);
		const double pool = context.Scalar("attribute_part_amount", 512);
		if (!std::isfinite(pool) || pool < 0 || pool > Limits::MaximumArrayElements ||
			std::trunc(pool) != pool)
			return context.Fail(
				Status::LimitExceeded, "Particle pool capacity exceeds bounds", "attribute_part_amount"
			);
		c.PoolCapacity = uint32_t(pool);
		const uint64_t frames = context.Timeline ? context.Timeline->Frames : 1;
		if (!frames || frames > UINT32_MAX)
			return context.Fail(
				Status::LimitExceeded, "Particle timeline exceeds frame bounds", "pre_render"
			);
		c.TotalFrames = uint32_t(frames);
		c.Spawn = context.Boolean("spawn", true);
		c.Trigger = context.Find("spawn_2") ? context.Boolean("spawn_2") : context.Boolean("spawn_trigger");
		c.StretchAnimation = context.Boolean("stretch_animation");
		c.DirectedFromCenter = context.Boolean("directed_from_center");
		c.RotateByDirection = context.Boolean("rotate_by_direction");
		c.Render = context.Boolean("render", true);
		c.Loop = context.Boolean("loop", true);
		c.RoundPosition = context.Boolean("round_position", true);
		c.SortY = context.Boolean("sort_y");
		c.FollowPath = context.Boolean("follow_path");
		c.PathLoop = context.Boolean("path_loop", true);
		c.Physics = context.Boolean("use_physics");
		c.TurnBothDirections = context.Boolean("turn_both_directions");
		c.Ground = context.Boolean("collide_ground");
		c.Wiggles = context.Boolean("use_wiggles");
		if (!Choice(context, "surface_array", 3, c.SpriteSelection) ||
			!Choice(context, "on_animation_end", 2, c.AnimationEnd) ||
			!Choice(context, "spawn_type", 2, c.SpawnType) ||
			!Choice(context, "spawn_source", 4, c.SpawnSource) ||
			!Choice(context, "distribution", 2, c.Distribution) ||
			!Choice(context, "direction_distribution", 1, c.DirectionDistribution) ||
			!Choice(context, "wrap", 3, c.Wrap) || !Choice(context, "rotation_type", 2, c.RotationType) ||
			!Choice(context, "render_type", 1, c.RenderType) ||
			!Choice(context, "blend_mode", 3, c.BlendMode) ||
			!Choice(context, "ground_offset_type", 1, c.GroundOffsetType))
			return false;
		if (!Choice(context, "attribute_array_select_color_by_index", 2, c.PaletteSelection)) return false;
		c.SpawnDelay = context.Integer("spawn_delay", 4);
		c.BurstDuration = context.Integer("burst_duration", 1);
		c.PreRender = context.Integer("pre_render", -1);
		c.LineLife = context.Integer("line_life", 4);
		c.Distance = context.Scalar("distance", 8);
		c.UniformPeriod = context.Scalar("uniform_period", 4);
		c.SnapRotation = context.Scalar("snap_rotation");
		c.Deviation = context.Scalar("deviation", 1);
		c.GravityDirection = context.Scalar("gravity_direction", -90);
		c.TurnScaleWithSpeed = context.Scalar("turn_scale_with_speed");
		c.BounceAmount = context.Scalar("bounce_amount", .5);
		c.BounceFriction = context.Scalar("bounce_friction", .1);
		if (!Vec2Range(context, "animation_speed", c.AnimationSpeed) ||
			!Vec2Range(context, "spawn_amount", c.SpawnAmount) ||
			!Vec2Range(context, "lifespan", c.Lifespan) || !Vec2Range(context, "speed", c.Speed) ||
			!Vec2Range(context, "angle_range", c.AngleRange) || !Vec2Range(context, "size", c.Size) ||
			!Vec2Range(context, "alpha", c.Alpha) || !Vec2Range(context, "range_shift", c.RangeShift) ||
			!Vec2Range(context, "friction", c.Friction) ||
			!Vec2Range(context, "acceleration", c.Acceleration) ||
			!Vec2Range(context, "gravity", c.Gravity) || !Vec2Range(context, "turning", c.Turning) ||
			!Vec2Range(context, "ground_offset", c.GroundOffset) ||
			!Vec2RangeAlias(context, "direction_wiggle", "direction", c.DirectionWiggle) ||
			!Vec2RangeAlias(context, "position_wiggle", "position", c.PositionWiggle) ||
			!Vec2RangeAlias(context, "rotation_wiggle", "rotation", c.RotationWiggle) ||
			!Vec2RangeAlias(context, "scale_wiggle", "scale_2", c.ScaleWiggle) ||
			!Vec4Range(context, "scale", c.Scale) || !Vec4Range(context, "path_range", c.PathRange))
			return false;
		if (!RotationRows(
				context, prepared, "initial_direction", prepared.Directions, c.Direction, c.Directions
			) ||
			!RotationRows(
				context, prepared, "initial_rotation", prepared.Rotations, c.Rotation, c.Rotations
			) ||
			!Rotation(context, "rotational_speed", c.RotationSpeed) ||
			!Rotation(context, "target_angle", c.TargetAngle))
			return false;
		if (!CurveInput(context, "speed_over_lifespan", c.SpeedCurve) ||
			!CurveInput(context, "rotational_speed_over_lifespan", c.RotationCurve) ||
			!CurveInput(context, "target_angle_over_lifespan", c.TargetCurve) ||
			!CurveInput(context, "size_over_lifespan", c.ScaleCurve) ||
			!CurveInput(context, "alpha_over_lifespan", c.AlphaCurve) ||
			!CurveInput(context, "path_speed", c.PathSpeedCurve) ||
			!CurveInput(context, "deviation_curve", c.PathDeviationCurve) ||
			!GradientInput(context, "color_on_spawn", c.SpawnColour) ||
			!GradientInput(context, "color_over_lifetime", c.LifetimeColour))
			return false;
		if (!ReadSprites(context, prepared) || !ReadPalette(context, prepared) ||
			!ReadSpawnData(context, prepared))
			return false;
		c.Background = context.Input("background");
		c.DistributionMap = context.Input("distribution_map");
		c.SampleSurface = context.Input("sample_surface");
		if (c.Background)
			c.Dimension = {double(c.Background->Width), double(c.Background->Height)};
		else {
			const Image *dimensionMask = c.Sprites.empty() ? nullptr : &c.Sprites.front();
			uint32_t dimensionWidth = 0, dimensionHeight = 0;
			if (!source2d::ResolveGeneratorDimensions(
					context, dimensionMask, dimensionWidth, dimensionHeight
				))
				return false;
			c.Dimension = {double(dimensionWidth), double(dimensionHeight)};
		}
		if (!std::isfinite(c.Dimension.X) || !std::isfinite(c.Dimension.Y) || c.Dimension.X < 1 ||
			c.Dimension.Y < 1 || c.Dimension.X > Limits::MaximumDimension ||
			c.Dimension.Y > Limits::MaximumDimension)
			return context.Fail(
				Status::LimitExceeded, "Particle output dimensions exceed bounds", "dimension"
			);
		if (const Value *v = context.Find("spawn_area")) {
			const auto *a = std::get_if<Area>(v);
			if (!a)
				return context.Fail(
					Status::TypeMismatch, "Particle spawn area requires an area", "spawn_area"
				);
			c.SpawnArea = *a;
		}
		const int64_t areaUnit = context.Integer("spawn_area_unit", 1);
		if (areaUnit < 0 || areaUnit > 1)
			return context.Fail(Status::InvalidValue, "Particle area unit is invalid", "spawn_area");
		if (!context.IsLinked("spawn_area") && areaUnit == 1) {
			c.SpawnArea.CenterX *= context.Project.SurfaceWidth;
			c.SpawnArea.HalfWidth *= context.Project.SurfaceWidth;
			c.SpawnArea.CenterY *= context.Project.SurfaceHeight;
			c.SpawnArea.HalfHeight *= context.Project.SurfaceHeight;
		}
		if (!std::isfinite(c.SpawnArea.CenterX) || !std::isfinite(c.SpawnArea.CenterY) ||
			!std::isfinite(c.SpawnArea.HalfWidth) || !std::isfinite(c.SpawnArea.HalfHeight) ||
			c.SpawnArea.HalfWidth < 0 || c.SpawnArea.HalfHeight < 0 || c.SpawnArea.Shape > 1)
			return context.Fail(Status::InvalidValue, "Particle spawn area is invalid", "spawn_area");
		if (c.SpawnSource == 3 &&
			!PreparePath(context, prepared, "spawn_path", prepared.SpawnRuntime, c.SpawnPath))
			return false;
		if (c.FollowPath && !PreparePath(context, prepared, "path", prepared.FollowRuntime, c.Path))
			return false;
		if (!c.Path) c.FollowPath = false;
		if (!GeneratePoisson(context, prepared)) return false;
		return context.FailureCode == Status::Ok;
	}
}
