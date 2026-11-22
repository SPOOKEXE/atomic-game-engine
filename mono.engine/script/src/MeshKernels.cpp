#include <engine/script/MeshKernels.hpp>

#include <cmath>
#include <cstring>
#include <vector>

// Contraction is also turned off for this file in `script/CMakeLists.txt`;
// the pragma covers a compiler that honours it and ignores the flag.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace engine::script::mesh_kernels {

	namespace {

		// Bytes `elements` 4-byte words need, in 64 bits so no product wraps.
		constexpr uint64_t Words(uint64_t elements) {
			return elements * 4;
		}

		bool Fits(size_t length, uint64_t needed) {
			return static_cast<uint64_t>(length) >= needed;
		}

		// A typed view of a stream. Aligned streams (every Luau `buffer` and
		// `ArrayBuffer`) are read in place; an unaligned one is copied into
		// `scratch` first. Element loops then index a plain pointer, which is
		// what keeps the kernels fast in the `-O0` dev build.
		template <typename T> const T *View(ConstBytes bytes, size_t elements, std::vector<T> &scratch) {
			if (reinterpret_cast<uintptr_t>(bytes.data()) % alignof(T) == 0) {
				return reinterpret_cast<const T *>(bytes.data());
			}
			scratch.resize(elements);
			std::memcpy(scratch.data(), bytes.data(), elements * sizeof(T));
			return scratch.data();
		}

		// A writable typed view of `out`. `Commit` copies an unaligned stream's
		// scratch back; an aligned one was written in place.
		template <typename T> struct Output {
			Bytes Target;
			std::vector<T> Scratch;
			T *Data = nullptr;

			Output(Bytes target, size_t elements, bool keep) : Target(target) {
				if (reinterpret_cast<uintptr_t>(target.data()) % alignof(T) == 0) {
					Data = reinterpret_cast<T *>(target.data());
					return;
				}
				Scratch.resize(elements);
				if (keep) std::memcpy(Scratch.data(), target.data(), elements * sizeof(T));
				Data = Scratch.data();
			}

			void Commit() {
				if (!Scratch.empty()) std::memcpy(Target.data(), Scratch.data(), Scratch.size() * sizeof(T));
			}
		};
	}

	bool Axpy(Bytes out, float a, ConstBytes x, uint32_t count) {
		if (!Fits(out.size(), Words(count)) || !Fits(x.size(), Words(count))) return false;
		// Element-wise, so `x` may be `out`: index `i` is read before it is written.
		std::vector<float> scratch;
		const float *in = View<float>(x, count, scratch);
		Output<float> result(out, count, true);
		float *o = result.Data;
		for (size_t i = 0; i < count; ++i) {
			const float product = a * in[i];
			o[i] = o[i] + product;
		}
		result.Commit();
		return true;
	}

	bool WeightedAxpy(Bytes out, ConstBytes weights, ConstBytes x, uint32_t count, uint32_t stride) {
		const uint64_t values = static_cast<uint64_t>(count) * stride;
		if (stride == 0 || !Fits(out.size(), Words(values)) || !Fits(x.size(), Words(values)) ||
			!Fits(weights.size(), Words(count))) {
			return false;
		}
		std::vector<float> xScratch, wScratch;
		const float *in = View<float>(x, values, xScratch);
		const float *w = View<float>(weights, count, wScratch);
		Output<float> result(out, values, true);
		float *o = result.Data;
		for (size_t v = 0; v < count; ++v) {
			const float weight = w[v];
			float *row = o + v * stride;
			const float *source = in + v * stride;
			for (size_t k = 0; k < stride; ++k) {
				const float product = weight * source[k];
				row[k] = row[k] + product;
			}
		}
		result.Commit();
		return true;
	}

	bool Transform(
		Bytes out, ConstBytes input, ConstBytes matrices, ConstBytes indices, ConstBytes weights, uint32_t count
	) {
		const uint64_t positions = static_cast<uint64_t>(count) * 3;
		const uint64_t slots = static_cast<uint64_t>(count) * 4;
		if (!Fits(out.size(), Words(positions)) || !Fits(input.size(), Words(positions)) ||
			!Fits(indices.size(), slots * 2) || !Fits(weights.size(), Words(slots))) {
			return false;
		}
		const uint64_t matrixCount = matrices.size() / 48;
		std::vector<float> mScratch, wScratch, pScratch;
		std::vector<uint16_t> iScratch;
		const float *m = View<float>(matrices, matrixCount * 12, mScratch);
		const float *w = View<float>(weights, slots, wScratch);
		const uint16_t *bone = View<uint16_t>(indices, slots, iScratch);
		for (size_t slot = 0; slot < slots; ++slot) {
			if (w[slot] != 0.0f && bone[slot] >= matrixCount) return false;
		}
		// `out` may be `input`: vertex `v` is read whole before it is written.
		const float *p = View<float>(input, positions, pScratch);
		Output<float> result(out, positions, false);
		float *o = result.Data;

		for (size_t v = 0; v < count; ++v) {
			const float x = p[v * 3];
			const float y = p[v * 3 + 1];
			const float z = p[v * 3 + 2];
			float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f;
			const float *vw = w + v * 4;
			const uint16_t *vb = bone + v * 4;
			for (size_t k = 0; k < 4; ++k) {
				const float weight = vw[k];
				if (weight == 0.0f) continue;
				const float *a = m + static_cast<size_t>(vb[k]) * 12;
				const float t0 = a[0] * x + a[1] * y + a[2] * z + a[3];
				const float t1 = a[4] * x + a[5] * y + a[6] * z + a[7];
				const float t2 = a[8] * x + a[9] * y + a[10] * z + a[11];
				r0 = r0 + weight * t0;
				r1 = r1 + weight * t1;
				r2 = r2 + weight * t2;
			}
			o[v * 3] = r0;
			o[v * 3 + 1] = r1;
			o[v * 3 + 2] = r2;
		}
		result.Commit();
		return true;
	}

	bool Normals(
		Bytes out, ConstBytes positions, ConstBytes triangles, std::optional<ConstBytes> weld, uint32_t count
	) {
		const uint64_t values = static_cast<uint64_t>(count) * 3;
		if (!Fits(out.size(), Words(values)) || !Fits(positions.size(), Words(values)) || triangles.size() % 12 != 0) {
			return false;
		}
		const size_t corners = triangles.size() / 4;
		std::vector<uint32_t> tScratch, wScratch;
		const uint32_t *t = View<uint32_t>(triangles, corners, tScratch);
		for (size_t i = 0; i < corners; ++i) {
			if (t[i] >= count) return false;
		}
		const uint32_t *canonical = nullptr;
		if (weld) {
			if (!Fits(weld->size(), Words(count))) return false;
			canonical = View<uint32_t>(*weld, count, wScratch);
			for (size_t v = 0; v < count; ++v) {
				if (canonical[v] >= count) return false;
			}
		}

		// `out` may be `positions`: every position is read before the first
		// normal is written.
		std::vector<float> pScratch;
		const float *p = View<float>(positions, values, pScratch);
		std::vector<double> sumStorage(values, 0.0);
		double *sums = sumStorage.data();
		for (size_t corner = 0; corner < corners; corner += 3) {
			const size_t a = size_t(t[corner]) * 3, b = size_t(t[corner + 1]) * 3, c = size_t(t[corner + 2]) * 3;
			const double ex = double(p[b]) - double(p[a]);
			const double ey = double(p[b + 1]) - double(p[a + 1]);
			const double ez = double(p[b + 2]) - double(p[a + 2]);
			const double fx = double(p[c]) - double(p[a]);
			const double fy = double(p[c + 1]) - double(p[a + 1]);
			const double fz = double(p[c + 2]) - double(p[a + 2]);
			const double nx = ey * fz - ez * fy;
			const double ny = ez * fx - ex * fz;
			const double nz = ex * fy - ey * fx;
			// Written out rather than looped: the dev build is `-O0`, and this
			// is the hottest loop of every kernel here.
			const uint32_t ta = t[corner], tb = t[corner + 1], tc = t[corner + 2];
			double *sa = sums + size_t(canonical ? canonical[ta] : ta) * 3;
			double *sb = sums + size_t(canonical ? canonical[tb] : tb) * 3;
			double *sc = sums + size_t(canonical ? canonical[tc] : tc) * 3;
			sa[0] += nx;
			sa[1] += ny;
			sa[2] += nz;
			sb[0] += nx;
			sb[1] += ny;
			sb[2] += nz;
			sc[0] += nx;
			sc[1] += ny;
			sc[2] += nz;
		}

		Output<float> result(out, values, false);
		float *o = result.Data;
		for (size_t v = 0; v < count; ++v) {
			const double *sum = sums + size_t(canonical ? canonical[v] : v) * 3;
			const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
			float *normal = o + v * 3;
			if (length > 0.0) {
				normal[0] = static_cast<float>(sum[0] / length);
				normal[1] = static_cast<float>(sum[1] / length);
				normal[2] = static_cast<float>(sum[2] / length);
			} else {
				normal[0] = normal[1] = normal[2] = 0.0f;
			}
		}
		result.Commit();
		return true;
	}

	bool Laplacian(
		Bytes out, ConstBytes positions, ConstBytes offsets, ConstBytes neighbours, std::optional<ConstBytes> mask,
		uint32_t iterations, float amount
	) {
		if (offsets.size() < 4 || offsets.size() % 4 != 0 || iterations > MAX_LAPLACIAN_ITERATIONS) return false;
		const uint64_t count = offsets.size() / 4 - 1;
		if (count > UINT32_MAX || !Fits(out.size(), Words(count * 3)) || !Fits(positions.size(), Words(count * 3))) {
			return false;
		}
		if (mask && !Fits(mask->size(), Words(count))) return false;
		const uint64_t neighbourCount = neighbours.size() / 4;
		std::vector<uint32_t> oScratch, nScratch;
		std::vector<float> mScratch;
		const uint32_t *offset = View<uint32_t>(offsets, count + 1, oScratch);
		uint32_t previous = 0;
		for (size_t v = 0; v <= count; ++v) {
			if (offset[v] < previous || offset[v] > neighbourCount) return false;
			previous = offset[v];
		}
		const uint32_t *neighbour = View<uint32_t>(neighbours, previous, nScratch);
		for (size_t i = 0; i < previous; ++i) {
			if (neighbour[i] >= count) return false;
		}
		const float *scale = mask ? View<float>(*mask, count, mScratch) : nullptr;

		std::vector<float> current(count * 3);
		std::memcpy(current.data(), positions.data(), current.size() * 4);
		std::vector<float> next(current.size());
		for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
			const float *here = current.data();
			float *there = next.data();
			for (size_t v = 0; v < count; ++v) {
				const uint32_t begin = offset[v];
				const uint32_t end = offset[v + 1];
				const float weight = scale ? amount * scale[v] : amount;
				const float *self = here + v * 3;
				float *moved = there + v * 3;
				if (begin == end || weight == 0.0f) {
					moved[0] = self[0];
					moved[1] = self[1];
					moved[2] = self[2];
					continue;
				}
				double mx = 0.0, my = 0.0, mz = 0.0;
				for (uint32_t i = begin; i < end; ++i) {
					const float *n = here + size_t(neighbour[i]) * 3;
					mx += n[0];
					my += n[1];
					mz += n[2];
				}
				const double inverse = 1.0 / double(end - begin);
				const float tx = static_cast<float>(mx * inverse);
				const float ty = static_cast<float>(my * inverse);
				const float tz = static_cast<float>(mz * inverse);
				const float sx = weight * (tx - self[0]);
				const float sy = weight * (ty - self[1]);
				const float sz = weight * (tz - self[2]);
				moved[0] = self[0] + sx;
				moved[1] = self[1] + sy;
				moved[2] = self[2] + sz;
			}
			current.swap(next);
		}
		std::memcpy(out.data(), current.data(), current.size() * 4);
		return true;
	}

	namespace {

		// Shared by `Gather` and `Scatter`: validates, then copies element by
		// element. `gather` picks which side the index stream addresses.
		bool Remap(Bytes out, ConstBytes input, ConstBytes indices, uint32_t width, bool gather) {
			if (width == 0 || indices.size() % 4 != 0) return false;
			const uint64_t elements = indices.size() / 4;
			const uint64_t elementBytes = Words(width);
			const ConstBytes dense = gather ? ConstBytes(out) : input;
			const ConstBytes sparse = gather ? input : ConstBytes(out);
			if (!Fits(dense.size(), elements * elementBytes)) return false;
			const uint64_t sparseCount = sparse.size() / elementBytes;
			std::vector<uint32_t> iScratch;
			const uint32_t *index = View<uint32_t>(indices, elements, iScratch);
			for (size_t i = 0; i < elements; ++i) {
				if (index[i] >= sparseCount) return false;
			}

			// A copy when the two overlap, so a slot read later is not one this
			// call already wrote.
			std::vector<std::byte> copy;
			const std::byte *source = input.data();
			const bool overlaps = input.data() < out.data() + out.size() && out.data() < input.data() + input.size();
			if (overlaps) {
				copy.assign(input.begin(), input.end());
				source = copy.data();
			}
			for (size_t i = 0; i < elements; ++i) {
				const uint64_t from = gather ? index[i] : i;
				const uint64_t to = gather ? i : index[i];
				std::memcpy(out.data() + to * elementBytes, source + from * elementBytes, elementBytes);
			}
			return true;
		}
	}

	bool Gather(Bytes out, ConstBytes input, ConstBytes indices, uint32_t width) {
		return Remap(out, input, indices, width, true);
	}

	bool Scatter(Bytes out, ConstBytes input, ConstBytes indices, uint32_t width) {
		return Remap(out, input, indices, width, false);
	}
}
