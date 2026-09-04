#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <studio/ContentResidency.hpp>

namespace {
	engine::assets::ContentHash RootWithFirstByte(uint8_t value) {
		engine::assets::ContentHash root;
		root.Digest[0] = value;
		return root;
	}
}

TEST_SUITE_ID("studio.contentresidency")

TEST_CASE("content residency ignores a verified delivery already resident", "[studio][contentresidency]") {
	studio::ContentResidency residency;
	const engine::core::Name mesh{"content/arch.mesh"};
	const engine::assets::ContentHash root = RootWithFirstByte(0x11);

	CHECK_FALSE(residency.Contains(mesh, engine::assets::AssetKind::Mesh, root));
	residency.Remember(mesh, engine::assets::AssetKind::Mesh, root);
	CHECK(residency.Contains(mesh, engine::assets::AssetKind::Mesh, root));
}

TEST_CASE("content residency accepts a changed verified root", "[studio][contentresidency]") {
	studio::ContentResidency residency;
	const engine::core::Name mesh{"content/arch.mesh"};
	const engine::assets::ContentHash oldRoot = RootWithFirstByte(0x11);
	const engine::assets::ContentHash newRoot = RootWithFirstByte(0x22);

	residency.Remember(mesh, engine::assets::AssetKind::Mesh, oldRoot);
	CHECK_FALSE(residency.Contains(mesh, engine::assets::AssetKind::Mesh, newRoot));
	residency.Remember(mesh, engine::assets::AssetKind::Mesh, newRoot);
	CHECK(residency.Contains(mesh, engine::assets::AssetKind::Mesh, newRoot));
}

TEST_CASE("content residency accepts a changed asset kind", "[studio][contentresidency]") {
	studio::ContentResidency residency;
	const engine::core::Name asset{"content/shared.bin"};
	const engine::assets::ContentHash root = RootWithFirstByte(0x11);

	residency.Remember(asset, engine::assets::AssetKind::Mesh, root);
	CHECK_FALSE(residency.Contains(asset, engine::assets::AssetKind::Texture, root));
	residency.Remember(asset, engine::assets::AssetKind::Texture, root);
	CHECK(residency.Contains(asset, engine::assets::AssetKind::Texture, root));
}
