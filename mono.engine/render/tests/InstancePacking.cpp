// What the packed instance row costs in accuracy.
//
// **The whole point of these cases is the size of a number, not a boolean.**
// `GpuInstance` went from ninety-six bytes to thirty-six by quantising the
// rotation to four sixteen-bit codes and the colour to RGBA8, and "is that
// visible" is a question with an answer in metres. So the reference here is the
// matrix the old layout uploaded, rebuilt inline, and every case measures how far
// the packed row lands from it at the corners of the geometry it draws - which
// is where the error is largest and where a seam between two parts would show it.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <InstancePacking.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

TEST_SUITE_ID("engine.render.instancepacking")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Vector3;
using engine::render::DecodeSnorm16;
using engine::render::EncodeSnorm16;
using engine::render::GpuInstance;
using engine::render::MeshEntry;
using engine::render::ModelMatrixOf;
using engine::render::PackColour;
using engine::render::PackRotation;
using engine::render::ToGpu;
using engine::render::UnpackColour;
using engine::render::UnpackRotation;
using engine::scene::DrawInstance;

namespace {
	// The model matrix `ToGpu` built before v0.19, rebuilt here.
	//
	// **Copied rather than called, and it has to be: the function it came from
	// no longer exists.** This is the baseline every accuracy case below is
	// measured against, so it is written out in full where a reader can check it
	// against the packed path line by line rather than trusting that a helper
	// somewhere still means what it did.
	glm::mat4 ReferenceModel(const DrawInstance &instance, const MeshEntry &mesh) {
		const auto stretch = [](float half, float extent) {
			return extent > 1e-6f ? half / extent : half * 2.0f;
		};
		const glm::vec3 scale{
			stretch(instance.HalfExtent.X, mesh.Extent.X),
			stretch(instance.HalfExtent.Y, mesh.Extent.Y),
			stretch(instance.HalfExtent.Z, mesh.Extent.Z),
		};

		glm::mat4 model = instance.Frame.ToMatrix();
		model[0] *= scale.x;
		model[1] *= scale.y;
		model[2] *= scale.z;

		const glm::vec3 centre{mesh.Centre.X, mesh.Centre.Y, mesh.Centre.Z};
		model[3] -= glm::vec4(glm::mat3(model) * centre, 0.0f);
		return model;
	}

	// A mesh whose own box is the unit cube about its origin, which is what every
	// built-in shape is.
	MeshEntry UnitMesh() {
		MeshEntry mesh;
		mesh.Centre = Vector3(0.0f, 0.0f, 0.0f);
		mesh.Extent = Vector3(0.5f, 0.5f, 0.5f);
		return mesh;
	}

	// The largest distance any corner of the mesh's own box moves between the two
	// matrices, in metres.
	//
	// **Corners rather than the origin, because the origin is exact.** Position
	// is still a full float and the rotation error only shows up multiplied by
	// how far a point is from the pivot, so measuring at the centre of a part
	// would report zero for a body whose ends had visibly sheared.
	float WorstCornerDrift(const glm::mat4 &reference, const glm::mat4 &packed, const MeshEntry &mesh) {
		float worst = 0.0f;
		for (int corner = 0; corner < 8; corner++) {
			const glm::vec4 at{
				(corner & 1) != 0 ? mesh.Extent.X : -mesh.Extent.X,
				(corner & 2) != 0 ? mesh.Extent.Y : -mesh.Extent.Y,
				(corner & 4) != 0 ? mesh.Extent.Z : -mesh.Extent.Z,
				1.0f,
			};
			worst = std::max(worst, glm::length(glm::vec3(reference * at) - glm::vec3(packed * at)));
		}
		return worst;
	}

	// A rotation from three angles, so a case can sweep orientations without
	// depending on a random device.
	glm::quat RotationAt(float yaw, float pitch, float roll) {
		return glm::quat(glm::vec3{pitch, yaw, roll});
	}

	DrawInstance PartAt(const glm::quat &rotation, const Vector3 &position, float half) {
		DrawInstance instance;
		instance.Frame = CFrame(position, rotation);
		instance.HalfExtent = Vector3(half, half, half);
		return instance;
	}
}

TEST_CASE("a signed-normalised code round-trips the ends exactly", "[render][instancepacking]") {
	// **The two values that decide whether the scale constant is right.** 32767
	// is the divisor every graphics API decodes SNORM16 by; encoding against
	// 32768 instead would put `1.0` one code short and leave the identity
	// rotation slightly not-identity, which is a static scene shimmering.
	CHECK(DecodeSnorm16(EncodeSnorm16(1.0f)) == Approx(1.0f));
	CHECK(DecodeSnorm16(EncodeSnorm16(-1.0f)) == Approx(-1.0f));
	CHECK(DecodeSnorm16(EncodeSnorm16(0.0f)) == Approx(0.0f));

	// Out of range clamps rather than wrapping. A component of a normalised
	// quaternion cannot exceed one, but a caller handing over an unnormalised
	// one before `PackRotation` gets to it would otherwise alias to the far end
	// of the range - a small error becoming the opposite rotation.
	CHECK(DecodeSnorm16(EncodeSnorm16(4.0f)) == Approx(1.0f));
	CHECK(DecodeSnorm16(EncodeSnorm16(-4.0f)) == Approx(-1.0f));

	// The resolution, stated so the number the accuracy cases below expect has
	// somewhere to come from.
	CHECK(DecodeSnorm16(EncodeSnorm16(0.5f)) == Approx(0.5f).margin(1.0 / 32767.0));
}

TEST_CASE("the identity rotation survives packing exactly", "[render][instancepacking]") {
	// **The case a still scene lives on.** Most parts in most worlds are
	// axis-aligned, so if the identity did not round-trip bit-exactly then the
	// commonest geometry in the engine would be the geometry carrying the error.
	// Every component is 0 or 1, and both are exact codes.
	const glm::quat identity{1.0f, 0.0f, 0.0f, 0.0f};
	const glm::quat unpacked = UnpackRotation(PackRotation(identity));

	CHECK(unpacked.w == Approx(1.0f));
	CHECK(unpacked.x == Approx(0.0f));
	CHECK(unpacked.y == Approx(0.0f));
	CHECK(unpacked.z == Approx(0.0f));

	// And so do the quarter turns, for the same reason: a component is 0, 1 or
	// the square root of a half, and only the last of those is quantised.
	const glm::quat quarter = RotationAt(1.5707963f, 0.0f, 0.0f);
	const glm::quat quarterBack = UnpackRotation(PackRotation(quarter));
	CHECK(
		glm::length(
			glm::vec3(quarterBack.x, quarterBack.y, quarterBack.z) -
			glm::vec3(quarter.x, quarter.y, quarter.z)
		) < 1e-4f
	);
}

TEST_CASE("an unnormalised rotation is normalised rather than carried", "[render][instancepacking]") {
	// **A change from the matrix layout, and a fix.** That one handed
	// `CFrame::Rotation()` straight to `glm::toMat4`, which assumes a unit
	// quaternion - a caller who stored one twice as long silently drew geometry
	// four times the size. A quantised component has to be in [-1, 1] to mean
	// anything, so the normalise is not optional here, and doing it turns that
	// authoring bug into a no-operation.
	const glm::quat doubled{2.0f, 0.0f, 0.0f, 0.0f};
	const glm::quat unpacked = UnpackRotation(PackRotation(doubled));
	CHECK(unpacked.w == Approx(1.0f));

	// A rotation with no length at all is a caller who never set one. It packs
	// as the identity rather than as NaN, which would take the whole draw list
	// with it.
	const glm::quat empty{0.0f, 0.0f, 0.0f, 0.0f};
	const glm::quat fallback = UnpackRotation(PackRotation(empty));
	CHECK(fallback.w == Approx(1.0f));
	CHECK(std::isfinite(fallback.x));
}

TEST_CASE("a packed part draws within a tenth of a millimetre per metre", "[render][instancepacking]") {
	// **The number this whole change has to justify, measured rather than
	// asserted.** Sixty bytes an instance were saved by quantising the rotation;
	// what that costs is how far a corner of a part lands from where the old
	// float matrix would have put it, and it scales with how far that corner is
	// from the part's pivot.
	//
	// **It measures 6.11e-5 metres per metre of radius**, which is 2/32767 to
	// three figures - so the renormalise in `UnpackRotation` removed the radial
	// half of the error outright and what is left is one code of angle either
	// way, exactly as designed. A one-metre part drifts six hundredths of a
	// millimetre; reaching a millimetre takes a part sixteen metres from its own
	// pivot. The bound is `1e-4` rather than the measurement, so the case fails
	// on a change of encoding rather than on the last digit of a rounding mode.
	//
	// Even at the far end the two halves of a seam move together, because both
	// are drawn from the same quantised rotation.
	//
	// **A sweep rather than one orientation**, because the error is a property of
	// where a quaternion lands between two codes, and a single hand-picked
	// rotation can be lucky. 512 orientations across all three axes is enough
	// that the worst is a real worst.
	const MeshEntry mesh = UnitMesh();
	float worstRelative = 0.0f;

	for (int step = 0; step < 512; step++) {
		const float t = static_cast<float>(step);
		const glm::quat rotation = RotationAt(t * 0.37f, t * 0.11f, t * 0.53f);

		for (const float half : {0.5f, 4.0f, 20.0f}) {
			const DrawInstance instance = PartAt(rotation, Vector3(13.0f, -7.5f, 210.0f), half);
			const GpuInstance packed = ToGpu(instance, mesh);

			const float drift = WorstCornerDrift(ReferenceModel(instance, mesh), ModelMatrixOf(packed), mesh);

			// The radius the drift is measured against: how far a corner of this
			// part is from its own pivot.
			const float radius = std::sqrt(3.0f) * half;
			worstRelative = std::max(worstRelative, drift / radius);
		}
	}

	INFO("worst corner drift per metre of radius: " << worstRelative);
	CHECK(worstRelative < 1.0e-4f);
}

TEST_CASE("the mesh centre correction survives quantisation", "[render][instancepacking]") {
	// **The subtle half of `ToGpu`, and the one a careless port breaks.** A mesh
	// authored off its own origin is pulled back onto the part's origin by
	// subtracting the rotated, scaled centre - and the rotation that offset is
	// taken through has to be the *unpacked* one, because the unpacked one is
	// what the shader will rotate the geometry by. Cancelling with the exact
	// rotation instead leaves the mesh off by the quantisation error times the
	// centre offset, which for a model authored a long way from its origin is
	// not small.
	MeshEntry mesh = UnitMesh();
	mesh.Centre = Vector3(6.0f, -2.0f, 3.0f);

	const glm::quat rotation = RotationAt(0.6f, 1.1f, -0.4f);
	const DrawInstance instance = PartAt(rotation, Vector3(40.0f, 5.0f, -18.0f), 2.0f);
	const GpuInstance packed = ToGpu(instance, mesh);

	// The mesh's own centre must land exactly on the part's position, because
	// that is what the correction is for.
	const glm::vec3 centre =
		glm::vec3(ModelMatrixOf(packed) * glm::vec4(mesh.Centre.X, mesh.Centre.Y, mesh.Centre.Z, 1.0f));
	CHECK(centre.x == Approx(40.0f).margin(1e-4));
	CHECK(centre.y == Approx(5.0f).margin(1e-4));
	CHECK(centre.z == Approx(-18.0f).margin(1e-4));
}

TEST_CASE("a part is stretched into its own box, not scaled by it", "[render][instancepacking]") {
	// The `MeshPart.Size` semantic, unchanged by the new layout: the mesh's own
	// box is mapped exactly onto the part's, so a mesh authored twenty units tall
	// and a mesh authored at unit scale both fill the same part.
	MeshEntry tall = UnitMesh();
	tall.Extent = Vector3(0.5f, 10.0f, 0.5f);

	DrawInstance instance;
	instance.HalfExtent = Vector3(1.0f, 3.0f, 1.0f);
	const GpuInstance packed = ToGpu(instance, tall);

	CHECK(packed.Scale.x == Approx(2.0f));
	CHECK(packed.Scale.y == Approx(0.3f));
	CHECK(packed.Scale.z == Approx(2.0f));

	// The drawn corner is the part's own half-extent, whatever the mesh was
	// authored at.
	const glm::vec3 corner =
		glm::vec3(ModelMatrixOf(packed) * glm::vec4(tall.Extent.X, tall.Extent.Y, tall.Extent.Z, 1.0f));
	CHECK(corner.x == Approx(1.0f));
	CHECK(corner.y == Approx(3.0f));
	CHECK(corner.z == Approx(1.0f));
}

TEST_CASE("a mesh with no thickness keeps the old fallback", "[render][instancepacking]") {
	// The built-in plane is a quad whose Y extent is exactly zero, and a flat
	// mesh is an ordinary thing to author. Dividing by that extent would produce
	// an infinity that reaches the vertex stream; the rule is `HalfExtent * 2`,
	// which is what a zero-thickness axis got before and does nothing to
	// geometry that has no extent on it anyway.
	MeshEntry flat = UnitMesh();
	flat.Extent = Vector3(0.5f, 0.0f, 0.5f);

	DrawInstance instance;
	instance.HalfExtent = Vector3(2.0f, 1.5f, 2.0f);
	const GpuInstance packed = ToGpu(instance, flat);

	CHECK(packed.Scale.x == Approx(4.0f));
	CHECK(packed.Scale.y == Approx(3.0f));
	CHECK(std::isfinite(packed.Scale.y));
}

TEST_CASE("a colour survives packing to within one code", "[render][instancepacking]") {
	// **Eight bits a channel is not a compromise, and this is the case that says
	// so.** The value reaches an eight-bit-per-channel swapchain through the
	// blend either way, so the sixteen bytes of float the old row carried were
	// paying to preserve precision the framebuffer discards. One code is
	// 1/255 - the same step the target itself has.
	constexpr float CODE = 1.0f / 255.0f;

	for (const glm::vec4 colour :
		 {glm::vec4{0.0f, 0.0f, 0.0f, 0.0f},
		  glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},
		  glm::vec4{0.25f, 0.5f, 0.75f, 0.125f},
		  glm::vec4{0.9f, 0.1f, 0.4f, 0.6f}}) {
		const glm::vec4 back = UnpackColour(PackColour(colour));
		CHECK(back.r == Approx(colour.r).margin(CODE));
		CHECK(back.g == Approx(colour.g).margin(CODE));
		CHECK(back.b == Approx(colour.b).margin(CODE));
		CHECK(back.a == Approx(colour.a).margin(CODE));
	}

	// Red in the low byte, alpha in the high one - the order `unpackUnorm4x8`
	// reads and the one `instance.glsl` depends on. A channel swap here would
	// tint the whole world and pass every margin check above.
	CHECK(PackColour(glm::vec4{1.0f, 0.0f, 0.0f, 0.0f}) == 0x000000FFu);
	CHECK(PackColour(glm::vec4{0.0f, 0.0f, 0.0f, 1.0f}) == 0xFF000000u);

	// Out of range clamps. A tint above one is a `Color3` an author overdrove,
	// and it should saturate rather than wrap to black.
	CHECK(UnpackColour(PackColour(glm::vec4{2.0f, -1.0f, 0.5f, 1.0f})).r == Approx(1.0f));
	CHECK(UnpackColour(PackColour(glm::vec4{2.0f, -1.0f, 0.5f, 1.0f})).g == Approx(0.0f));
}

TEST_CASE("transparency reaches the row as alpha", "[render][instancepacking]") {
	// Author-facing `Transparency` is the complement of shader alpha, and the
	// conversion is the one thing `ToGpu` does to the tint. An opaque part must
	// pack to a full alpha exactly, because that is every part in most scenes and
	// a value one code short would put the whole world on the blended path.
	DrawInstance opaque;
	opaque.Tint = Color3(1.0f, 1.0f, 1.0f);
	opaque.Transparency = 0.0f;
	CHECK(UnpackColour(ToGpu(opaque, UnitMesh()).Colour).a == Approx(1.0f));

	DrawInstance ghost;
	ghost.Transparency = 0.75f;
	CHECK(UnpackColour(ToGpu(ghost, UnitMesh()).Colour).a == Approx(0.25f).margin(1.0 / 255.0));
}

TEST_CASE("the row is thirty-six bytes with nothing hidden in it", "[render][instancepacking]") {
	// **The number three other places depend on and only one of them is checked
	// by a compiler.** The vertex buffer description takes `sizeof(GpuInstance)`,
	// the late-instance buffer takes it as an element stride, and
	// `occlusion-cull.comp` spells it as `INSTANCE_WORDS = 9u` - a GLSL constant
	// no C++ build can see. A row that grew would keep compiling and would copy
	// eight ninths of each survivor into the late buffer, which draws as
	// geometry smeared across the scene.
	static_assert(sizeof(GpuInstance) == 36);
	static_assert(sizeof(GpuInstance) % sizeof(uint32_t) == 0);
	CHECK(sizeof(GpuInstance) / sizeof(uint32_t) == 9);

	// Every field four-aligned and packed end to end, so the offsets the vertex
	// attribute table states are the offsets the fields have.
	CHECK(offsetof(GpuInstance, Position) == 0);
	CHECK(offsetof(GpuInstance, Rotation) == 12);
	CHECK(offsetof(GpuInstance, Scale) == 20);
	CHECK(offsetof(GpuInstance, Colour) == 32);

	// **Two and two thirds times smaller than what it replaced.** The old row was
	// a `mat4`, an RGBA float4 and an inverse-scale float4. At a hundred thousand
	// instances that difference is six megabytes a frame, which at sixty frames a
	// second is 360 MB/s of bus this no longer spends.
	constexpr size_t PREVIOUS_ROW_BYTES = 96;
	CHECK(PREVIOUS_ROW_BYTES / sizeof(GpuInstance) >= 2);
}

TEST_CASE("the rotation words are laid out the way the shader reads them", "[render][instancepacking]") {
	// **The one contract `instance.glsl` cannot be compiled against.** The shader
	// decodes with two `unpackSnorm2x16` calls, and that intrinsic returns the
	// *low* half in `.x` and the high half in `.y`. So word zero must hold x then
	// y and word one must hold z then w, in that order - and if this file and
	// that file ever disagreed about it, every rotated part in the world would be
	// rotated wrongly with nothing here failing.
	//
	// Built by hand rather than round-tripped, so the case states the layout
	// instead of agreeing with whatever `PackRotation` happens to do.
	engine::render::PackedRotation packed;
	packed.Words[0] = 0x40002000u; // low 0x2000 -> x, high 0x4000 -> y
	packed.Words[1] = 0x7FFF6000u; // low 0x6000 -> z, high 0x7FFF -> w

	// `UnpackRotation` normalises, which scales all four together and leaves
	// their ratios alone - so the ratios are what pin the layout.
	const glm::quat unpacked = UnpackRotation(packed);
	CHECK(unpacked.x / unpacked.w == Approx(0x2000 / 32767.0f).margin(1e-4));
	CHECK(unpacked.y / unpacked.w == Approx(0x4000 / 32767.0f).margin(1e-4));
	CHECK(unpacked.z / unpacked.w == Approx(0x6000 / 32767.0f).margin(1e-4));

	// And the identity's words are the ones the default member initialiser
	// states, so a row that was allocated and never written draws unrotated
	// rather than drawing whatever zero decodes to - which for a quaternion is
	// not a rotation at all.
	const engine::render::PackedRotation fresh;
	const engine::render::PackedRotation identity = PackRotation(glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
	CHECK(fresh.Words[0] == identity.Words[0]);
	CHECK(fresh.Words[1] == identity.Words[1]);
	CHECK(identity.Words[0] == 0u);
	CHECK(identity.Words[1] == 0x7FFF0000u);
}

TEST_CASE("a part far from the origin keeps its position exactly", "[render][instancepacking]") {
	// **The reason position stayed a full float while rotation and colour were
	// quantised.** Rotation lives in [-1, 1] and colour in [0, 1], where a
	// fixed-point code is the right encoding; a world coordinate has no such
	// bound. A half float at two thousand metres has an interval of two metres,
	// and even a 16-bit fixed point over a generous world size would put a part
	// on a visible lattice.
	//
	// With no mesh centre to fold in, the field must be the authored value bit
	// for bit - not close to it.
	const Vector3 far(20000.0f, -15000.0f, 8192.5f);
	const DrawInstance instance = PartAt(RotationAt(0.9f, 0.3f, -1.2f), far, 1.0f);
	const GpuInstance packed = ToGpu(instance, UnitMesh());

	CHECK(packed.Position.x == far.X);
	CHECK(packed.Position.y == far.Y);
	CHECK(packed.Position.z == far.Z);
}

TEST_CASE("a mirrored part survives the row", "[render][instancepacking]") {
	// A negative half-extent is a part flipped on an axis. The scale field is a
	// plain float precisely so this needs no special case: it carries the sign,
	// and `ModelMatrixOf` and the shader both multiply by it.
	//
	// Worth a case because the obvious saving - storing scale as an unsigned
	// fixed point over some ceiling, the way `ParticleInstance::Size` does -
	// would have silently unmirrored every one of these.
	DrawInstance instance;
	instance.HalfExtent = Vector3(-2.0f, 3.0f, 1.0f);
	const GpuInstance packed = ToGpu(instance, UnitMesh());

	CHECK(packed.Scale.x == Approx(-4.0f));
	CHECK(packed.Scale.y == Approx(6.0f));

	// The corner at the mesh's +X lands at the part's -X, which is what mirrored
	// means.
	const glm::vec3 corner = glm::vec3(ModelMatrixOf(packed) * glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
	CHECK(corner.x == Approx(-2.0f));
	CHECK(corner.y == Approx(3.0f));
}

TEST_CASE("a tiny part is not rounded away", "[render][instancepacking]") {
	// Scale is unbounded in the same direction position is, so a millimetre-sized
	// detail part has to survive as well as a baseplate does. This is the case a
	// half float would have passed and a fixed point over a fixed ceiling would
	// have failed.
	DrawInstance instance;
	instance.HalfExtent = Vector3(0.0005f, 0.0005f, 0.0005f);
	const GpuInstance packed = ToGpu(instance, UnitMesh());

	CHECK(packed.Scale.x == Approx(0.001f));
	CHECK(packed.Scale.x > 0.0f);
}
