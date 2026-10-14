#include "WeatherSample.hpp"

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace engine::weather::detail {

// At -O0 the intrinsic wrappers measured slower than the scalar reference. Keep that build scalar.
#if (defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_NEON__)) &&                                   \
	(!defined(__GNUC__) || defined(__OPTIMIZE__))
	namespace {

#if defined(__SSE2__)
		using Lanes = __m128i;
		Lanes Broadcast(uint32_t value) {
			return _mm_set1_epi32(static_cast<int32_t>(value));
		}
		Lanes Load(const std::array<uint32_t, 4> &values) {
			return _mm_loadu_si128(reinterpret_cast<const __m128i *>(values.data()));
		}
		void Store(Lanes value, std::array<uint32_t, 4> &output) {
			_mm_storeu_si128(reinterpret_cast<__m128i *>(output.data()), value);
		}
		Lanes Add(Lanes first, Lanes second) {
			return _mm_add_epi32(first, second);
		}
		Lanes Subtract(Lanes first, Lanes second) {
			return _mm_sub_epi32(first, second);
		}
		Lanes Xor(Lanes first, Lanes second) {
			return _mm_xor_si128(first, second);
		}
		Lanes And(Lanes first, Lanes second) {
			return _mm_and_si128(first, second);
		}
		Lanes ShiftRight(Lanes value, uint32_t shift) {
			return _mm_srl_epi32(value, _mm_cvtsi32_si128(static_cast<int32_t>(shift)));
		}
		Lanes Multiply(Lanes first, Lanes second) {
			// SSE2 multiplies the even lanes to uint64. Interleave low words from both pairs.
			const Lanes even = _mm_mul_epu32(first, second);
			const Lanes odd = _mm_mul_epu32(_mm_srli_si128(first, 4), _mm_srli_si128(second, 4));
			return _mm_unpacklo_epi32(
				_mm_shuffle_epi32(even, _MM_SHUFFLE(2, 0, 2, 0)),
				_mm_shuffle_epi32(odd, _MM_SHUFFLE(2, 0, 2, 0))
			);
		}
#else
		using Lanes = uint32x4_t;
		Lanes Broadcast(uint32_t value) {
			return vdupq_n_u32(value);
		}
		Lanes Load(const std::array<uint32_t, 4> &values) {
			return vld1q_u32(values.data());
		}
		void Store(Lanes value, std::array<uint32_t, 4> &output) {
			vst1q_u32(output.data(), value);
		}
		Lanes Add(Lanes first, Lanes second) {
			return vaddq_u32(first, second);
		}
		Lanes Subtract(Lanes first, Lanes second) {
			return vsubq_u32(first, second);
		}
		Lanes Xor(Lanes first, Lanes second) {
			return veorq_u32(first, second);
		}
		Lanes And(Lanes first, Lanes second) {
			return vandq_u32(first, second);
		}
		Lanes ShiftRight(Lanes value, uint32_t shift) {
			return vshlq_u32(value, vdupq_n_s32(-static_cast<int32_t>(shift)));
		}
		Lanes Multiply(Lanes first, Lanes second) {
			return vmulq_u32(first, second);
		}
#endif

		Lanes HashLanes(Lanes value) {
			value = Xor(value, ShiftRight(value, 16));
			value = Multiply(value, Broadcast(0x7feb352du));
			value = Xor(value, ShiftRight(value, 15));
			value = Multiply(value, Broadcast(0x846ca68bu));
			return Xor(value, ShiftRight(value, 16));
		}

		Lanes SmoothLanes(Lanes fraction) {
			return ShiftRight(
				Add(Multiply(
						Multiply(fraction, fraction),
						Subtract(Broadcast(768), Multiply(Broadcast(2), fraction))
					),
					Broadcast(32768)),
				16
			);
		}

		Lanes InterpolateLanes(Lanes first, Lanes second, Lanes fraction) {
			return ShiftRight(
				Add(Add(Multiply(first, Subtract(Broadcast(256), fraction)), Multiply(second, fraction)),
					Broadcast(128)),
				8
			);
		}

		struct PackedAxis {
			Lanes Lattice;
			Lanes Fraction;
			Lanes Mask;
		};

		PackedAxis AxisLanes(Lanes coordinate, uint32_t displacement, uint32_t shift) {
			const uint32_t fraction = displacement & 255u;
			const Lanes shifted =
				And(Subtract(coordinate, Broadcast((displacement >> 8u) + (fraction != 0 ? 1u : 0u))),
					Broadcast(0x00ffffffu));
			const uint32_t remainder = fraction != 0 ? 256u - fraction : 0u;
			const Lanes cellFraction = ShiftRight(
				Add(Multiply(And(shifted, Broadcast((1u << shift) - 1u)), Broadcast(256)),
					Broadcast(remainder)),
				shift
			);
			return {ShiftRight(shifted, shift), SmoothLanes(cellFraction), Broadcast(0x00ffffffu >> shift)};
		}

		Lanes CornerLanes(uint32_t seed, uint32_t purpose, uint32_t epoch, Lanes x, Lanes y, Lanes z) {
			const Lanes key =
				Xor(Xor(Xor(Broadcast(seed ^ purpose ^ Hash(epoch)), Multiply(x, Broadcast(0x9e3779b9u))),
						Multiply(y, Broadcast(0x85ebca6bu))),
					Multiply(z, Broadcast(0xc2b2ae35u)));
			return ShiftRight(HashLanes(key), 24);
		}

		Lanes NoiseAtLanes(
			uint32_t seed, uint32_t purpose, uint32_t epoch, PackedAxis x, PackedAxis y, PackedAxis z
		) {
			const Lanes nextX = And(Add(x.Lattice, Broadcast(1)), x.Mask);
			struct LaneWord {
				Lanes Value;
			};
			std::array<LaneWord, 4> rows{};
			for (uint32_t index = 0; index < 4; index++) {
				const Lanes yCorner = And(Add(y.Lattice, Broadcast(index & 1u)), y.Mask);
				const Lanes zCorner = And(Add(z.Lattice, Broadcast(index >> 1u)), z.Mask);
				rows[index].Value = InterpolateLanes(
					CornerLanes(seed, purpose, epoch, x.Lattice, yCorner, zCorner),
					CornerLanes(seed, purpose, epoch, nextX, yCorner, zCorner),
					x.Fraction
				);
			}
			return InterpolateLanes(
				InterpolateLanes(rows[0].Value, rows[1].Value, y.Fraction),
				InterpolateLanes(rows[2].Value, rows[3].Value, y.Fraction),
				z.Fraction
			);
		}

		struct PackedCoordinates {
			Lanes X;
			Lanes Y;
			Lanes Z;
		};

		Lanes NoiseLanes(
			const WeatherDefinition &definition,
			const WeatherTime &time,
			const PackedCoordinates &coordinates,
			uint32_t purpose,
			uint32_t shift
		) {
			const PackedAxis x = AxisLanes(coordinates.X, time.DisplacementQ8[0], shift);
			const PackedAxis y = AxisLanes(coordinates.Y, time.DisplacementQ8[1], shift);
			const PackedAxis z = AxisLanes(coordinates.Z, time.DisplacementQ8[2], shift);
			const uint32_t seed = definition.Seed ^ Hash(definition.LayerKey);
			const uint32_t epoch = static_cast<uint32_t>(time.PhaseTick >> 8u);
			const uint32_t phase = static_cast<uint32_t>(time.PhaseTick & 255u);
			const Lanes smoothPhase = SmoothLanes(Broadcast(phase));
			return InterpolateLanes(
				NoiseAtLanes(seed, purpose, epoch, x, y, z),
				NoiseAtLanes(seed, purpose, epoch + 1u, x, y, z),
				smoothPhase
			);
		}
	}
#endif

	size_t EvaluatePackedCells(
		const WeatherDefinition &definition,
		const WeatherTime &time,
		std::span<const CellCoordinate> coordinates,
		std::span<EncodedCell> output
	) {
#if (defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_NEON__)) &&                                   \
	(!defined(__GNUC__) || defined(__OPTIMIZE__))
		const size_t packedCount = coordinates.size() - coordinates.size() % 4;
		const uint32_t shift = std::min(definition.FeatureShift, 10u);
		const std::array purposes{
			0x46524f4eu,
			0x4d4f4953u,
			0x424f4459u,
			0x45524f53u,
			0x54454d50u,
			0x57494e44u,
			0x57494e45u,
			0x57494e46u
		};
		for (size_t index = 0; index < packedCount; index += 4) {
			std::array<uint32_t, 4> x{}, y{}, z{};
			for (size_t lane = 0; lane < 4; lane++) {
				x[lane] = static_cast<uint32_t>(coordinates[index + lane].X);
				y[lane] = static_cast<uint32_t>(coordinates[index + lane].Y);
				z[lane] = static_cast<uint32_t>(coordinates[index + lane].Z);
			}
			const PackedCoordinates packedCoordinates{Load(x), Load(y), Load(z)};
			std::array<std::array<uint32_t, 4>, 8> fields{};
			for (size_t field = 0; field < fields.size(); field++) {
				const uint32_t fieldShift = field == 0	 ? std::min(shift + 2u, 12u)
											: field == 3 ? (shift > 2u ? shift - 2u : 0u)
														 : shift;
				Store(
					NoiseLanes(definition, time, packedCoordinates, purposes[field], fieldShift),
					fields[field]
				);
			}
			for (size_t lane = 0; lane < 4; lane++) {
				std::array<uint32_t, 8> noise{};
				for (size_t field = 0; field < noise.size(); field++)
					noise[field] = fields[field][lane];
				output[index + lane] = FinishWeatherSample(definition, coordinates[index + lane], noise);
			}
		}
		return packedCount;
#else
		(void)definition;
		(void)time;
		(void)coordinates;
		(void)output;
		return 0;
#endif
	}
}
