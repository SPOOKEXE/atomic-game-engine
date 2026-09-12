#include "RenderFixture.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.texturetable")
TEST_DEPENDS("engine.assets.texture")

using namespace engine;

TEST_CASE("renderer content retirement preserves other owners", "[render][texture-owner][gpu][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	const core::Name asset("scoped.asset"), first("session:first"), second("session:second");
	assets::TextureData image;
	image.Width = image.Height = 2;
	image.Pixels.assign(16, std::byte{71});
	const auto mesh = assets::MakeBuiltin(assets::BuiltinMesh::Cube);
	const core::Name checker(assets::BuiltinName(assets::BuiltinTexture::Checker));
	CHECK(renderer.TextureHandle(checker, first) == renderer.TextureHandle(checker));
	uint32_t builtinWidth = 0, builtinHeight = 0;
	CHECK(renderer.TextureSize(checker, builtinWidth, builtinHeight, first));
	CHECK(builtinWidth > 0);
	CHECK(builtinHeight > 0);
	CHECK(renderer.TextureCell(checker, 0, first).Scale == 1);
	core::Vector3 builtinExtent;
	CHECK(renderer.MeshExtentOf(
		core::Name(assets::BuiltinName(assets::BuiltinMesh::Sphere)), builtinExtent, first
	));
	for (core::Name owner : {core::Name{}, first, second}) {
		renderer.ExpectTexture(asset, owner);
		CHECK(renderer.ExpectingTexture(asset, owner));
		REQUIRE(renderer.AddTexture(asset, image, owner));
		CHECK_FALSE(renderer.ExpectingTexture(asset, owner));
		REQUIRE(renderer.AddMesh(asset, mesh, owner));
	}
	const auto shared = renderer.TextureHandle(asset);
	const auto remaining = renderer.TextureHandle(asset, second);
	CHECK(shared != remaining);
	CHECK(renderer.TextureHandle(asset, first) != remaining);
	uint32_t width = 0, height = 0;
	REQUIRE(renderer.TextureSize(asset, width, height, first));
	CHECK(width == 2);
	CHECK(height == 2);
	CHECK(renderer.TextureCell(asset, 0, first).Scale == 1);
	core::Vector3 extent;
	CHECK(renderer.MeshExtentOf(asset, extent, first));
	const core::Name pending("later.asset");
	renderer.ExpectTexture(pending, first);
	renderer.ExpectTexture(pending, second);
	const uint64_t revision = renderer.ResourceRevision();
	renderer.DropContentOwner(first);
	CHECK(renderer.ResourceRevision() > revision);
	CHECK(renderer.TextureHandle(asset, first) == nullptr);
	CHECK_FALSE(renderer.MeshExtentOf(asset, extent, first));
	CHECK_FALSE(renderer.ExpectingTexture(pending, first));
	CHECK(renderer.ExpectingTexture(pending, second));
	CHECK(renderer.MeshExtentOf(asset, extent, second));
	CHECK(renderer.MeshExtentOf(asset, extent));
	CHECK(renderer.TextureHandle(asset, second) == remaining);
	CHECK(renderer.TextureHandle(asset) == shared);
	const uint64_t retired = renderer.ResourceRevision();
	renderer.DropContentOwner(first);
	renderer.DropContentOwner({});
	CHECK(renderer.ResourceRevision() == retired);
	renderer.StopExpectingTexture(pending, second);
	CHECK_FALSE(renderer.ExpectingTexture(pending, second));
	CHECK(renderer.DropTexture(asset, second));
	CHECK(renderer.TextureHandle(asset) == shared);
}

TEST_CASE("texture arrivals are tracked independently for each content owner", "[render][texture-owner]") {
	render::TextureTable table;
	const core::Name name("skin.atex"), first("publisher:first"), second("publisher:second");
	table.Expect(name, first);
	table.Expect(name, first);
	table.Expect(name, second);
	table.Expect(name);
	CHECK(table.Awaited() == 3);
	table.StopExpecting(name, first);
	CHECK_FALSE(table.Expecting(name, first));
	CHECK(table.Expecting(name, second));
	CHECK(table.Expecting(name));
	CHECK(table.DropOwner({}) == 0);
	CHECK(table.Awaited() == 2);
	CHECK(table.DropOwner(second) == 0);
	CHECK_FALSE(table.Expecting(name, second));
	CHECK(table.Expecting(name));
	CHECK(table.Find(name, first) == nullptr);
	uint32_t width = 17, height = 23;
	CHECK_FALSE(table.SizeOf(name, width, height, first));
	CHECK(width == 17);
	CHECK(height == 23);
	table.Expect({}, first);
	CHECK(table.Awaited() == 1);
	table.Shutdown();
	CHECK(table.Awaited() == 0);
}

TEST_CASE(
	"same-name texture residency and retirement stay within their owner", "[render][texture-owner][gpu][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	render::TextureTable table;
	REQUIRE(table.Initialise(device));
	const size_t initialCount = table.Count();
	const size_t initialBytes = table.Bytes();
	const core::Name name("skin.atex"), first("publisher:first"), second("publisher:second");
	assets::TextureData small;
	small.Width = small.Height = 2;
	small.Pixels.assign(16, std::byte{71});
	assets::TextureData sheet;
	sheet.Width = sheet.Height = 4;
	sheet.Pixels.assign(64, std::byte{142});
	sheet.FlipbookSide = 2;
	sheet.FlipbookFrames = 4;
	sheet.FlipbookFrameRate = 2;
	REQUIRE(table.Add(name, small));
	table.Expect(name, first);
	table.Expect(name, second);
	REQUIRE(table.Add(name, small, first));
	CHECK_FALSE(table.Expecting(name, first));
	CHECK(table.Expecting(name, second));
	REQUIRE(table.Add(name, sheet, second));
	CHECK_FALSE(table.Expecting(name, second));
	CHECK(table.Count() == initialCount + 3);
	CHECK(table.Bytes() == initialBytes + 96);
	const auto shared = table.Find(name);
	const auto source = table.Find(name, first);
	const auto destination = table.Find(name, second);
	REQUIRE(shared != nullptr);
	REQUIRE(source != nullptr);
	REQUIRE(destination != nullptr);
	CHECK(source != destination);
	CHECK(shared != source);
	CHECK(table.Find(name, core::Name("absent")) == nullptr);
	uint32_t width = 0, height = 0;
	REQUIRE(table.SizeOf(name, width, height, first));
	CHECK(width == 2);
	CHECK(height == 2);
	REQUIRE(table.SizeOf(name, width, height, second));
	CHECK(width == 4);
	CHECK(height == 4);
	CHECK(table.CellOf(name, .5, first).Scale == 1);
	CHECK(table.CellOf(name, .5, second).Scale == .5f);
	CHECK(table.AnimationSignature(0) != table.AnimationSignature(.5));
	CHECK_FALSE(table.Add(name, {}, first));
	CHECK(table.Find(name, first) == source);
	REQUIRE(table.Add(name, sheet, first));
	CHECK(table.Find(name, second) == destination);
	CHECK(table.Find(name) == shared);
	CHECK(table.Bytes() == initialBytes + 144);
	CHECK(table.AnimationSignature(0) != 0);
	CHECK(table.AnimationSignature(0) != table.AnimationSignature(.5));
	table.Expect(core::Name("later.atex"), first);
	CHECK(table.DropOwner(first) == 1);
	CHECK(table.Find(name, first) == nullptr);
	CHECK_FALSE(table.Expecting(core::Name("later.atex"), first));
	CHECK(table.Find(name, second) == destination);
	CHECK(table.Bytes() == initialBytes + 80);
	CHECK(table.DropOwner(first) == 0);
	CHECK(table.DropOwner({}) == 0);
	CHECK(table.Find(name) == shared);
	REQUIRE(table.Drop(name, second));
	CHECK(table.Count() == initialCount + 1);
	CHECK(table.Bytes() == initialBytes + 16);
	CHECK(table.AnimationSignature(.5) == 0);
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = info.height = info.layer_count_or_depth = info.num_levels = 1;
	const auto release = [device](SDL_GPUTexture *texture) { render::gpu::ReleaseTexture(device, texture); };
	std::unique_ptr<SDL_GPUTexture, decltype(release)> adopted(
		render::gpu::CreateTexture(device, &info), release
	);
	REQUIRE(adopted);
	CHECK_FALSE(table.Adopt({}, adopted.get(), 1, 1, 4, first));
	REQUIRE(table.Adopt(name, adopted.get(), 1, 1, 4, first));
	const auto adoptedHandle = adopted.release();
	CHECK(table.Find(name, first) == adoptedHandle);
	CHECK(table.Find(name) == shared);
	CHECK(table.Bytes() == initialBytes + 20);
	CHECK(table.DropOwner(first) == 1);
	CHECK(table.Bytes() == initialBytes + 16);
}
