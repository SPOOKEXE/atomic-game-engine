#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/packing.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.render.skinfixtures")
TEST_DEPENDS("engine.render.instancepacking")

namespace {
	using namespace engine;
	using namespace engine::render;
	constexpr uint32_t EXTENT = 65;
	const core::Name PIPELINE("skin-oracle"), SKIN("two-joint-mesh");
	const std::array<core::Name, 2> EXPORTS{core::Name("skin-export"), core::Name("native-export")};

	assets::MeshData WeightedMesh() {
		assets::MeshData mesh;
		mesh.JointCount = 2;
		mesh.Vertices = {
			{{-.9f, -.7f, 0}, {0, 0, 1}, {0, 1}},
			{{.8f, -.7f, 0}, {0, 0, 1}, {1, 1}},
			{{.8f, .9f, 0}, {0, 0, 1}, {1, 0}},
			{{-.9f, .9f, 0}, {0, 0, 1}, {0, 0}}
		};
		for (size_t index = 0; index < mesh.Vertices.size(); ++index) {
			auto &vertex = mesh.Vertices[index];
			vertex.Joints[0] = 0;
			vertex.Joints[1] = 1;
			vertex.Weights[0] = index == 0 || index == 3 ? 49151 : 16384;
			vertex.Weights[1] = 65535 - vertex.Weights[0];
		}
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	assets::MeshData Deform(const assets::MeshData &source, const std::array<core::CFrame, 2> &joints) {
		auto mesh = source;
		mesh.JointCount = 0;
		for (auto &vertex : mesh.Vertices) {
			const glm::dvec3 original{vertex.Position[0], vertex.Position[1], vertex.Position[2]};
			const glm::dvec3 normal{vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]};
			glm::dvec3 position{}, direction{};
			for (size_t influence = 0; influence < 2; ++influence) {
				const auto &joint = joints[vertex.Joints[influence]];
				// Independent double matrix skinning, without GPU packing or shader helpers.
				const auto rotation = glm::mat3_cast(glm::normalize(glm::dquat(joint.Rotation())));
				const glm::dvec3 translation{joint.Position.X, joint.Position.Y, joint.Position.Z};
				const double weight = double(vertex.Weights[influence]) / 65535.0;
				position += (rotation * original + translation) * weight;
				direction += rotation * normal * weight;
			}
			for (size_t axis = 0; axis < 3; ++axis) {
				vertex.Position[axis] = float(position[axis]);
				vertex.Normal[axis] = float(direction[axis]);
			}
			std::fill(std::begin(vertex.Joints), std::end(vertex.Joints), 0);
			std::fill(std::begin(vertex.Weights), std::end(vertex.Weights), 0);
		}
		mesh.ComputeBounds();
		return mesh;
	}

	scene::DrawInstance Row(core::Name meshName, const assets::MeshData &mesh) {
		scene::DrawInstance row;
		row.Source = 1;
		row.Mesh = meshName;
		row.Frame.Position = (mesh.Minimum + mesh.Maximum) * .5f + core::Vector3{0, 0, -4};
		row.HalfExtent = (mesh.Maximum - mesh.Minimum) * .5f;
		if (row.HalfExtent.Z == 0) row.HalfExtent.Z = .5f;
		row.Tint = {.7f, .35f, .1f};
		row.CastShadow = false;
		row.SkinCount = mesh.JointCount;
		return row;
	}

	test::CapturedImage Capture(Renderer &renderer, View view) {
		const auto node = EXPORTS.at(view.Slot);
		const auto token = renderer.QueueResourceImage(PIPELINE, node, view.Slot);
		REQUIRE(token != 0);
		OverlayImage overlay;
		REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Ran(node));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (const auto captured = renderer.TakeResourceImage(token)) {
				REQUIRE(captured->Status == ResourceImageStatus::Ok);
				REQUIRE(captured->Width == EXTENT);
				REQUIRE(captured->Height == EXTENT);
				test::CapturedImage image{EXTENT, EXTENT, test::ImageFormat::Rgba32Float, EXTENT * 16, {}};
				core::ByteWriter floats;
				for (size_t row = 0; row < EXTENT; ++row) {
					core::ByteReader reader(
						std::span(captured->Pixels).subspan(row * captured->RowStride, EXTENT * 8)
					);
					while (!reader.AtEnd()) {
						const auto pair = glm::unpackHalf2x16(reader.ReadUInt32());
						floats.WriteFloat(pair.x);
						floats.WriteFloat(pair.y);
					}
				}
				image.Bytes.assign(floats.Bytes().begin(), floats.Bytes().end());
				return image;
			}
			SDL_Delay(1);
		}
		FAIL("skin HDR capture did not complete");
		return {};
	}
}

TEST_CASE(
	"GPU joint palettes match independently deformed meshes across pose updates",
	"[render][gpu][skin-fixture][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	auto document = graph::DefaultPbrDocument();
	for (size_t slot = 0; slot < EXPORTS.size(); ++slot) {
		document.Record(
			{.Kind = graph::EditKind::AddNode,
			 .Name = EXPORTS[slot],
			 .NodeKind = core::Name("capture"),
			 .Scope = graph::NodeScope::Frame}
		);
		document.Record(
			{.Kind = graph::EditKind::Reads, .Target = core::Name("lens-b"), .Key = core::Name("source")}
		);
		document.Record(
			{.Kind = graph::EditKind::Set, .Key = core::Name("view"), .Value = std::to_string(slot)}
		);
	}
	graph::RenderGraph pipeline;
	core::Name offender;
	REQUIRE(graph::Build(document, pipeline, offender) == graph::PipelineDocumentStatus::Ok);
	REQUIRE(renderer.SetPipeline(PIPELINE, pipeline));
	const auto weighted = WeightedMesh();
	REQUIRE(weighted.IsValid());
	REQUIRE(renderer.AddMesh(SKIN, weighted));
	const std::array<std::array<core::CFrame, 2>, 2> poses{
		{{core::CFrame({-.24f, .13f, -.17f}, glm::quat(glm::vec3{.18f, -.22f, .31f})),
		  core::CFrame({.27f, -.16f, .21f}, glm::quat(glm::vec3{-.23f, .34f, -.19f}))},
		 {core::CFrame({.18f, -.23f, .12f}, glm::quat(glm::vec3{-.29f, .17f, -.38f})),
		  core::CFrame({-.21f, .24f, -.19f}, glm::quat(glm::vec3{.26f, -.31f, .27f}))}}
	};
	std::array<assets::MeshData, 2> references;
	std::array<core::Name, 2> names{core::Name("skin-cpu-pose-0"), core::Name("skin-cpu-pose-1")};
	for (size_t pose = 0; pose < poses.size(); ++pose) {
		references[pose] = Deform(weighted, poses[pose]);
		REQUIRE(references[pose].IsValid());
		REQUIRE(renderer.AddMesh(names[pose], references[pose]));
	}
	SceneTarget target{EXTENT, EXTENT};
	View view;
	view.World = 41;
	view.WorldName = core::Name("skin-oracle-world");
	view.Target = &target;
	view.Pipeline = PIPELINE;
	view.Camera.FieldOfViewRadians = 1.1f;
	view.OverrideLighting = true;
	view.Lighting.Ambient = {.15f, .15f, .15f};
	view.Lighting.OutdoorAmbient = view.Lighting.Ambient;
	view.Lighting.Direction = {.3f, -.5f, -.8f};
	view.Lighting.Direct = {.7f, .7f, .7f};
	const auto skinned = Row(SKIN, weighted);
	test::CapturedImage previous;
	for (size_t pose = 0; pose < poses.size(); ++pose) {
		CAPTURE(pose);
		view.Slot = 0;
		view.Instances = std::span(&skinned, 1);
		view.JointFrames = poses[pose];
		const auto actual = Capture(renderer, view);
		const auto native = Row(names[pose], references[pose]);
		view.Slot = 1;
		view.Instances = std::span(&native, 1);
		view.JointFrames = {};
		const auto expected = Capture(renderer, view);
		test::CheckImage(
			renderer,
			"skin-palette",
			std::to_string(pose),
			"two weighted joints; independent double CPU deformation",
			expected.View(),
			actual.View(),
			{.Absolute = .002, .Region = {}}
		);
		if (pose != 0)
			CHECK(
				test::CompareImages(
					previous.View(), actual.View(), {.Absolute = .002, .Region = {}}
				).MismatchedPixels > 100
			);
		previous = actual;
	}
}
