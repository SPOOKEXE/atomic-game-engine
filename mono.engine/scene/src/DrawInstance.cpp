#include <engine/scene/DrawInstance.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace engine::scene {

	namespace {
		// Below this a value is arithmetic noise rather than an author's
		// intent. A tween settling on "opaque" lands a few millionths off, and
		// paying a sort and a pipeline switch for that is paying for nothing.
		constexpr float TRANSPARENCY_EPSILON = 1.0f / 1024.0f;

		// **The bit pattern, not the value.** `-0.0f` and `0.0f` compare equal
		// and hash apart, so a sign flip through zero reports a change that is
		// not one. That is the harmless direction: a false difference costs a
		// redraw, a false match shows a stale image, and only one of those is a
		// bug somebody has to find.
		//
		// NaN falls the same way — two NaNs with different payloads hash apart —
		// and for the same reason it does not matter.
		uint32_t BitsOf(float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		// Two 32-bit fields as one 64-bit word.
		//
		// **`MixSignature` costs the same for one bit as for sixty-four**, so
		// feeding it a 32-bit value wastes half of every mix — and every field
		// on a `DrawInstance` that a surface can see is 32 bits or fewer. Pairing
		// them halves the number of mixes without folding in one bit less.
		//
		// Order within the pair is fixed and arbitrary; what matters is that it
		// is the same on both sides of every comparison, which it is because
		// there is one function.
		constexpr uint64_t Pair(uint32_t high, uint32_t low) {
			return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
		}

		// The seed. FNV's 64-bit offset basis, used as a starting constant
		// rather than as part of FNV — what follows is not FNV, and picking a
		// well-known non-zero start is only so an empty list does not hash to
		// zero and collide with an uninitialised field.
		constexpr uint64_t SIGNATURE_SEED = 0xCBF29CE484222325ull;
	}

	bool IsTransparent(const DrawInstance &instance) {
		return instance.Transparency > TRANSPARENCY_EPSILON;
	}

	uint64_t MixSignature(uint64_t hash, uint64_t word) {
		// Boost's `hash_combine` constant and shifts. Chosen because it is the
		// one every reader has already seen: the avalanche matters far less here
		// than for a hash table — a collision costs one skipped redraw of one
		// surface, on one frame — and an unfamiliar mixer would invite somebody
		// to improve it.
		return hash ^ (word + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2));
	}

	uint64_t SignatureOf(std::span<const DrawInstance> instances) {
		// **Four chains rather than one, because this is latency-bound and not
		// throughput-bound.** `MixSignature` is a shift, a shift, an add and an
		// xor, and every one of them needs the previous hash — so folding
		// eighteen fields into one accumulator is an eighteen-deep dependency
		// chain per instance that a superscalar core can do nothing with. It
		// measured at 13 ns an instance in `engine.scene.bench.ordering`, which
		// is thirteen times what ordering the whole opaque list costs and made
		// the guard dearer than the redraw it exists to skip: sixteen surfaces
		// over a ten-thousand-instance list is about 2 ms of a frame spent
		// deciding that nothing had changed.
		//
		// Independent lanes let four mixes be in flight at once, and pairing the
		// 32-bit fields halves how many there are. Both are pure rearrangement —
		// every field still lands in the result, still by its bit pattern, and
		// still field-wise rather than over the object representation, which is
		// what `tests/DrawInstance.cpp` pins and what keeps `Reserved` out of it.
		//
		// **The value changes, and nothing may depend on the old one.** A
		// signature is compared against another produced by this same function
		// within one run; `scene/Registration.cpp` deliberately serialises a
		// `RenderedSignature` as nothing and reads it back as a default, so
		// there is no file and no wire carrying one.
		uint64_t a = SIGNATURE_SEED;
		uint64_t b = SIGNATURE_SEED;
		uint64_t c = SIGNATURE_SEED;
		uint64_t d = SIGNATURE_SEED;

		for (const DrawInstance &instance : instances) {
			const core::CFrame &frame = instance.Frame;

			a = MixSignature(a, Pair(BitsOf(frame.Position.X), BitsOf(frame.Position.Y)));
			b = MixSignature(b, Pair(BitsOf(frame.Position.Z), BitsOf(frame.QuaternionX)));
			c = MixSignature(c, Pair(BitsOf(frame.QuaternionY), BitsOf(frame.QuaternionZ)));
			d = MixSignature(d, Pair(BitsOf(frame.QuaternionW), BitsOf(instance.HalfExtent.X)));

			a = MixSignature(a, Pair(BitsOf(instance.HalfExtent.Y), BitsOf(instance.HalfExtent.Z)));
			b = MixSignature(b, Pair(BitsOf(instance.Tint.R), BitsOf(instance.Tint.G)));
			c = MixSignature(c, Pair(BitsOf(instance.Tint.B), BitsOf(instance.Transparency)));
			// **The mesh alone, where it used to be the mesh and the material.**
			// The material name is gone from a `DrawInstance` — a material is
			// content now and what it resolves to is `Texture`, which is folded
			// in below. Keeping both would have signed one fact twice.
			d = MixSignature(d, Pair(instance.Mesh.Id(), 0u));

			// `Surface` is signed and -1 means "no surface", so it is widened
			// through `uint8_t` exactly as it was before: sign-extending it
			// would fold in twenty-four bits that say nothing.
			a = MixSignature(a, Pair(static_cast<uint8_t>(instance.Surface), instance.CastShadow ? 1u : 0u));

			// The three fields v0.9 added. Folded in for the reason every other
			// field is: this signature answers "would drawing this again produce
			// the same image", and a texture swap, a tag change or an alpha mode
			// change all produce a different one.
			b = MixSignature(b, Pair(instance.Texture.Id(), static_cast<uint32_t>(instance.TagMask)));
			c = MixSignature(c, static_cast<uint64_t>(instance.Alpha));
		}

		// Combined in a fixed order, so the four lanes give one value. The
		// combine is four mixes for the whole list rather than per instance, so
		// its cost rounds to nothing.
		return MixSignature(MixSignature(a, b), MixSignature(c, d));
	}

	size_t OrderForDrawing(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	) {
		// **Every instance, which is what this always meant.** The body moved to
		// `OrderSubset` when the culling became graph nodes and a pass started
		// being handed a list rather than the whole world; the sort itself is
		// unchanged and is deliberately not written twice — see the comment on
		// the reverse, which is there because a test caught it.
		order.resize(instances.size());
		for (size_t index = 0; index < instances.size(); index++) {
			order[index] = static_cast<uint32_t>(index);
		}

		std::vector<uint32_t> ordered;
		const size_t opaque = OrderSubset(instances, order, eye, ordered);
		order = std::move(ordered);
		return opaque;
	}

	size_t OrderSubset(
		std::span<const DrawInstance> instances,
		std::span<const uint32_t> from,
		const core::Vector3 &eye,
		std::vector<uint32_t> &order
	) {
		// **Resized rather than cleared and filled.** A steady scene calls this
		// every frame per view, and `clear` followed by `push_back` would
		// value-initialise every element only to overwrite it a moment later.
		order.resize(from.size());

		// The opaque head keeps the order the world produced it in, so an opaque
		// scene comes out of this exactly as it went in — which is what makes a
		// recording of one replay, and what makes the cost on a scene with no
		// transparency a single pass and no comparisons.
		size_t opaque = 0;
		size_t transparent = from.size();

		for (const uint32_t index : from) {
			// **A bad index is dropped rather than dereferenced.** A list is
			// whatever a chain of filter nodes produced, and one of them naming
			// an instance that no longer exists is a mis-wired pipeline — which
			// should be a missing object, not a read off the end.
			if (index >= instances.size()) {
				order[--transparent] = index;
				continue;
			}

			if (IsTransparent(instances[index])) {
				// Filled from the back, so both halves are written in one pass
				// without knowing either count in advance.
				order[--transparent] = index;
			} else {
				order[opaque++] = index;
			}
		}

		if (opaque == from.size()) {
			return opaque;
		}

		// **The tail came out reversed, and a stable sort preserves that.**
		// This looked unnecessary until a test put three panes at one distance
		// and got them back `{2, 1, 0}`: stability only promises that *equal*
		// elements keep the order they were given, and the order they were given
		// was backwards. Reversing here is what makes "equal distances keep
		// world order" true, and that is what makes a recording of a scene with
		// coincident transparent faces replay.
		std::reverse(order.begin() + static_cast<ptrdiff_t>(opaque), order.end());

		// Farthest first. Squared distance, because the square root is monotonic
		// and cannot change an ordering — and this runs over every transparent
		// instance every frame per view.
		std::stable_sort(
			order.begin() + static_cast<ptrdiff_t>(opaque), order.end(), [&](uint32_t left, uint32_t right) {
				const float far = std::numeric_limits<float>::max();
				const float leftDistance =
					left < instances.size() ? (instances[left].Frame.Position - eye).MagnitudeSquared() : far;
				const float rightDistance = right < instances.size()
												? (instances[right].Frame.Position - eye).MagnitudeSquared()
												: far;
				return leftDistance > rightDistance;
			}
		);

		return opaque;
	}

	size_t PartitionCasters(std::span<const DrawInstance> instances, std::span<uint32_t> order) {
		const auto casts = [instances](uint32_t index) {
			return index < instances.size() && instances[index].CastShadow;
		};

		return static_cast<size_t>(
			std::distance(order.begin(), std::stable_partition(order.begin(), order.end(), casts))
		);
	}

	size_t PartitionSurfaces(std::span<const DrawInstance> instances, std::span<uint32_t> order) {
		const auto shows = [instances](uint32_t index) {
			return index < instances.size() && instances[index].Surface >= 0;
		};

		// **The scan first, because the common scene has no mirror in it.**
		// `stable_partition` allocates a temporary buffer whatever it finds, and
		// with every `Surface` at its default of -1 the partition is a provable
		// no-op — so a mirrorless frame was paying an allocation and two passes
		// to reorder nothing.
		if (std::none_of(order.begin(), order.end(), shows)) {
			return 0;
		}

		const auto boundary = std::stable_partition(order.begin(), order.end(), [&shows](uint32_t index) {
			return !shows(index);
		});
		return static_cast<size_t>(std::distance(boundary, order.end()));
	}

	namespace {
		// Which surface an ordered entry shows, or -1 when it shows none.
		//
		// The bounds test is the same one `PartitionSurfaces` makes and for the
		// same reason: an order may name an index past the end of the list it
		// was built for, and the four copies of that guard which had already
		// drifted are why these predicates live in one file.
		int8_t SurfaceOf(std::span<const DrawInstance> instances, uint32_t index) {
			return index < instances.size() ? instances[index].Surface : static_cast<int8_t>(-1);
		}

		// Groups a run of mirrors by the surface each one shows.
		//
		// **A sort rather than sixteen partitions**, because the run is a
		// handful of panes and a comparison sort over it is cheaper to read than
		// a loop of `stable_partition` calls each allocating its own buffer.
		// Stable, so the order within one surface is whatever the caller
		// established — world order in the opaque head, back-to-front in the
		// blended tail.
		//
		// An index at or above `MAX_SURFACES` sorts to the end and is left out
		// of every run, which is what makes it drop out of the frame rather than
		// index past an array.
		void GroupBySurface(std::span<const DrawInstance> instances, std::span<uint32_t> order) {
			std::stable_sort(order.begin(), order.end(), [instances](uint32_t left, uint32_t right) {
				return SurfaceOf(instances, left) < SurfaceOf(instances, right);
			});
		}

		// Finds where each surface's entries sit in a run that has been grouped.
		//
		// @param instances The draw list the order refers to.
		// @param order     The grouped run.
		// @param base      Where that run starts in the whole order, because the
		//                  runs a pass submits are offsets into the buffer and
		//                  not into this span.
		// @param casters   Whether to partition each group by shadow casting and
		//                  record how many cast. Only the opaque run wants it —
		//                  a blended fragment never reaches the shadow pass.
		// @param out       The plan's per-surface runs, written for the indices
		//                  that appear and left alone for the ones that do not.
		void RecordRuns(
			std::span<const DrawInstance> instances,
			std::span<uint32_t> order,
			uint32_t base,
			bool casters,
			SurfaceRun (&out)[MAX_SURFACES]
		) {
			size_t start = 0;
			while (start < order.size()) {
				const int8_t surface = SurfaceOf(instances, order[start]);
				size_t end = start;
				while (end < order.size() && SurfaceOf(instances, order[end]) == surface) {
					end++;
				}

				// Negative cannot appear — the caller passes only the mirror run
				// — but an index past the cap can, and it is dropped here rather
				// than written past the end of the array.
				if (surface >= 0 && static_cast<uint8_t>(surface) < MAX_SURFACES) {
					SurfaceRun &run = out[surface];
					const auto first = base + static_cast<uint32_t>(start);
					const auto count = static_cast<uint32_t>(end - start);

					if (casters) {
						run.OpaqueFirst = first;
						run.OpaqueCount = count;
						run.OpaqueCasters = static_cast<uint32_t>(
							PartitionCasters(instances, order.subspan(start, end - start))
						);
					} else {
						run.BlendedFirst = first;
						run.BlendedCount = count;
					}
				}

				start = end;
			}
		}
	}

	void GroupSurfaces(
		std::span<const DrawInstance> instances,
		std::span<uint32_t> order,
		uint32_t base,
		bool opaque,
		SurfaceRun (&runs)[MAX_SURFACES]
	) {
		if (order.empty()) {
			return;
		}

		GroupBySurface(instances, order);
		RecordRuns(instances, order, base, opaque, runs);
	}

	ScenePlan OrderScene(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	) {
		ScenePlan plan;

		const size_t opaque = OrderForDrawing(instances, eye, order);
		plan.Opaque = static_cast<uint32_t>(opaque);
		plan.Transparent = static_cast<uint32_t>(instances.size() - opaque);

		// **Mirrors to the back of the blended tail, keeping their sort.** See
		// `ScenePlan::TransparentSurfaces` for why they go last and what that
		// costs.
		if (plan.Transparent > 0) {
			plan.TransparentSurfaces = static_cast<uint32_t>(
				PartitionSurfaces(instances, std::span<uint32_t>(order.data() + opaque, plan.Transparent))
			);

			// **Grouped by index, which costs the blended tail part of its
			// depth order and is worth it.** Two mirrors at different depths are
			// drawn in index order rather than far-to-near, so a nearer pane may
			// be blended before a farther one. That was already true across the
			// mirror/non-mirror boundary — `TransparentSurfaces` documents the
			// trade — and this extends it between mirrors, because a texture
			// per surface means a sampler binding per surface and an order that
			// interleaved them would need one draw call per pane.
			//
			// Within one surface the far-to-near sort survives, because the sort
			// is stable.
			if (plan.TransparentSurfaces > 0) {
				const std::span<uint32_t> mirrors(
					order.data() + opaque + (plan.Transparent - plan.TransparentSurfaces),
					plan.TransparentSurfaces
				);
				GroupSurfaces(
					instances,
					mirrors,
					static_cast<uint32_t>(opaque) + (plan.Transparent - plan.TransparentSurfaces),
					false,
					plan.Runs
				);
			}
		}

		if (opaque == 0) {
			return plan;
		}

		// **Mirrors to the back of the opaque head, so the surface pass can
		// skip them.** A mirror sits between its own reflection camera and the
		// world — the camera is *behind* the plane looking through it — so
		// drawing the pane into its own reflection fills the texture with the
		// pane, and the mirror then shows itself. That reads as a mirror which
		// is not working at all rather than as an ordering mistake.
		//
		// Physically right as well as necessary: nothing sees itself in its own
		// reflection.
		const std::span<uint32_t> head(order.data(), opaque);
		plan.Surfaces = static_cast<uint32_t>(PartitionSurfaces(instances, head));
		plan.Reflected = plan.Opaque - plan.Surfaces;

		// Casters within the non-mirror run. Nested rather than replacing the
		// partition above, for the reason `ScenePlan::SurfaceCasters` gives.
		plan.ReflectedCasters =
			static_cast<uint32_t>(PartitionCasters(instances, head.subspan(0, plan.Reflected)));

		// **The mirrors are grouped by surface and then split by caster inside
		// each group**, which is the order the two demands can both be met in.
		// The surface pass has to bind one texture per index, so grouping comes
		// first; the shadow pass only needs its casters contiguous *somewhere*
		// it can be told about, and `SurfaceRun::OpaqueCasters` tells it.
		if (plan.Surfaces > 0) {
			const std::span<uint32_t> mirrors = head.subspan(plan.Reflected, plan.Surfaces);
			GroupSurfaces(instances, mirrors, plan.Reflected, true, plan.Runs);

			for (const SurfaceRun &run : plan.Runs) {
				plan.SurfaceCasters += run.OpaqueCasters;
			}
		}

		return plan;
	}
}
