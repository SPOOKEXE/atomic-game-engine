#include "../src/MeshPayload.hpp"
#include "../src/SourceMeshAvailability.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_particle_3d_mesh")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

TEST_CASE(
	"particle mesh discriminator admits empty typed payload and requires exact records",
	"[imagegraph][particle3d]"
) {
	MeshValue3D value;
	auto &mesh = value.Data.emplace();
	mesh.LocalTransforms.emplace_back();
	mesh.Instanced = mesh.ParticleInstanced = true;
	CHECK(ValidMeshPayload(value));
	mesh.Instances.emplace_back();
	CHECK_FALSE(ValidMeshPayload(value));
	mesh.ParticleRecords.emplace_back();
	CHECK(ValidMeshPayload(value));
	mesh.ParticleInstanced = false;
	CHECK_FALSE(ValidMeshPayload(value));
	mesh.ParticleInstanced = true;
	mesh.Instanced = false;
	CHECK_FALSE(ValidMeshPayload(value));
}

TEST_CASE(
	"particle mesh storage and clones retain independent separate records", "[imagegraph][particle3d]"
) {
	MeshValue3D source;
	auto &mesh = source.Data.emplace();
	mesh.LocalTransforms.emplace_back();
	mesh.Instanced = mesh.ParticleInstanced = true;
	mesh.ParticleTransparent = true;
	mesh.ParticleBlend = ParticleBlend3D::Maximum;
	mesh.Instances.emplace_back();
	const auto withoutRecords = MeshStorageBytes<false>(source);
	mesh.ParticleRecords.emplace_back();
	mesh.ParticleRecords[0].Active = 1;
	mesh.ParticleRecords[0].Colour = {.25f, .5f, .75f, .125f};
	mesh.ParticleRecords[0].Velocity[2] = 3;
	CHECK(MeshStorageBytes<false>(source) == withoutRecords + sizeof(ParticleRecord3D));
	mesh.ParticleRecords.reserve(8);
	CHECK(MeshStorageBytes<true>(source) >= MeshStorageBytes<false>(source) + 7 * sizeof(ParticleRecord3D));
	auto clone = CloneSourceObjectMesh(source, true);
	REQUIRE(clone.Data);
	CHECK(clone.Data->ParticleInstanced);
	CHECK(clone.Data->ParticleTransparent);
	CHECK(clone.Data->ParticleBlend == ParticleBlend3D::Maximum);
	CHECK(clone.Data->ParticleRecords == mesh.ParticleRecords);
	clone.Data->ParticleRecords[0].Colour[3] = 1;
	CHECK(mesh.ParticleRecords[0].Colour[3] == .125f);
	mesh.ParticleRecords[0].Velocity[2] = std::numeric_limits<float>::infinity();
	CHECK_FALSE(ValidMeshPayload(source));
}
