#include "ImageGraphTransform3DResident.hpp"
#include "RenderFixture.hpp"

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/render/ComposerSurface.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

TEST_SUITE_ID("engine.render.composersurface_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.cookedcomposer")
TEST_DEPENDS("engine.render.composersurface")

namespace {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::hlsl;
	namespace ig = engine::imagegraph;
	using Access = test_support::TransformImage3DResidentTestAccess;
	using Phase = test_support::TransformImage3DQueuePhase;

	void Accepted(std::optional<std::string> failure) {
		INFO(failure.value_or("accepted"));
		REQUIRE_FALSE(failure);
	}

	// Only the fixture polls: production completion stays nonblocking, and a
	// stalled backend fails within a wall-clock bound instead of waiting forever.
	template <class Predicate> void Drive(Renderer &renderer, Predicate complete) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		size_t submissions = 0;
		while (std::chrono::steady_clock::now() < deadline) {
			Access::Poll(renderer);
			if (complete()) return;
			const auto slots = Access::Slots(renderer);
			if (std::any_of(slots.begin(), slots.end(), [](const auto &slot) {
					return slot.Phase == Phase::Queued;
				})) {
				REQUIRE(++submissions <= 16);
				REQUIRE(Access::RecordAndSubmit(renderer));
			}
			SDL_Delay(1);
		}
		FAIL("Composer real-device producer did not complete within fixture deadline");
	}
	void Ready(Renderer &renderer, core::Name owner, core::Name name, uint64_t generation) {
		Drive(renderer, [&] {
			return renderer.SourceOutputStatus(owner, name, generation) == SourceTextureStatus::Ready;
		});
	}
	void Retired(Renderer &renderer, core::Name owner, core::Name name) {
		Drive(renderer, [&] {
			const auto slots = Access::Slots(renderer);
			return std::none_of(slots.begin(), slots.end(), [&](const auto &slot) {
				return slot.Owner == owner && slot.Name == name && slot.Phase != Phase::Free;
			});
		});
	}

	// A raw transfer verifies the actual resident texture bytes, including the
	// display interpretation. Sampling an sRGB output would decode those bytes.
	std::vector<uint8_t>
	Bytes(Renderer &renderer, core::Name owner, core::Name name, uint32_t width, uint32_t height) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		auto *texture = static_cast<SDL_GPUTexture *>(Access::PublishedTexture(renderer, owner, name));
		REQUIRE(device);
		REQUIRE(texture);
		REQUIRE(width <= 16);
		REQUIRE(height <= 16);
		const uint32_t pitch = 256;
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		info.size = pitch * height;
		const auto releaseTransfer = [device](SDL_GPUTransferBuffer *value) {
			gpu::ReleaseTransferBuffer(device, value);
		};
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(releaseTransfer)> transfer(
			gpu::CreateTransferBuffer(device, &info), releaseTransfer
		);
		REQUIRE(transfer);
		const auto cancelCommand = [](SDL_GPUCommandBuffer *value) { SDL_CancelGPUCommandBuffer(value); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancelCommand)> command(
			SDL_AcquireGPUCommandBuffer(device), cancelCommand
		);
		REQUIRE(command);
		auto *copy = SDL_BeginGPUCopyPass(command.get());
		REQUIRE(copy);
		SDL_GPUTextureRegion from{};
		from.texture = texture;
		from.w = width;
		from.h = height;
		from.d = 1;
		SDL_GPUTextureTransferInfo to{};
		to.transfer_buffer = transfer.get();
		to.pixels_per_row = pitch / 4;
		to.rows_per_layer = height;
		SDL_DownloadFromGPUTexture(copy, &from, &to);
		SDL_EndGPUCopyPass(copy);
		const auto releaseFence = [device](SDL_GPUFence *value) { SDL_ReleaseGPUFence(device, value); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(
			SDL_SubmitGPUCommandBufferAndAcquireFence(command.release()), releaseFence
		);
		REQUIRE(fence);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		const auto *mapped =
			static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(device, transfer.get(), false));
		REQUIRE(mapped);
		std::vector<uint8_t> bytes(size_t(width) * height * 4);
		for (uint32_t row = 0; row < height; ++row)
			std::memcpy(bytes.data() + size_t(row) * width * 4, mapped + row * pitch, width * 4);
		SDL_UnmapGPUTransferBuffer(device, transfer.get());
		return bytes;
	}

	struct FixtureProgram {
		ig::Node Node{"gpu-shader", "pc.hlsl", "", {}, {}};
		std::vector<ig::AuthoredValue> Controls;
		ig::Image Base{
			3,
			2,
			{19, 63, 129, 251, 37,	97,	 193, 127, 61,	149, 239, 31,
			 83, 17, 223, 53,  109, 173, 41,  211, 137, 229, 73,  101},
			0
		};
		ig::Image Other = Base;
		std::vector<ig::HostResolvedImage> Images{{"base_texture", &Base}};
		ig::EvaluationRequest Clock{.Tick = 23, .Seed = 17};
		std::vector<Argument> Arguments;
		core::Name Owner{"composer.gpu.owner"}, Shader{"composer.gpu.cooked"};
		FixtureProgram(std::string main, std::vector<Argument> arguments = {})
			: Arguments(std::move(arguments)) {
			Node.Values = {
				{"vertex", std::string{}},
				{"main", std::move(main)},
				{"global", std::string{}},
				{"libraries", std::string{}}
			};
			for (size_t index = 0; index < Arguments.size(); ++index) {
				const auto suffix = std::to_string(index);
				Node.DynamicInputs.push_back(
					{"argument_name_" + suffix, ig::ValueType::Text, Arguments[index].Name}
				);
				Node.DynamicInputs.push_back(
					{"argument_type_" + suffix,
					 ig::ValueType::Enum,
					 ig::EnumValue{int64_t(Arguments[index].Kind)}}
				);
				const auto type = Arguments[index].Kind == ArgumentKind::Sampler2D ? ig::ValueType::Image
																				   : ig::ValueType::Array;
				Node.DynamicInputs.push_back({"argument_value_" + suffix, type, double(-4)});
			}
			Node.SourceProperties.push_back({std::string(COOKED_SELECTOR), std::string(Shader.Text())});
			RefreshControls();
		}
		void RefreshControls() {
			Controls = Node.Values;
			for (const auto &input : Node.DynamicInputs)
				Controls.push_back({input.Id, *input.Default});
		}
		ig::HostNodeInvocation Invocation() const {
			return {Node, Clock, Controls, Images, MAXIMUM_SURFACE_JOB_BYTES};
		}
		void Install(Renderer &renderer) {
			const auto identity = CookerIdentity();
			REQUIRE_FALSE(identity.empty());
			const auto &main = std::get<std::string>(Node.Values[1].Data);
			assets::ShaderData artifact;
			Accepted(CookArtifact({"", main, "", "", Arguments}, {}, identity, false, artifact));
			Accepted(renderer.InstallComposerShader(Owner, Shader, artifact));
		}
		SurfaceRequest Request(Renderer &renderer, bool display = false) const {
			SurfaceRequest request;
			Accepted(renderer.BuildComposerSurface(Invocation(), Owner, display, request));
			return request;
		}
	};
	std::vector<uint8_t> Swizzled(std::vector<uint8_t> bytes) {
		for (size_t i = 0; i < bytes.size(); i += 4)
			std::swap(bytes[i], bytes[i + 2]);
		return bytes;
	}
}

TEST_CASE(
	"cooked Composer source slots draw exact swizzled bytes into linear and display sinks",
	"[render][gpu][composer-gpu][.]"
) {
	test::FixtureDevice device;
	device.Initialise();
	FixtureProgram program(
		"output.color=otherObject.Sample(other,input.uv).bgra;",
		{{"unused", ArgumentKind::Sampler2D}, {"other", ArgumentKind::Sampler2D}}
	);
	program.Other.Pixels = {247, 37, 85,  113, 29,	151, 209, 67,  71,	233, 43,  191,
							97,	 13, 179, 227, 131, 59,	 17,  149, 163, 101, 199, 31};
	program.Images.push_back({"argument_value_1", &program.Other});
	program.Install(device.Render);
	const core::Name linear("composer.gpu.linear"), display("composer.gpu.display");
	for (const bool interpreted : {false, true}) {
		auto request = program.Request(device.Render, interpreted);
		REQUIRE(request.Textures.size() == 3);
		std::vector<SamplerBinding> active;
		Accepted(SamplerBindings(request.Pair.SpirV, "spirv", active));
		CHECK(std::any_of(active.begin(), active.end(), [](const auto &binding) {
			return binding.SourceName == "other" && binding.SourceSlot == 2;
		}));
		CHECK(std::none_of(active.begin(), active.end(), [](const auto &binding) {
			return binding.SourceSlot == 1;
		}));
		CHECK(request.Textures[1].Pixels == std::vector<uint8_t>{0, 0, 0, 0});
		const auto name = interpreted ? display : linear;
		REQUIRE(
			device.Render.QueueComposerSurface({program.Owner, name, 1, std::move(request)}) ==
			render::imagegraph::TransformImage3DQueueResult::Queued
		);
		CHECK(device.Render.SourceOutputStatus(program.Owner, name, 1) == SourceTextureStatus::Pending);
	}
	Ready(device.Render, program.Owner, linear, 1);
	Ready(device.Render, program.Owner, display, 1);
	CHECK(Bytes(device.Render, program.Owner, linear, 3, 2) == Swizzled(program.Other.Pixels));
	CHECK(Bytes(device.Render, program.Owner, display, 3, 2) == Swizzled(program.Other.Pixels));
	assets::TextureFormat format{};
	REQUIRE(Access::PublishedFormat(device.Render, program.Owner, linear, format));
	CHECK(format == assets::TextureFormat::RGBA8_LINEAR);
	REQUIRE(Access::PublishedFormat(device.Render, program.Owner, display, format));
	CHECK(format == assets::TextureFormat::RGBA8);
	CHECK(
		device.Render.SourceOutputStatus(core::Name("other.owner"), display, 1) == SourceTextureStatus::Absent
	);
}

TEST_CASE(
	"cooked Composer asymmetric matrix reaches async host receipt and submitted cancellation",
	"[render][gpu][composer-gpu][.]"
) {
	test::FixtureDevice device;
	device.Initialise();
	FixtureProgram program(
		"output.color=float4(mul(matrixArg,float3(1,0,0)),1);", {{"matrixArg", ArgumentKind::Mat3}}
	);
	ig::ArrayValue matrix;
	matrix.ElementType = ig::ValueType::Scalar;
	auto &components = matrix.Elements;
	for (const double value : {0., 1., 0., 0., 0., 1., 1., 0., 0.})
		components.emplace_back(value);
	program.Node.DynamicInputs[2].Default = std::move(matrix);
	program.RefreshControls();
	program.Install(device.Render);
	const core::Name name("composer.gpu.async");
	ig::HostNodeCapture receipt;
	receipt.Failure = "retained";
	std::string failure;
	bool pending = false;
	CHECK_FALSE(device.Render.CaptureComposerSurfaceAsync(
		program.Invocation(), program.Owner, name, receipt, failure, &pending
	));
	REQUIRE(pending);
	CHECK(receipt.Failure == "retained");
	Drive(device.Render, [&] {
		const bool ready = device.Render.CaptureComposerSurfaceAsync(
			program.Invocation(), program.Owner, name, receipt, failure, &pending
		);
		REQUIRE((ready || pending));
		return ready;
	});
	CHECK_FALSE(pending);
	CHECK(receipt.Tick == 23);
	REQUIRE(receipt.Images.size() == 1);
	CHECK(receipt.Images[0].Port == "surface");
	std::vector<uint8_t> expected;
	for (size_t pixel = 0; pixel < 6; ++pixel)
		expected.insert(expected.end(), {0, 255, 0, 255});
	CHECK(receipt.Images[0].Data.Pixels == expected);
	program.Clock.Tick = 24;
	CHECK_FALSE(device.Render.CaptureComposerSurfaceAsync(
		program.Invocation(), program.Owner, name, receipt, failure, &pending
	));
	REQUIRE(pending);
	REQUIRE(Access::RecordAndSubmit(device.Render));
	device.Render.CancelComposerCapture(program.Owner, name);
	Retired(device.Render, program.Owner, name);
	CHECK(receipt.Tick == 23);
	CHECK(receipt.Images[0].Data.Pixels == expected);
}

TEST_CASE(
	"cooked Composer submitted cancellation and shader replacement preserve last good output",
	"[render][gpu][composer-gpu][.]"
) {
	test::FixtureDevice device;
	device.Initialise();
	FixtureProgram program("output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv);");
	program.Install(device.Render);
	const core::Name name("composer.gpu.revision");
	const auto old = program.Request(device.Render);
	REQUIRE(
		device.Render.QueueComposerSurface({program.Owner, name, 1, old}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	Ready(device.Render, program.Owner, name, 1);
	CHECK(Bytes(device.Render, program.Owner, name, 3, 2) == program.Base.Pixels);
	REQUIRE(
		device.Render.QueueComposerSurface({program.Owner, name, 2, old}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	REQUIRE(Access::RecordAndSubmit(device.Render));
	REQUIRE(device.Render.CancelTransformImage3D(program.Owner, name, 2));
	Retired(device.Render, program.Owner, name);
	CHECK(device.Render.SourceOutputStatus(program.Owner, name, 2) == SourceTextureStatus::Absent);
	CHECK(device.Render.SourceOutputStatus(program.Owner, name, 1) == SourceTextureStatus::Ready);
	CHECK(Bytes(device.Render, program.Owner, name, 3, 2) == program.Base.Pixels);
	REQUIRE(
		device.Render.QueueComposerSurface({program.Owner, name, 3, old}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	REQUIRE(Access::RecordAndSubmit(device.Render));
	program.Node.Values[1].Data =
		std::string("output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv).bgra;");
	program.RefreshControls();
	program.Install(device.Render);
	CHECK(device.Render.ComposerShaderRevision(program.Owner, program.Shader) == 2);
	CHECK(
		device.Render.QueueComposerSurface({program.Owner, name, 4, old}) ==
		render::imagegraph::TransformImage3DQueueResult::Invalid
	);
	Retired(device.Render, program.Owner, name);
	CHECK(device.Render.SourceOutputStatus(program.Owner, name, 3) == SourceTextureStatus::Absent);
	CHECK(device.Render.SourceOutputStatus(program.Owner, name, 1) == SourceTextureStatus::Ready);
	CHECK(Bytes(device.Render, program.Owner, name, 3, 2) == program.Base.Pixels);
	REQUIRE(
		device.Render.QueueComposerSurface({program.Owner, name, 4, program.Request(device.Render)}) ==
		render::imagegraph::TransformImage3DQueueResult::Queued
	);
	Ready(device.Render, program.Owner, name, 4);
	CHECK(Bytes(device.Render, program.Owner, name, 3, 2) == Swizzled(program.Base.Pixels));
	CHECK(device.Render.SourceOutputStatus(program.Owner, name, 1) == SourceTextureStatus::Absent);
	device.Render.ForgetWorld(0, program.Owner);
	CHECK(device.Render.ComposerShaderRevision(program.Owner, program.Shader) == 0);
	CHECK(device.Render.SourceOutputStatus(program.Owner, name, 4) == SourceTextureStatus::Absent);
	CHECK(Access::PublishedTexture(device.Render, program.Owner, name) == nullptr);
}
