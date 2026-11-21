// Roblox smooth terrain (`Terrain.SmoothGrid`) decoded into surface meshes.
//
// The engine has no voxel terrain, so an imported place keeps its ground as one
// EditableMesh: only the faces between a voxel and a different kind of
// neighbour, with coplanar faces of one material merged into rectangles. A
// mesh is one collider and one draw where the box parts it replaced were tens
// of thousands of each, and the static physics index rebuilt over all of them.
//
// The layout was read off real places, since Roblox does not document it:
//
// - two header bytes, `01 05` (version 1, chunks of 2^5 voxels per side);
// - then chunks until the end of the buffer. Each chunk starts with its grid
//   position as three int32, byte-interleaved big-endian (x0 y0 z0 x1 y1 z1 ...),
//   stored as a delta from the previous chunk's position;
// - then runs covering exactly 32768 voxels, x fastest, then y, then z. A run
//   is a flag byte: bits 0-5 the material, 0x40 an occupancy byte follows
//   (otherwise full), 0x80 a count byte follows (run length is count + 1,
//   otherwise 1). The occupancy byte comes before the count byte. Newer
//   places also write a count byte of 0 followed by one more byte (seen: 0xFF
//   and 0x4C); that run is one voxel and the extra byte is skipped. With that
//   reading every chunk of every place tried sums to exactly 32768 voxels.
//
// A voxel is 4 studs, and voxel `i` spans [4i, 4i + 4) on each axis.
//
// One serial pass checks the bytes and finds each chunk; chunks then expand in
// parallel, and mesh in parallel against their expanded neighbours.

#include <engine/core/types/Color3.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <studio/RobloxImport.hpp>
#include <unordered_map>
#include <vector>

namespace studio {
	namespace {
		constexpr int CHUNK = 32;
		constexpr size_t CHUNK_VOXELS = static_cast<size_t>(CHUNK) * CHUNK * CHUNK;
		constexpr float VOXEL_STUDS = 4.0f;
		constexpr uint8_t AIR = 0;

		// Chunk coordinates further out than this are not a real place. Bounding
		// the sum also keeps every stud position exactly representable.
		constexpr int64_t MAXIMUM_CHUNK_COORDINATE = 1 << 20;

		// A partial voxel counts as solid from half full. Below that the smooth
		// surface passes under the voxel centre.
		constexpr uint8_t SOLID_OCCUPANCY = 128;

		struct MaterialInfo {
			std::string_view Name;
			uint8_t R, G, B;
		};

		// Terrain material ids and Roblox's default colours, in the order the
		// `MaterialColors` blob stores them. Ids 0 and 1 are air and water.
		// Newer places also use ids past this table (up to 63, the six-bit
		// limit). Those voxels stay solid in a neutral grey and are counted.
		constexpr std::array<MaterialInfo, 23> MATERIALS{{
			{"Air", 0, 0, 0},
			{"Water", 12, 84, 92},
			{"Grass", 106, 127, 63},
			{"Slate", 63, 127, 107},
			{"Concrete", 127, 102, 63},
			{"Brick", 138, 86, 62},
			{"Sand", 143, 126, 95},
			{"WoodPlanks", 139, 109, 79},
			{"Rock", 102, 108, 111},
			{"Glacier", 101, 176, 234},
			{"Snow", 195, 199, 218},
			{"Sandstone", 137, 90, 71},
			{"Mud", 58, 46, 36},
			{"Basalt", 30, 30, 37},
			{"Ground", 102, 92, 59},
			{"CrackedLava", 232, 156, 74},
			{"Asphalt", 115, 123, 107},
			{"Cobblestone", 132, 123, 90},
			{"Ice", 129, 194, 224},
			{"LeafyGrass", 115, 132, 74},
			{"Salt", 198, 189, 181},
			{"Limestone", 206, 173, 148},
			{"Pavement", 148, 148, 140},
		}};

		int32_t InterleavedInt(std::string_view bytes, size_t at, size_t axis) {
			uint32_t value = 0;
			for (size_t index = 0; index < 4; index++) {
				value = (value << 8) | static_cast<uint8_t>(bytes[at + axis + index * 3]);
			}
			return static_cast<int32_t>(value);
		}

	}

	namespace {
		// Where one chunk's runs start, and its grid position.
		struct ChunkSpan {
			size_t Begin = 0;
			std::array<int64_t, 3> Position{};
		};

		// Walks the whole grid once, checking every bound and counting solid
		// voxels, so the parallel passes after it can trust the bytes.
		bool IndexChunks(
			std::string_view bytes,
			std::vector<ChunkSpan> &chunks,
			size_t &solidVoxels,
			size_t &unknownVoxels,
			std::string &error
		) {
			std::array<int64_t, 3> position{0, 0, 0};
			size_t at = 2;
			while (at < bytes.size()) {
				if (bytes.size() - at < 12) {
					error = "terrain SmoothGrid ends inside a chunk position";
					return false;
				}
				for (size_t axis = 0; axis < 3; axis++) {
					position[axis] += InterleavedInt(bytes, at, axis);
					if (position[axis] > MAXIMUM_CHUNK_COORDINATE ||
						position[axis] < -MAXIMUM_CHUNK_COORDINATE) {
						error = "terrain SmoothGrid chunk lies outside the supported extent";
						return false;
					}
				}
				at += 12;
				chunks.push_back({at, position});

				size_t filled = 0;
				while (filled < CHUNK_VOXELS) {
					if (at >= bytes.size()) {
						error = "terrain SmoothGrid ends inside a chunk";
						return false;
					}
					const auto flags = static_cast<uint8_t>(bytes[at++]);
					const uint8_t material = flags & 0x3F;
					uint8_t occupancy = 0xFF;
					size_t run = 1;
					if ((flags & 0x40) != 0) {
						if (at >= bytes.size()) {
							error = "terrain SmoothGrid ends inside a run";
							return false;
						}
						occupancy = static_cast<uint8_t>(bytes[at++]);
					}
					if ((flags & 0x80) != 0) {
						if (at >= bytes.size()) {
							error = "terrain SmoothGrid ends inside a run";
							return false;
						}
						const auto count = static_cast<uint8_t>(bytes[at++]);
						if (count == 0) {
							// The escape: one more byte, and a run of one.
							if (at >= bytes.size()) {
								error = "terrain SmoothGrid ends inside a run";
								return false;
							}
							at++;
						} else {
							run = static_cast<size_t>(count) + 1;
						}
					}
					if (run > CHUNK_VOXELS - filled) {
						error = "terrain SmoothGrid run overflows its chunk";
						return false;
					}
					if (material != AIR && occupancy >= SOLID_OCCUPANCY) {
						solidVoxels += run;
						if (material >= MATERIALS.size()) {
							unknownVoxels += run;
						}
					}
					filled += run;
				}
			}
			return true;
		}

		// Expands one chunk's runs, already checked by `IndexChunks`.
		void FillChunk(std::string_view bytes, size_t at, std::vector<uint8_t> &voxels) {
			size_t filled = 0;
			while (filled < CHUNK_VOXELS) {
				const auto flags = static_cast<uint8_t>(bytes[at++]);
				uint8_t occupancy = 0xFF;
				size_t run = 1;
				if ((flags & 0x40) != 0) {
					occupancy = static_cast<uint8_t>(bytes[at++]);
				}
				if ((flags & 0x80) != 0) {
					const auto count = static_cast<uint8_t>(bytes[at++]);
					if (count == 0) {
						at++;
					} else {
						run = static_cast<size_t>(count) + 1;
					}
				}
				const uint8_t stored =
					occupancy >= SOLID_OCCUPANCY ? static_cast<uint8_t>(flags & 0x3F) : AIR;
				std::fill_n(voxels.begin() + static_cast<std::ptrdiff_t>(filled), run, stored);
				filled += run;
			}
		}
	}

	namespace {
		constexpr uint8_t WATER = 1;

		// 0 air, 1 water, 2 solid: which faces a voxel shows to which neighbour.
		int Kind(uint8_t material) {
			return material == AIR ? 0 : material == WATER ? 1 : 2;
		}

		uint64_t ChunkKey(const std::array<int64_t, 3> &position) {
			const auto bits = [](int64_t value) {
				return static_cast<uint64_t>(value + MAXIMUM_CHUNK_COORDINATE) & ((uint64_t{1} << 21) - 1);
			};
			return bits(position[0]) | (bits(position[1]) << 21) | (bits(position[2]) << 42);
		}

		// Appends one chunk's faces of one surface to `out`, merging coplanar
		// faces of one material into rectangles slice by slice.
		template <class Sample, class Shows>
		void MeshChunk(
			const std::array<int64_t, 3> &chunk,
			const Sample &sample,
			const Shows &shows,
			RobloxTerrainSurface &out
		) {
			std::array<uint8_t, CHUNK * CHUNK> mask{};
			// Vertices already emitted in this chunk, by corner, face and material.
			std::unordered_map<uint64_t, uint32_t> shared;
			for (int axis = 0; axis < 3; ++axis) {
				const int u = (axis + 1) % 3;
				const int v = (axis + 2) % 3;
				for (const int direction : {-1, 1}) {
					for (int slice = 0; slice < CHUNK; ++slice) {
						bool any = false;
						for (int b = 0; b < CHUNK; ++b) {
							for (int a = 0; a < CHUNK; ++a) {
								std::array<int, 3> at{};
								at[axis] = slice;
								at[u] = a;
								at[v] = b;
								const uint8_t material = sample(at);
								std::array<int, 3> next = at;
								next[axis] += direction;
								const uint8_t face = shows(material, sample(next)) ? material : AIR;
								mask[static_cast<size_t>(a) + static_cast<size_t>(b) * CHUNK] = face;
								any = any || face != AIR;
							}
						}
						if (!any) continue;
						const float plane = static_cast<float>(slice + (direction > 0 ? 1 : 0));
						for (int b = 0; b < CHUNK; ++b) {
							for (int a = 0; a < CHUNK;) {
								const uint8_t material =
									mask[static_cast<size_t>(a) + static_cast<size_t>(b) * CHUNK];
								if (material == AIR) {
									++a;
									continue;
								}
								int width = 1;
								while (
									a + width < CHUNK &&
									mask[static_cast<size_t>(a + width) + static_cast<size_t>(b) * CHUNK] ==
										material
								)
									++width;
								int height = 1;
								const auto rowMatches = [&](int row) {
									for (int column = a; column < a + width; ++column)
										if (mask
												[static_cast<size_t>(column) +
												 static_cast<size_t>(row) * CHUNK] != material)
											return false;
									return true;
								};
								while (b + height < CHUNK && rowMatches(b + height))
									++height;
								for (int row = b; row < b + height; ++row)
									for (int column = a; column < a + width; ++column)
										mask[static_cast<size_t>(column) + static_cast<size_t>(row) * CHUNK] =
											AIR;

								// Corners in (u, v) order, counter-clockwise about +axis.
								const std::array<std::array<float, 2>, 4> corners{{
									{static_cast<float>(a), static_cast<float>(b)},
									{static_cast<float>(a + width), static_cast<float>(b)},
									{static_cast<float>(a + width), static_cast<float>(b + height)},
									{static_cast<float>(a), static_cast<float>(b + height)},
								}};
								engine::core::Vector3 normal{};
								(axis == 0	 ? normal.X
								 : axis == 1 ? normal.Y
											 : normal.Z) = static_cast<float>(direction);
								// Corners shared with a neighbouring rectangle of the same
								// plane and material are one vertex.
								const int face = axis * 2 + (direction > 0 ? 1 : 0);
								std::array<uint32_t, 4> id{};
								for (size_t corner = 0; corner < corners.size(); ++corner) {
									std::array<int, 3> local{};
									local[axis] = static_cast<int>(plane);
									local[u] = static_cast<int>(corners[corner][0]);
									local[v] = static_cast<int>(corners[corner][1]);
									const uint64_t key = static_cast<uint64_t>(local[0]) |
														 (static_cast<uint64_t>(local[1]) << 6) |
														 (static_cast<uint64_t>(local[2]) << 12) |
														 (static_cast<uint64_t>(face) << 18) |
														 (static_cast<uint64_t>(material) << 21);
									const auto [found, added] =
										shared.try_emplace(key, static_cast<uint32_t>(out.Positions.size()));
									id[corner] = found->second;
									if (!added) continue;
									const auto stud = [&](int axisIndex) {
										return (static_cast<float>(
													chunk[static_cast<size_t>(axisIndex)] * CHUNK
												) +
												static_cast<float>(local[static_cast<size_t>(axisIndex)])) *
											   VOXEL_STUDS;
									};
									out.Positions.push_back({stud(0), stud(1), stud(2)});
									out.Normals.push_back(normal);
									out.Materials.push_back(material);
								}
								if (direction > 0)
									out.Indices.insert(
										out.Indices.end(), {id[0], id[1], id[2], id[0], id[2], id[3]}
									);
								else
									out.Indices.insert(
										out.Indices.end(), {id[0], id[2], id[1], id[0], id[3], id[2]}
									);
								a += width;
							}
						}
					}
				}
			}
		}

		void Append(RobloxTerrainSurface &into, const RobloxTerrainSurface &from) {
			const auto offset = static_cast<uint32_t>(into.Positions.size());
			into.Positions.insert(into.Positions.end(), from.Positions.begin(), from.Positions.end());
			into.Normals.insert(into.Normals.end(), from.Normals.begin(), from.Normals.end());
			into.Materials.insert(into.Materials.end(), from.Materials.begin(), from.Materials.end());
			for (const uint32_t index : from.Indices)
				into.Indices.push_back(index + offset);
		}

		void Bound(RobloxTerrainSurface &surface) {
			if (surface.Positions.empty()) return;
			surface.Minimum = surface.Maximum = surface.Positions.front();
			for (const auto &position : surface.Positions) {
				surface.Minimum = {
					std::min(surface.Minimum.X, position.X),
					std::min(surface.Minimum.Y, position.Y),
					std::min(surface.Minimum.Z, position.Z)
				};
				surface.Maximum = {
					std::max(surface.Maximum.X, position.X),
					std::max(surface.Maximum.Y, position.Y),
					std::max(surface.Maximum.Z, position.Z)
				};
			}
		}
	}

	bool BuildRobloxTerrainMesh(std::string_view bytes, RobloxTerrainMesh &out, std::string &error) {
		if (bytes.size() < 2 || bytes[0] != 0x01 || bytes[1] != 0x05) {
			error = "terrain SmoothGrid has an unknown header";
			return false;
		}
		RobloxTerrainMesh candidate;
		std::vector<ChunkSpan> chunks;
		if (!IndexChunks(bytes, chunks, candidate.SolidVoxels, candidate.UnknownMaterialVoxels, error)) {
			return false;
		}
		candidate.Chunks = chunks.size();

		// Every chunk expanded and findable by position, so a face on a chunk
		// edge asks the real neighbour rather than assuming air.
		std::vector<std::vector<uint8_t>> voxels(chunks.size());
		std::unordered_map<uint64_t, size_t> byPosition;
		byPosition.reserve(chunks.size());
		for (size_t index = 0; index < chunks.size(); ++index)
			byPosition.emplace(ChunkKey(chunks[index].Position), index);
		engine::parallel::Jobs::For(
			chunks.size(),
			4,
			[&](size_t begin, size_t end) {
				for (size_t index = begin; index < end; ++index) {
					voxels[index].resize(CHUNK_VOXELS);
					FillChunk(bytes, chunks[index].Begin, voxels[index]);
				}
			},
			1
		);

		// Each chunk meshes on its own; the results join in file order, so the
		// mesh matches a serial build exactly.
		std::vector<RobloxTerrainSurface> solid(chunks.size());
		std::vector<RobloxTerrainSurface> water(chunks.size());
		engine::parallel::Jobs::For(
			chunks.size(),
			4,
			[&](size_t begin, size_t end) {
				for (size_t index = begin; index < end; ++index) {
					const auto &position = chunks[index].Position;
					const auto sample = [&](const std::array<int, 3> &at) -> uint8_t {
						std::array<int64_t, 3> chunk = position;
						std::array<int, 3> local = at;
						for (size_t axis = 0; axis < 3; ++axis) {
							if (local[axis] < 0) {
								--chunk[axis];
								local[axis] += CHUNK;
							} else if (local[axis] >= CHUNK) {
								++chunk[axis];
								local[axis] -= CHUNK;
							}
						}
						const std::vector<uint8_t> *held = &voxels[index];
						if (chunk != position) {
							const auto found = byPosition.find(ChunkKey(chunk));
							if (found == byPosition.end()) return AIR;
							held = &voxels[found->second];
						}
						return (
							*held
						)[static_cast<size_t>(local[0]) + static_cast<size_t>(local[1]) * CHUNK +
						  static_cast<size_t>(local[2]) * CHUNK * CHUNK];
					};
					MeshChunk(
						position,
						sample,
						[](uint8_t material, uint8_t neighbour) {
							return Kind(material) == 2 && Kind(neighbour) != 2;
						},
						solid[index]
					);
					MeshChunk(
						position,
						sample,
						[](uint8_t material, uint8_t neighbour) {
							return Kind(material) == 1 && Kind(neighbour) == 0;
						},
						water[index]
					);
				}
			},
			1
		);
		size_t solidVertices = 0;
		size_t waterVertices = 0;
		for (size_t index = 0; index < chunks.size(); ++index) {
			solidVertices += solid[index].Positions.size();
			waterVertices += water[index].Positions.size();
		}
		if (solidVertices > std::numeric_limits<uint32_t>::max() ||
			waterVertices > std::numeric_limits<uint32_t>::max()) {
			error = "terrain surface has more vertices than one mesh can index";
			return false;
		}
		for (size_t index = 0; index < chunks.size(); ++index) {
			Append(candidate.Solid, solid[index]);
			Append(candidate.Water, water[index]);
		}
		Bound(candidate.Solid);
		Bound(candidate.Water);
		out = std::move(candidate);
		return true;
	}

	std::string_view RobloxTerrainMaterialName(uint8_t material) {
		return material < MATERIALS.size() ? MATERIALS[material].Name : std::string_view("Unknown");
	}

	engine::core::Color3 RobloxTerrainMaterialColor(uint8_t material, std::string_view materialColors) {
		if (material >= MATERIALS.size()) {
			return engine::core::Color3::FromRGB(128, 128, 128);
		}
		// `MaterialColors` is 69 bytes: RGB per material slot, slots 0 and 1
		// unused. An all-zero slot means the author never changed it.
		const size_t offset = static_cast<size_t>(material) * 3;
		if (material > 1 && materialColors.size() == MATERIALS.size() * 3) {
			const auto r = static_cast<uint8_t>(materialColors[offset]);
			const auto g = static_cast<uint8_t>(materialColors[offset + 1]);
			const auto b = static_cast<uint8_t>(materialColors[offset + 2]);
			if (r != 0 || g != 0 || b != 0) {
				return engine::core::Color3::FromRGB(r, g, b);
			}
		}
		const MaterialInfo &info = MATERIALS[material];
		return engine::core::Color3::FromRGB(info.R, info.G, info.B);
	}

	std::vector<RobloxTerrainSurface>
	SplitRobloxTerrainSurface(const RobloxTerrainSurface &surface, float cell) {
		struct Key {
			int32_t X, Y, Z;
			bool operator==(const Key &) const = default;
		};
		struct Hash {
			size_t operator()(const Key &key) const {
				return (static_cast<size_t>(static_cast<uint32_t>(key.X)) * 73856093u) ^
					   (static_cast<size_t>(static_cast<uint32_t>(key.Y)) * 19349663u) ^
					   (static_cast<size_t>(static_cast<uint32_t>(key.Z)) * 83492791u);
			}
		};
		std::unordered_map<Key, size_t, Hash> regionOf;
		std::vector<RobloxTerrainSurface> regions;
		// Per region: source vertex index to region vertex index.
		std::vector<std::unordered_map<uint32_t, uint32_t>> remap;
		for (size_t at = 0; at + 2 < surface.Indices.size(); at += 3) {
			const auto &a = surface.Positions[surface.Indices[at]];
			const auto &b = surface.Positions[surface.Indices[at + 1]];
			const auto &c = surface.Positions[surface.Indices[at + 2]];
			const Key key{
				static_cast<int32_t>(std::floor((a.X + b.X + c.X) / (3.0f * cell))),
				static_cast<int32_t>(std::floor((a.Y + b.Y + c.Y) / (3.0f * cell))),
				static_cast<int32_t>(std::floor((a.Z + b.Z + c.Z) / (3.0f * cell)))
			};
			const auto [found, added] = regionOf.try_emplace(key, regions.size());
			if (added) {
				regions.emplace_back();
				remap.emplace_back();
			}
			RobloxTerrainSurface &region = regions[found->second];
			auto &map = remap[found->second];
			for (size_t corner = 0; corner < 3; ++corner) {
				const uint32_t source = surface.Indices[at + corner];
				const auto [slot, fresh] =
					map.try_emplace(source, static_cast<uint32_t>(region.Positions.size()));
				if (fresh) {
					region.Positions.push_back(surface.Positions[source]);
					region.Normals.push_back(surface.Normals[source]);
					region.Materials.push_back(surface.Materials[source]);
				}
				region.Indices.push_back(slot->second);
			}
		}
		for (auto &region : regions)
			Bound(region);
		return regions;
	}
}
