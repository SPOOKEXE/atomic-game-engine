// Resident instance precision and byte layout, checked against independent double rotations.

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
#include <limits>

TEST_SUITE_ID("engine.render.instancepacking")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Vector3;
using engine::render::GpuInstance;
using engine::render::MeshEntry;
using engine::render::ModelMatrixOf;
using engine::render::PackAppearance;
using engine::render::PackColour;
using engine::render::PackRotation;
using engine::render::ToGpu;
using engine::render::UnpackAlphaCutoff;
using engine::render::UnpackAlphaMode;
using engine::render::UnpackColour;
using engine::render::UnpackResampleMode;
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
	for (const float invalid :
		 {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
		const auto packed = PackRotation(glm::quat{1, invalid, 0, 0});
		CHECK(packed.Words[0] == 0);
		CHECK(packed.Words[1] == 0u);
		CHECK(packed.Words[2] == 0u);
		CHECK(packed.Words[3] == 0x3F800000u);
	}
	const float huge = std::numeric_limits<float>::max();
	const auto large = UnpackRotation(PackRotation(glm::quat{huge, huge, huge, huge}));
	CHECK(large.w == Approx(.5).margin(2e-6));
	CHECK(large.x == Approx(.5).margin(2e-6));
}

TEST_CASE("a packed part draws within a tenth of a millimetre per metre", "[render][instancepacking]") {
	// This path includes float world translation, whose precision can dominate
	// small parts far from the origin. The double oracle below isolates rotation.
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

TEST_CASE("the mesh centre correction uses the uploaded rotation", "[render][instancepacking]") {
	// **The subtle half of `ToGpu`, and the one a careless port breaks.** A mesh
	// authored off its own origin is pulled back onto the part's origin by
	// subtracting the rotated, scaled centre - and the rotation that offset is
	// taken through has to be the *unpacked* one, because the unpacked one is
	// what the shader will rotate the geometry by. Cancelling with the exact
	// rotation instead leaves the mesh off by the rounding error times the
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

TEST_CASE("alpha mode and cutoff share one pinned resident word", "[render][instancepacking]") {
	const uint32_t clipped = PackAppearance(engine::scene::AlphaMode::Transparency, 0.5f);
	CHECK((clipped & 0xFFu) == 1u);
	CHECK(((clipped >> 8u) & 0xFFu) == 128u);
	CHECK(UnpackAlphaMode(clipped) == engine::scene::AlphaMode::Transparency);
	CHECK(UnpackAlphaCutoff(clipped) == Approx(0.5f).margin(1.0f / 255.0f));

	CHECK(UnpackAlphaCutoff(PackAppearance(engine::scene::AlphaMode::Opaque, -2.0f)) == 0.0f);
	CHECK(UnpackAlphaCutoff(PackAppearance(engine::scene::AlphaMode::TintMask, 2.0f)) == 1.0f);
	CHECK(UnpackAlphaMode(0xFFu) == engine::scene::AlphaMode::Opaque);

	DrawInstance instance;
	instance.Alpha = engine::scene::AlphaMode::Transparency;
	instance.AlphaCutoff = 0.25f;
	instance.Resample = engine::scene::SurfaceResampleMode::Pixelated;
	instance.SurfaceColour = {0.2f, 0.4f, 0.8f};
	instance.EmissiveTint = {1.0f, 0.25f, 0.1f};
	instance.EmissiveStrength = 4.0f;
	const GpuInstance row = ToGpu(instance, UnitMesh());
	CHECK(UnpackAlphaMode(row.Appearance) == instance.Alpha);
	CHECK(UnpackAlphaCutoff(row.Appearance) == Approx(instance.AlphaCutoff).margin(1.0f / 255.0f));
	CHECK(UnpackResampleMode(row.Appearance) == instance.Resample);
	const glm::vec4 surface = UnpackColour(row.SurfaceColour);
	CHECK(surface.r == Approx(instance.SurfaceColour.R).margin(1.0f / 255.0f));
	CHECK(surface.g == Approx(instance.SurfaceColour.G).margin(1.0f / 255.0f));
	CHECK(surface.b == Approx(instance.SurfaceColour.B).margin(1.0f / 255.0f));
	const glm::vec4 emission = UnpackColour(row.Emission);
	CHECK(emission.r == Approx(instance.EmissiveTint.R).margin(1.0f / 255.0f));
	CHECK(emission.g == Approx(instance.EmissiveTint.G).margin(1.0f / 255.0f));
	CHECK(emission.b == Approx(instance.EmissiveTint.B).margin(1.0f / 255.0f));
	CHECK(emission.a * 16.0f == Approx(instance.EmissiveStrength).margin(16.0f / 255.0f));
}

TEST_CASE("the instance row matches four shader vectors", "[render][instancepacking]") {
	static_assert(sizeof(GpuInstance) == 64);
	static_assert(alignof(GpuInstance) == 16);
	CHECK(sizeof(GpuInstance) / sizeof(uint32_t) == engine::render::GPU_INSTANCE_WORDS);
	CHECK(offsetof(GpuInstance, Position) == 0);
	CHECK(offsetof(GpuInstance, Colour) == 12);
	CHECK(offsetof(GpuInstance, Rotation) == 16);
	CHECK(offsetof(GpuInstance, Scale) == 32);
	CHECK(offsetof(GpuInstance, Appearance) == 44);
	CHECK(offsetof(GpuInstance, SurfaceColour) == 48);
	CHECK(offsetof(GpuInstance, Emission) == 52);
	CHECK(offsetof(GpuInstance, FeatureEnable) == 56);
	CHECK(offsetof(GpuInstance, FeatureDisable) == 60);
	const GpuInstance fresh;
	CHECK(fresh.FeatureEnable == 0);
	CHECK(fresh.FeatureDisable == 0);
	CHECK(engine::render::GPU_JOINT_WORDS == 3 + sizeof(fresh.Rotation) / sizeof(uint32_t));
}

TEST_CASE("instance feature policy occupies the resident GPU row", "[render][instancepacking]") {
	DrawInstance instance;
	instance.RenderFeatures.Enable = engine::scene::FeatureBit(engine::scene::RenderFeature::Emission) |
									 engine::scene::FeatureBit(engine::scene::RenderFeature::PostProcessing) |
									 (1u << 31u);
	instance.RenderFeatures.Disable =
		engine::scene::FeatureBit(engine::scene::RenderFeature::Shadows) | (1u << 30u);

	const GpuInstance row = ToGpu(instance, UnitMesh());
	CHECK(
		row.FeatureEnable == (engine::scene::FeatureBit(engine::scene::RenderFeature::Emission) |
							  engine::scene::FeatureBit(engine::scene::RenderFeature::PostProcessing))
	);
	CHECK(row.FeatureDisable == engine::scene::FeatureBit(engine::scene::RenderFeature::Shadows));
}

TEST_CASE(
	"rotation words retain full float xyzw components for instances and joints", "[render][instancepacking]"
) {
	const engine::render::PackedRotation packed{{0x3F000000u, 0xBF000000u, 0x3F000000u, 0x3F000000u}};
	const auto decoded = UnpackRotation(packed);
	CHECK(decoded.x == .5f);
	CHECK(decoded.y == -.5f);
	CHECK(decoded.z == .5f);
	CHECK(decoded.w == .5f);
	const auto encoded = PackRotation(glm::quat{.5f, .5f, -.5f, .5f});
	for (size_t index = 0; index < 4; ++index)
		CHECK(encoded.Words[index] == packed.Words[index]);
	const engine::render::PackedRotation fresh;
	const auto identity = PackRotation(glm::quat{1, 0, 0, 0});
	for (size_t index = 0; index < 4; ++index)
		CHECK(identity.Words[index] == fresh.Words[index]);
	CHECK(identity.Words[0] == 0);
	CHECK(identity.Words[1] == 0);
	CHECK(identity.Words[2] == 0);
	CHECK(identity.Words[3] == 0x3F800000u);
}

TEST_CASE(
	"float rotations keep thirty micrometre error at a hundred metre radius", "[render][instancepacking]"
) {
	double worst = 0;
	const glm::dvec3 point = glm::normalize(glm::dvec3{2, -3, 5}) * 100.0;
	for (int index = 0; index < 4096; ++index) {
		const double phase = index * 0.371;
		const glm::dvec3 axis = glm::normalize(
			glm::dvec3{std::sin(phase * .73), std::cos(phase * .31), std::sin(phase * .57) + .2}
		);
		const double angle = phase * .91;
		const double sine = std::sin(angle * .5);
		const glm::quat input{
			float(std::cos(angle * .5)), float(axis.x * sine), float(axis.y * sine), float(axis.z * sine)
		};
		const auto packed = PackRotation(input);
		const auto decoded = UnpackRotation(packed);
		// Rodrigues in double uses the original axis/angle, never the packer's
		// reconstruction or its model-matrix helper.
		const auto reference = point * std::cos(angle) + glm::cross(axis, point) * std::sin(angle) +
							   axis * glm::dot(axis, point) * (1 - std::cos(angle));
		const auto actual = glm::dquat(decoded) * point;
		worst = std::max(worst, glm::length(actual - reference));
		CHECK(std::abs(glm::length(actual) - 100.0) < 0.0001);
		const auto negated = UnpackRotation(PackRotation(-input));
		CHECK(glm::length(glm::dquat(negated) * point - actual) < 1e-10);
	}
	INFO("maximum drift at 100 metre radius: " << worst);
	CHECK(worst < 0.00003);
	static_assert(sizeof(engine::render::PackedRotation) == 16);
}

TEST_CASE("a part far from the origin keeps its position exactly", "[render][instancepacking]") {
	// Translation stays a full float, independent of orientation and packed colour.
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
