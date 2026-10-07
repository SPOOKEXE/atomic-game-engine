#include <engine/imagegraph/SourceParticle3DVertex.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_particle_3d_vertex")
using namespace engine::imagegraph;

namespace source_particle3d_vertex_test {
	constexpr std::array<double, 16> IDENTITY{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	MeshInstance3D IdentityInstance() {
		MeshInstance3D instance;
		instance.Fields[8] = instance.Fields[9] = instance.Fields[10] = 1;
		return instance;
	}
	ParticleRecord3D ActiveParticle() {
		ParticleRecord3D particle;
		particle.Active = 1;
		return particle;
	}
	void Near(Vector3 actual, Vector3 expected) {
		CHECK(std::abs(actual.X - expected.X) < 1e-4);
		CHECK(std::abs(actual.Y - expected.Y) < 1e-4);
		CHECK(std::abs(actual.Z - expected.Z) < 1e-4);
	}
}
using namespace source_particle3d_vertex_test;

TEST_CASE(
	"Particle vertex uses object Euler scale position order and separate RGBA", "[imagegraph][particle3d]"
) {
	MeshVertex3D vertex{{1, 2, 3}, {2, 0, 0}, {.25, .75}, {128, 255, 64, 128}};
	auto instance = IdentityInstance();
	instance.Fields[0] = 1;
	instance.Fields[1] = 2;
	instance.Fields[2] = 3;
	instance.Fields[3] = .9f;
	instance.Fields[6] = 90;
	instance.Fields[7] = .8f;
	instance.Fields[8] = 2;
	instance.Fields[9] = 3;
	instance.Fields[10] = 4;
	instance.Fields[11] = .7f;
	auto particle = ActiveParticle();
	particle.Colour = {.5f, .25f, 2.f, .5f};
	const std::array<double, 16> object{2, 0, 0, 5, 0, 3, 0, 7, 0, 0, 4, 9, 0, 0, 0, 1};
	SourceParticle3DVertex prepared;
	REQUIRE(PrepareSourceParticle3DVertex(vertex, instance, particle, object, {0, 1, 0}, prepared));
	REQUIRE(prepared.Active);
	Near(prepared.Position, {-25, 23, 87});
	Near(prepared.Normal, {0, 2, 0});
	CHECK((prepared.UV == Vector2{.25, .75}));
	CHECK(std::abs(prepared.Colour[0] - 128 / 255. * .5) < 1e-7);
	CHECK(std::abs(prepared.Colour[1] - .25) < 1e-7);
	CHECK(std::abs(prepared.Colour[2] - 64 / 255. * 2) < 1e-7);
	CHECK(std::abs(prepared.Colour[3] - 128 / 255. * .5) < 1e-7);
}

TEST_CASE("Particle billboard and up-normal preserve distinct normal paths", "[imagegraph][particle3d]") {
	const MeshVertex3D vertex{{1, 2, 3}, {0, 0, 2}, {}};
	auto instance = IdentityInstance();
	auto particle = ActiveParticle();
	particle.RenderFlags = 1;
	SourceParticle3DVertex prepared;
	REQUIRE(PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {0, 1, 0}, prepared));
	Near(prepared.Position, {-1, 3, 2});
	Near(prepared.Normal, {0, 2, 0});
	instance.Fields[13] = 1;
	REQUIRE(PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {0, 1, 0}, prepared));
	Near(prepared.Position, {1, 2, 3});
	Near(prepared.Normal, {0, 2, 0});
	particle.RenderFlags = 0;
	REQUIRE(PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {0, 1, 0}, prepared));
	Near(prepared.Position, {-1, 3, 2});
	Near(prepared.Normal, {0, 0, 2});
}

TEST_CASE("Particle source look-at uses identity at either Z pole", "[imagegraph][particle3d]") {
	const MeshVertex3D vertex{{1, 2, 3}, {0, 1, 0}, {}};
	const auto instance = IdentityInstance();
	auto particle = ActiveParticle();
	particle.RenderFlags = 1;
	for (double direction : {-1., 1.}) {
		SourceParticle3DVertex prepared;
		REQUIRE(
			PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {0, 0, direction}, prepared)
		);
		Near(prepared.Position, vertex.Position);
		Near(prepared.Normal, vertex.Normal);
	}
}

TEST_CASE(
	"Inactive particles collapse while invalid active preparation preserves output",
	"[imagegraph][particle3d]"
) {
	MeshVertex3D vertex{{1, 2, 3}, {1, 0, 0}, {}};
	auto instance = IdentityInstance();
	ParticleRecord3D particle;
	SourceParticle3DVertex prepared;
	prepared.Active = true;
	prepared.Position = {99, 99, 99};
	REQUIRE(PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {}, prepared));
	CHECK_FALSE(prepared.Active);
	Near(prepared.Position, {});
	particle.Active = 1;
	particle.RenderFlags = 1;
	prepared.Position = {99, 99, 99};
	CHECK_FALSE(PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {}, prepared));
	Near(prepared.Position, {99, 99, 99});
	particle.RenderFlags = 0;
	instance.Fields[10] = std::numeric_limits<float>::infinity();
	CHECK_FALSE(PrepareSourceParticle3DVertex(vertex, instance, particle, IDENTITY, {0, 1, 0}, prepared));
	Near(prepared.Position, {99, 99, 99});
}
