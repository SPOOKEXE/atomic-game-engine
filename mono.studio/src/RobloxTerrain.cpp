// Roblox smooth terrain (`Terrain.SmoothGrid`) decoded into merged voxel boxes.
//
// The engine has no voxel terrain, so an imported place keeps its ground as
// anchored box parts. Each 32^3 chunk is merged greedily per material, which
// keeps a solid hillside to a handful of boxes instead of one part per voxel.
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
// A terrain that needs more boxes than the cap is merged again at 2x, 4x and
// then 8x coarser voxels, each cell taking its most common solid material when
// at least half of it is solid. One serial pass checks the bytes and finds each
// chunk; chunks then expand and merge in parallel, one chunk buffer per worker.

#include <engine/core/types/Color3.hpp>
#include <engine/parallel/Jobs.hpp>

#include <algorithm>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <studio/RobloxImport.hpp>
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

		// Greedy box merge of one chunk of `side`^3 cells. `voxels` holds a
		// material per cell (AIR for empty) and is consumed: merged cells are
		// set to AIR. Boxes are appended to `out`.
		void MergeChunk(
			std::vector<uint8_t> &voxels, int side, const std::array<int64_t, 3> &chunk, std::vector<RobloxTerrainBox> &out
		) {
			const auto at = [side](int x, int y, int z) {
				return static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(side) +
					   static_cast<size_t>(z) * static_cast<size_t>(side) * static_cast<size_t>(side);
			};
			for (int z = 0; z < side; z++) {
				for (int y = 0; y < side; y++) {
					for (int x = 0; x < side; x++) {
						const uint8_t material = voxels[at(x, y, z)];
						if (material == AIR) {
							continue;
						}
						int width = 1;
						while (x + width < side && voxels[at(x + width, y, z)] == material) {
							width++;
						}
						int height = 1;
						const auto rowMatches = [&](int row, int slab) {
							for (int column = x; column < x + width; column++) {
								if (voxels[at(column, row, slab)] != material) {
									return false;
								}
							}
							return true;
						};
						while (y + height < side && rowMatches(y + height, z)) {
							height++;
						}
						int depth = 1;
						const auto slabMatches = [&](int slab) {
							for (int row = y; row < y + height; row++) {
								if (!rowMatches(row, slab)) {
									return false;
								}
							}
							return true;
						};
						while (z + depth < side && slabMatches(z + depth)) {
							depth++;
						}
						for (int slab = z; slab < z + depth; slab++) {
							for (int row = y; row < y + height; row++) {
								for (int column = x; column < x + width; column++) {
									voxels[at(column, row, slab)] = AIR;
								}
							}
						}
						const float cell = VOXEL_STUDS * static_cast<float>(CHUNK / side);
						const auto stud = [&](int64_t chunkAxis, int local) {
							return static_cast<float>(chunkAxis * side + local) * cell;
						};
						out.push_back(
							RobloxTerrainBox{
								{stud(chunk[0], x), stud(chunk[1], y), stud(chunk[2], z)},
								{stud(chunk[0], x + width), stud(chunk[1], y + height), stud(chunk[2], z + depth)},
								material,
							}
						);
					}
				}
			}
		}
	}

	namespace {
		// Reduces a 32^3 chunk to (32/factor)^3 cells by majority.
		void Downsample(const std::vector<uint8_t> &voxels, int factor, std::vector<uint8_t> &out) {
			const int side = CHUNK / factor;
			out.assign(static_cast<size_t>(side) * side * side, AIR);
			std::array<uint32_t, 64> counts{};
			for (int z = 0; z < side; z++) {
				for (int y = 0; y < side; y++) {
					for (int x = 0; x < side; x++) {
						counts.fill(0);
						uint32_t solid = 0;
						for (int dz = 0; dz < factor; dz++) {
							for (int dy = 0; dy < factor; dy++) {
								for (int dx = 0; dx < factor; dx++) {
									const size_t index = static_cast<size_t>(x * factor + dx) +
														 static_cast<size_t>(y * factor + dy) * CHUNK +
														 static_cast<size_t>(z * factor + dz) * CHUNK * CHUNK;
									const uint8_t material = voxels[index];
									if (material != AIR) {
										counts[material]++;
										solid++;
									}
								}
							}
						}
						const auto cells = static_cast<uint32_t>(factor * factor * factor);
						if (solid * 2 < cells) {
							continue;
						}
						uint8_t best = AIR;
						for (uint8_t material = 1; material < counts.size(); material++) {
							if (counts[material] > counts[best]) {
								best = material;
							}
						}
						out[static_cast<size_t>(x) + static_cast<size_t>(y) * side +
							static_cast<size_t>(z) * side * side] = best;
					}
				}
			}
		}

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
					if (position[axis] > MAXIMUM_CHUNK_COORDINATE || position[axis] < -MAXIMUM_CHUNK_COORDINATE) {
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
				const uint8_t stored = occupancy >= SOLID_OCCUPANCY ? static_cast<uint8_t>(flags & 0x3F) : AIR;
				std::fill_n(voxels.begin() + static_cast<std::ptrdiff_t>(filled), run, stored);
				filled += run;
			}
		}
	}

	bool DecodeRobloxSmoothGrid(
		std::string_view bytes, size_t maximumBoxes, RobloxTerrainVoxels &out, std::string &error
	) {
		if (bytes.size() < 2 || bytes[0] != 0x01 || bytes[1] != 0x05) {
			error = "terrain SmoothGrid has an unknown header";
			return false;
		}

		RobloxTerrainVoxels candidate;
		std::vector<ChunkSpan> chunks;
		if (!IndexChunks(bytes, chunks, candidate.SolidVoxels, candidate.UnknownMaterialVoxels, error)) {
			return false;
		}
		candidate.Chunks = chunks.size();

		// Each chunk merges on its own, in parallel, and the results join in
		// file order, so the boxes match a serial decode exactly.
		std::vector<std::vector<RobloxTerrainBox>> perChunk(chunks.size());
		constexpr std::array<int, 4> FACTORS{1, 2, 4, 8};
		size_t total = 0;
		for (const int factor : FACTORS) {
			engine::parallel::Jobs::For(
				chunks.size(),
				4,
				[&](size_t begin, size_t end) {
					std::vector<uint8_t> voxels(CHUNK_VOXELS);
					std::vector<uint8_t> cells;
					for (size_t index = begin; index < end; index++) {
						perChunk[index].clear();
						FillChunk(bytes, chunks[index].Begin, voxels);
						if (factor == 1) {
							MergeChunk(voxels, CHUNK, chunks[index].Position, perChunk[index]);
						} else {
							Downsample(voxels, factor, cells);
							MergeChunk(cells, CHUNK / factor, chunks[index].Position, perChunk[index]);
						}
					}
				},
				1
			);
			total = 0;
			for (const std::vector<RobloxTerrainBox> &boxes : perChunk) {
				total += boxes.size();
			}
			candidate.VoxelStuds = VOXEL_STUDS * static_cast<float>(factor);
			if (total <= maximumBoxes) {
				break;
			}
		}

		// Even the coarsest cells overflowed: keep whole chunks in file order
		// up to the cap.
		candidate.Truncated = total > maximumBoxes;
		candidate.Boxes.reserve(std::min(total, maximumBoxes));
		for (std::vector<RobloxTerrainBox> &boxes : perChunk) {
			if (candidate.Boxes.size() + boxes.size() > maximumBoxes) {
				break;
			}
			candidate.Boxes.insert(candidate.Boxes.end(), boxes.begin(), boxes.end());
		}
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
}
