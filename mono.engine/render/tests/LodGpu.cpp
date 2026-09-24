// Real-device proof that authored mesh LOD choice stays on the GPU.

#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.lodgpu")
TEST_DEPENDS("engine.graph.pipelinedocument")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	assets::MeshData Quad() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	assets::MeshData Triangle() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2};
		mesh.ComputeBounds();
		return mesh;
	}

	assets::MeshData DiagonalBand(float width) {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{-0.5f + width, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{0.5f - width, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	struct SpirvSelectionLayout {
		std::array<uint32_t, 6> Offsets{};
		uint32_t Stride = 0;
		bool Found = false;
	};

	// The test follows descriptor set zero, binding zero from staged compiler output.
	SpirvSelectionLayout ReflectStagedSelectionLayout(const std::filesystem::path &path) {
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input) return {};
		const std::streamsize size = input.tellg();
		if (size < 20 || size % 4 != 0) return {};
		input.seekg(0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		if (!input.read(reinterpret_cast<char *>(bytes.data()), size)) return {};
		std::vector<uint32_t> words(bytes.size() / 4);
		for (size_t index = 0; index < words.size(); ++index) {
			words[index] = static_cast<uint32_t>(bytes[index * 4]) |
						   static_cast<uint32_t>(bytes[index * 4 + 1]) << 8 |
						   static_cast<uint32_t>(bytes[index * 4 + 2]) << 16 |
						   static_cast<uint32_t>(bytes[index * 4 + 3]) << 24;
		}
		if (words[0] != 0x07230203u) return {};

		std::unordered_map<uint32_t, uint32_t> bindings, sets, pointerPointees, arrayElements, arrayStrides;
		std::unordered_map<uint32_t, std::vector<uint32_t>> structMembers;
		std::unordered_map<uint64_t, uint32_t> memberOffsets;
		std::vector<std::pair<uint32_t, uint32_t>> variables;
		for (size_t at = 5; at < words.size();) {
			const uint32_t wordCount = words[at] >> 16;
			const uint32_t opcode = words[at] & 0xFFFFu;
			if (wordCount == 0 || at + wordCount > words.size()) return {};
			const uint32_t *const operands = words.data() + at + 1;
			const uint32_t operandCount = wordCount - 1;
			switch (opcode) {
			case 71: // OpDecorate
				if (operandCount >= 3 && operands[1] == 33) bindings[operands[0]] = operands[2];
				if (operandCount >= 3 && operands[1] == 34) sets[operands[0]] = operands[2];
				if (operandCount >= 3 && operands[1] == 6) arrayStrides[operands[0]] = operands[2];
				break;
			case 72: // OpMemberDecorate
				if (operandCount >= 4 && operands[2] == 35) {
					memberOffsets[(static_cast<uint64_t>(operands[0]) << 32) | operands[1]] = operands[3];
				}
				break;
			case 29: // OpTypeRuntimeArray
				if (operandCount >= 2) arrayElements[operands[0]] = operands[1];
				break;
			case 30: // OpTypeStruct
				if (operandCount >= 1) structMembers[operands[0]] = {operands + 1, operands + operandCount};
				break;
			case 32: // OpTypePointer
				if (operandCount >= 3) pointerPointees[operands[0]] = operands[2];
				break;
			case 59: // OpVariable
				if (operandCount >= 2) variables.emplace_back(operands[1], operands[0]);
				break;
			default:
				break;
			}
			at += wordCount;
		}

		for (const auto &[variable, pointer] : variables) {
			const auto binding = bindings.find(variable);
			const auto set = sets.find(variable);
			if (binding == bindings.end() || set == sets.end() || binding->second != 0 || set->second != 0) {
				continue;
			}
			const auto buffer = pointerPointees.find(pointer);
			if (buffer == pointerPointees.end()) continue;
			const auto rows = structMembers.find(buffer->second);
			if (rows == structMembers.end() || rows->second.size() != 1) continue;
			const auto element = arrayElements.find(rows->second[0]);
			if (element == arrayElements.end()) continue;
			const auto selection = structMembers.find(element->second);
			const auto stride = arrayStrides.find(rows->second[0]);
			if (selection == structMembers.end() || selection->second.size() != 6 ||
				stride == arrayStrides.end())
				continue;
			SpirvSelectionLayout result;
			result.Stride = stride->second;
			for (uint32_t member = 0; member < result.Offsets.size(); ++member) {
				const auto offset =
					memberOffsets.find((static_cast<uint64_t>(element->second) << 32) | member);
				if (offset == memberOffsets.end()) return {};
				result.Offsets[member] = offset->second;
			}
			result.Found = true;
			return result;
		}
		return {};
	}

	graph::PipelineDocument KeepAlbedo(render::Renderer &renderer) {
		graph::PipelineDocument document = graph::DefaultPbrDocument();
		const core::Name kind("lod-test-boundary");
		graph::NodeKindSpec boundary;
		boundary.Kind = kind;
		boundary.Scope = graph::NodeScope::Frame;
		boundary.Queue = graph::ExecutionQueue::Cpu;
		boundary.Category = graph::NodeCategory::Output;
		boundary.Inputs.push_back({.Name = core::Name("albedo"), .Kind = graph::ResourceKind::Texture});
		REQUIRE(graph::RegisterNodeKind(std::move(boundary)));
		REQUIRE(renderer.InstallNodeHandler(kind, [](const graph::RunContext &) { return true; }));
		document.Record({
			.Kind = graph::EditKind::AddNode,
			.Name = kind,
			.NodeKind = kind,
			.Scope = graph::NodeScope::Frame,
		});
		document.Record({
			.Kind = graph::EditKind::Reads,
			.Target = core::Name("albedo"),
			.Key = core::Name("albedo"),
		});
		graph::RenderGraph graph;
		core::Name offender;
		REQUIRE(graph::Build(document, graph, offender) == graph::PipelineDocumentStatus::Ok);
		REQUIRE(renderer.SetPipeline(core::Name("lod.gpu"), graph));
		return document;
	}
}

TEST_CASE("staged lod selection shader matches the host row layout", "[render][gpu][lod][.]") {
	const std::filesystem::path shader =
		std::filesystem::path(SDL_GetBasePath()) / "shaders/resources/lod-select.comp.spv";
	const SpirvSelectionLayout layout = ReflectStagedSelectionLayout(shader);
	INFO("staged shader: " << shader.string());
	REQUIRE(layout.Found);
	CHECK(layout.Stride == 96);
	const std::array<uint32_t, 6> expectedOffsets = {0, 16, 32, 48, 64, 80};
	CHECK(layout.Offsets == expectedOffsets);
}

TEST_CASE("gpu projected area selects an authored mesh level", "[render][gpu][lod][.]") {
	using namespace engine;
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();
	KeepAlbedo(fixture.Render);

	const std::array names = {
		core::Name("lod.quad"),
		core::Name("lod.triangle.1"),
		core::Name("lod.triangle.2"),
		core::Name("lod.triangle.3"),
	};
	REQUIRE(fixture.Render.AddMesh(names[0], Quad()));
	for (size_t level = 1; level < names.size(); ++level) {
		REQUIRE(fixture.Render.AddMesh(names[level], Triangle()));
	}

	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = {0.0f, 0.0f, -4.0f};
	instance.HalfExtent = {1.0f, 1.0f, 0.01f};
	instance.Tint = {1.0f, 0.0f, 0.0f};
	instance.Mesh = names[0];
	instance.LodStrategyMode = scene::LodStrategy::Authored;
	instance.LodLevels = 4;
	for (size_t level = 1; level < names.size(); ++level) {
		instance.LodMeshes[level - 1] = names[level];
	}

	render::SceneTarget target{65, 65};
	render::View view;
	view.World = 944;
	view.WorldName = core::Name("lod.gpu.world");
	view.Pipeline = core::Name("lod.gpu");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;

	instance.LodTargetQuadArea = 1.0f;
	const auto detailedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(detailedFrame.Ran(core::Name("select-lod")));
	const CapturedImage detailed = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);

	instance.LodTargetQuadArea = 1'000'000.0f;
	view.Damage.Objects = true;
	const auto coarseFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(coarseFrame.Ran(core::Name("select-lod")));
	const CapturedImage coarse = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);

	const auto redPixels = [](const CapturedImage &image) {
		size_t count = 0;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto *pixel = reinterpret_cast<const uint8_t *>(image.Bytes.data() + offset);
				count += pixel[0] > 200 && pixel[1] < 30 && pixel[2] < 30;
			}
		}
		return count;
	};
	const size_t detailedPixels = redPixels(detailed);
	const size_t coarsePixels = redPixels(coarse);
	INFO("detailed red pixels: " << detailedPixels << ", coarse red pixels: " << coarsePixels);
	CHECK(detailedPixels > coarsePixels + 100);
	CHECK(detailedFrame.Triangles > coarseFrame.Triangles);

	// Distance limits are a coarse-level floor. Keep projected area on level
	// zero, then prove the same mesh reaches its last resident page by distance.
	instance.LodTargetQuadArea = 1.0f;
	view.LodMinimumDistances = {30.0f, 60.0f, 120.0f};
	view.Damage.Objects = true;
	const auto nearbyFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(nearbyFrame.Ran(core::Name("select-lod")));
	const CapturedImage nearby = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	CHECK(redPixels(nearby) == detailedPixels);
	CHECK(nearbyFrame.Triangles == detailedFrame.Triangles);

	view.LodMinimumDistances = {1.0f, 2.0f, 3.0f};
	view.Damage.Objects = true;
	const auto farFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(farFrame.Ran(core::Name("select-lod")));
	const CapturedImage distant = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	CHECK(redPixels(distant) == coarsePixels);
	CHECK(farFrame.Triangles == coarseFrame.Triangles);

	view.LodMinimumDistances = {4.2f, 4.3f, 4.4f};
	view.VisibilityFrame = core::CFrame{};
	view.CameraFrame = core::CFrame(core::Vector3{2, 0, 0});
	view.Damage.Objects = true;
	const auto lockedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(lockedFrame.Ran(core::Name("select-lod")));
	CHECK(lockedFrame.Triangles == nearbyFrame.Triangles);

	view.VisibilityFrame.reset();
	view.Damage.Objects = true;
	const auto unlockedFrame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(unlockedFrame.Ran(core::Name("select-lod")));
	CHECK(unlockedFrame.Triangles < lockedFrame.Triangles);
}

TEST_CASE(
	"gpu LOD distance inheritance and rotated part selection write distinct pages", "[render][gpu][lod][.]"
) {
	using namespace engine;
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();
	KeepAlbedo(fixture.Render);
	const std::array names = {
		core::Name("lod.lockstep.quad"),
		core::Name("lod.lockstep.triangle"),
		core::Name("lod.lockstep.medium"),
		core::Name("lod.lockstep.narrow"),
	};
	REQUIRE(fixture.Render.AddMesh(names[0], Quad()));
	REQUIRE(fixture.Render.AddMesh(names[1], Triangle()));
	REQUIRE(fixture.Render.AddMesh(names[2], DiagonalBand(0.30f)));
	REQUIRE(fixture.Render.AddMesh(names[3], DiagonalBand(0.10f)));

	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = {0.0f, 0.0f, -4.0f};
	instance.HalfExtent = {1.0f, 1.0f, 0.01f};
	instance.Tint = {1.0f, 0.0f, 0.0f};
	instance.Mesh = names[0];
	instance.LodStrategyMode = scene::LodStrategy::Authored;
	instance.LodLevels = 4;
	instance.LodTargetQuadArea = 1.0f;
	for (size_t level = 1; level < names.size(); ++level) {
		instance.LodMeshes[level - 1] = names[level];
	}

	render::SceneTarget target{65, 65};
	render::View view;
	view.World = 946;
	view.WorldName = core::Name("lod.lockstep.world");
	view.Pipeline = core::Name("lod.gpu");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;
	const auto render = [&] {
		view.Damage.Objects = true;
		const auto frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(frame.Ran(core::Name("select-lod")));
		return CaptureResource(
			fixture.Render,
			core::Name("albedo"),
			view.Slot,
			target.Width,
			target.Height,
			ImageFormat::Rgba8Unorm
		);
	};
	const auto redPixels = [](const CapturedImage &image) {
		size_t count = 0;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto *pixel = reinterpret_cast<const uint8_t *>(image.Bytes.data() + offset);
				count += pixel[0] > 200 && pixel[1] < 30 && pixel[2] < 30;
			}
		}
		return count;
	};
	const auto pixelSignature = [](const CapturedImage &image) {
		std::vector<uint32_t> signature;
		signature.reserve(static_cast<size_t>(image.Width) * image.Height);
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto *pixel = reinterpret_cast<const uint8_t *>(image.Bytes.data() + offset);
				signature.push_back(
					static_cast<uint32_t>(pixel[0]) | static_cast<uint32_t>(pixel[1]) << 8 |
					static_cast<uint32_t>(pixel[2]) << 16 | static_cast<uint32_t>(pixel[3]) << 24
				);
			}
		}
		return signature;
	};

	// Zero is inheritance, so the view's last threshold selects the narrow LOD 3 page.
	view.LodMinimumDistances = {1.0f, 2.0f, 3.0f};
	const size_t inherited = redPixels(render());

	// A complete item override selects its LOD 2 page despite the inherited ladder.
	instance.LodMinimumDistances[0] = 1.0f;
	instance.LodMinimumDistances[1] = 2.0f;
	instance.LodMinimumDistances[2] = 5.0f;
	const size_t override = redPixels(render());
	CHECK(override > inherited);

	// An unordered override is rejected as a whole and returns to the inherited LOD 3 page.
	instance.LodMinimumDistances[0] = 3.0f;
	instance.LodMinimumDistances[1] = 2.0f;
	instance.LodMinimumDistances[2] = 1.0f;
	const size_t invalid = redPixels(render());
	CHECK(invalid == inherited);

	// Compare automatic selection with forced pages at the same transform. This
	// reads GPU output only, so a CPU SelectedLevels diagnostic cannot satisfy it.
	instance.LodMinimumDistances[0] = 0.0f;
	instance.LodMinimumDistances[1] = 0.0f;
	instance.LodMinimumDistances[2] = 0.0f;
	view.LodMinimumDistances = {};
	instance.Frame = core::CFrame::Angles(0.0f, 0.0f, 0.7853981633974483f);
	instance.Frame.Position = {0.0f, 0.0f, -4.0f};
	instance.HalfExtent = {3.0f, 0.2f, 0.01f};
	instance.LodTargetQuadArea = 1.0f;
	const CapturedImage rotated = render();
	instance.LodTargetQuadArea = 0.000001f;
	const CapturedImage forcedDetailed = render();
	instance.LodTargetQuadArea = 1.0f;
	instance.LodMinimumDistances[0] = 1.0f;
	instance.LodMinimumDistances[1] = 2.0f;
	instance.LodMinimumDistances[2] = 3.0f;
	const CapturedImage forcedNarrow = render();
	const size_t rotatedPixels = redPixels(rotated);
	INFO(
		"inherited LOD 3 pixels: " << inherited << ", override LOD 2 pixels: " << override
								   << ", rotated LOD 0 pixels: " << rotatedPixels
	);
	CHECK(pixelSignature(rotated) == pixelSignature(forcedDetailed));
	CHECK(pixelSignature(rotated) != pixelSignature(forcedNarrow));
}

TEST_CASE(
	"gpu final billboard lod faces the camera with upright texture coordinates", "[render][gpu][lod][.]"
) {
	using namespace engine;
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();
	KeepAlbedo(fixture.Render);
	const core::Name mesh("lod.billboard.edge");
	const core::Name texture("lod.billboard.texture");
	REQUIRE(fixture.Render.AddMesh(mesh, Quad()));
	assets::TextureData orientation;
	orientation.Width = orientation.Height = 16;
	orientation.Pixels.resize(16 * 16 * 4);
	for (uint32_t y = 0; y < orientation.Height; ++y) {
		for (uint32_t x = 0; x < orientation.Width; ++x) {
			const bool top = y < orientation.Height / 2;
			const bool left = x < orientation.Width / 2;
			const std::array<std::byte, 4> colour =
				top && left ? std::array{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}}
				: top		? std::array{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}}
				: left		? std::array{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}}
							: std::array{std::byte{255}, std::byte{255}, std::byte{0}, std::byte{255}};
			const size_t offset = (y * orientation.Width + x) * 4;
			std::copy(
				colour.begin(), colour.end(), orientation.Pixels.begin() + static_cast<ptrdiff_t>(offset)
			);
		}
	}
	REQUIRE(fixture.Render.AddTexture(texture, orientation));

	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame = core::CFrame::Angles(1.5707963267948966f, 0.0f, 0.0f);
	instance.Frame.Position = {0.0f, 0.0f, -4.0f};
	instance.HalfExtent = {1.0f, 1.0f, 0.1f};
	instance.Mesh = mesh;
	instance.LodStrategyMode = scene::LodStrategy::Authored;
	instance.LodLevels = 2;
	instance.LodBillboard = texture;
	instance.LodTargetQuadArea = 1'000'000.0f;

	render::SceneTarget target{65, 65};
	render::View view;
	view.World = 945;
	view.WorldName = core::Name("lod.billboard.world");
	view.Pipeline = core::Name("lod.gpu");
	view.Target = &target;
	view.Camera.FieldOfViewRadians = 1.5707963267948966f;
	view.Camera.NearPlane = 0.25f;
	view.Camera.FarPlane = 32.0f;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	const CapturedImage image = CaptureResource(
		fixture.Render, core::Name("albedo"), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
	);
	const auto colourPixels = [&](bool top, bool red) {
		size_t count = 0;
		for (uint32_t y = 0; y < image.Height; ++y) {
			if ((y < image.Height / 2) != top) continue;
			for (uint32_t x = 0; x < image.Width / 2; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const auto *pixel = reinterpret_cast<const uint8_t *>(image.Bytes.data() + offset);
				count += red ? pixel[0] > 200 && pixel[1] < 30 && pixel[2] < 30
							 : pixel[2] > 200 && pixel[0] < 30 && pixel[1] < 30;
			}
		}
		return count;
	};
	const size_t topRed = colourPixels(true, true);
	const size_t bottomBlue = colourPixels(false, false);
	INFO("top red pixels: " << topRed << ", bottom blue pixels: " << bottomBlue);
	CHECK(topRed > 20);
	CHECK(bottomBlue > 20);
}
