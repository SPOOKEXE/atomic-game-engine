#include "RenderFixture.hpp"

#include <engine/assets/Builtin.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.texturetable")
TEST_DEPENDS("engine.assets.texture")

using namespace engine;

TEST_CASE(
	"texture table capacity is defaulted and enforced per renderer", "[render][texture-budget][gpu][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);

	render::TextureTable defaultTable;
	REQUIRE(defaultTable.Initialise(device));
	CHECK(defaultTable.CapacityBytes() == render::TextureTable::MAXIMUM_BYTES);
	const size_t builtInBytes = defaultTable.Bytes();
	defaultTable.Shutdown();

	render::TextureTable limited;
	REQUIRE(limited.Initialise(device, false, builtInBytes + 4));
	CHECK(limited.CapacityBytes() == builtInBytes + 4);
	assets::TextureData pixel;
	pixel.Width = pixel.Height = 1;
	pixel.Pixels.assign(4, std::byte{0});
	REQUIRE(limited.Add(core::Name("first"), pixel));
	CHECK_FALSE(limited.Add(core::Name("second"), pixel));
	limited.Shutdown();
}

TEST_CASE("texture copies retain only exact owner-scoped base pixels", "[render][texture-copy][gpu][.]") {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	render::TextureTable table;
	REQUIRE(table.Initialise(device, true));
	const core::Name name("copy.atex"), first("copy:first"), second("copy:second");
	assets::TextureData firstImage;
	firstImage.Width = firstImage.Height = 2;
	firstImage.Format = assets::TextureFormat::RGBA8_LINEAR;
	firstImage.Pixels.assign(16, std::byte{17});
	firstImage.Mips = {{std::byte{99}, std::byte{99}, std::byte{99}, std::byte{99}}};
	assets::TextureData sentinel;
	sentinel.Width = sentinel.Height = 1;
	sentinel.Pixels.assign(4, std::byte{7});

	REQUIRE(table.Add(name, firstImage, first));
	CHECK(table.Copy(name, sentinel, 16, second) == render::TextureCopyStatus::Missing);
	CHECK(sentinel.Pixels[0] == std::byte{7});
	CHECK(table.Copy(name, sentinel, 15, first) == render::TextureCopyStatus::OverLimit);
	CHECK(sentinel.Pixels[0] == std::byte{7});
	REQUIRE(table.Copy(name, sentinel, 16, first) == render::TextureCopyStatus::Copied);
	CHECK(sentinel.Format == assets::TextureFormat::RGBA8_LINEAR);
	CHECK(sentinel.Pixels == firstImage.Pixels);
	CHECK(sentinel.Mips.empty());

	assets::TextureData replacement = firstImage;
	replacement.Pixels.assign(16, std::byte{41});
	REQUIRE(table.Add(name, replacement, first));
	REQUIRE(table.Copy(name, sentinel, 16, first) == render::TextureCopyStatus::Copied);
	CHECK(sentinel.Pixels == replacement.Pixels);
	REQUIRE(table.Add(name, firstImage, second));
	REQUIRE(table.Copy(name, sentinel, 16, second) == render::TextureCopyStatus::Copied);
	CHECK(sentinel.Pixels == firstImage.Pixels);
	SDL_GPUTextureCreateInfo adoptedInfo{};
	adoptedInfo.type = SDL_GPU_TEXTURETYPE_2D;
	adoptedInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	adoptedInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	adoptedInfo.width = adoptedInfo.height = adoptedInfo.layer_count_or_depth = adoptedInfo.num_levels = 1;
	const core::Name adoptedName("adopted.atex");
	REQUIRE(table.Adopt(adoptedName, SDL_CreateGPUTexture(device, &adoptedInfo), 1, 1, 4, first));
	CHECK(table.Copy(adoptedName, sentinel, 4, first) == render::TextureCopyStatus::Unsupported);
	CHECK(sentinel.Pixels == firstImage.Pixels);
	table.Shutdown();
	render::TextureTable ordinary;
	REQUIRE(ordinary.Initialise(device));
	REQUIRE(ordinary.Add(name, firstImage, first));
	CHECK(ordinary.Copy(name, sentinel, 16, first) == render::TextureCopyStatus::Unsupported);
	CHECK(sentinel.Pixels == firstImage.Pixels);
	ordinary.Shutdown();
}

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
	"texture colour space and mip selection reach GPU sampling", "[render][texture-colour-space][gpu][.]"
) {
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	render::TextureTable table;
	REQUIRE(table.Initialise(device));

	assets::TextureData srgb;
	srgb.Width = srgb.Height = 1;
	srgb.Pixels = {std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
	assets::TextureData linear = srgb;
	linear.Format = assets::TextureFormat::RGBA8_LINEAR;
	assets::TextureData mip = linear;
	mip.Width = mip.Height = 2;
	mip.Pixels.assign(16, std::byte{0});
	mip.Mips = {{std::byte{200}, std::byte{200}, std::byte{200}, std::byte{255}}};
	const core::Name srgbName("colour"), linearName("data"), mipName("mip-data");
	REQUIRE(table.Add(srgbName, srgb));
	REQUIRE(table.Add(linearName, linear));
	REQUIRE(table.Add(mipName, mip));
	assets::TextureFormat retained = assets::TextureFormat::R8;
	REQUIRE(table.FormatOf(srgbName, retained));
	CHECK(retained == assets::TextureFormat::RGBA8);
	REQUIRE(table.FormatOf(linearName, retained));
	CHECK(retained == assets::TextureFormat::RGBA8_LINEAR);

	SDL_GPUTextureCreateInfo targetInfo{};
	targetInfo.type = SDL_GPU_TEXTURETYPE_2D;
	targetInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	targetInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	targetInfo.width = 3;
	targetInfo.height = targetInfo.layer_count_or_depth = targetInfo.num_levels = 1;
	const auto releaseTexture = [device](SDL_GPUTexture *texture) {
		render::gpu::ReleaseTexture(device, texture);
	};
	std::unique_ptr<SDL_GPUTexture, decltype(releaseTexture)> target(
		render::gpu::CreateTexture(device, &targetInfo), releaseTexture
	);
	REQUIRE(target);
	SDL_GPUTransferBufferCreateInfo downloadInfo{};
	downloadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	downloadInfo.size = 12;
	const auto releaseTransfer = [device](SDL_GPUTransferBuffer *transfer) {
		render::gpu::ReleaseTransferBuffer(device, transfer);
	};
	std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> download(
		render::gpu::CreateTransferBuffer(device, &downloadInfo), releaseTransfer
	);
	REQUIRE(download);

	auto *command = SDL_AcquireGPUCommandBuffer(device);
	REQUIRE(command);
	const std::array sources = {table.Find(srgbName), table.Find(linearName), table.Find(mipName)};
	for (uint32_t index = 0; index < sources.size(); index++) {
		SDL_GPUBlitInfo blit{};
		blit.source.texture = sources[index];
		blit.source.mip_level = index == 2 ? 1 : 0;
		blit.source.w = blit.source.h = 1;
		blit.destination.texture = target.get();
		blit.destination.x = index;
		blit.destination.w = blit.destination.h = 1;
		blit.load_op = index == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		blit.filter = SDL_GPU_FILTER_NEAREST;
		SDL_BlitGPUTexture(command, &blit);
	}
	auto *copy = SDL_BeginGPUCopyPass(command);
	REQUIRE(copy);
	SDL_GPUTextureRegion source{};
	source.texture = target.get();
	source.w = 3;
	source.h = source.d = 1;
	SDL_GPUTextureTransferInfo destination{};
	destination.transfer_buffer = download.get();
	destination.pixels_per_row = 3;
	destination.rows_per_layer = 1;
	SDL_DownloadFromGPUTexture(copy, &source, &destination);
	SDL_EndGPUCopyPass(copy);
	auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
	REQUIRE(fence);
	REQUIRE(SDL_WaitForGPUFences(device, true, &fence, 1));
	SDL_ReleaseGPUFence(device, fence);
	const auto *pixels =
		static_cast<const std::byte *>(SDL_MapGPUTransferBuffer(device, download.get(), false));
	REQUIRE(pixels);
	const uint8_t decoded = std::to_integer<uint8_t>(pixels[0]);
	CHECK(decoded >= 53);
	CHECK(decoded <= 57);
	CHECK(std::to_integer<uint8_t>(pixels[4]) == 128);
	CHECK(std::to_integer<uint8_t>(pixels[8]) == 200);
	SDL_UnmapGPUTransferBuffer(device, download.get());
	table.Shutdown();
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
