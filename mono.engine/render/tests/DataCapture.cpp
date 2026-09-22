#include "CaptureRecordValidation.hpp"
#include "DataCaptureCompact.hpp"
#include "DataCapturePacking.hpp"
#include "RenderFixture.hpp"
#include "SecondSurfaceDepth.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/DataFactoryHookBind.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ScriptDataCaptureBridge.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.datacapture")

using namespace engine::render;

TEST_CASE(
	"compact depth uses exact binary16 rounding and preserves special classes", "[render][data-capture]"
) {
	using namespace data_capture_compact;
	for (uint32_t raw = 0; raw <= 0xffff; ++raw) {
		const uint16_t half = static_cast<uint16_t>(raw);
		if ((half & 0x7c00) == 0x7c00) continue;
		uint16_t encoded = 0;
		REQUIRE(Float32ToFloat16(Float16ToFloat32(half), encoded));
		CHECK(encoded == half);
	}
	uint16_t encoded = 0;
	const float leastSubnormal = std::ldexp(1.0f, -24);
	REQUIRE(Float32ToFloat16(std::ldexp(1.0f, -25), encoded));
	CHECK(encoded == 0x0000);
	REQUIRE(Float32ToFloat16(std::nextafter(std::ldexp(1.0f, -25), 1.0f), encoded));
	CHECK(encoded == 0x0001);
	REQUIRE(Float32ToFloat16(leastSubnormal * 1.5f, encoded));
	CHECK(encoded == 0x0002);
	REQUIRE(Float32ToFloat16(65504.0f, encoded));
	CHECK(encoded == 0x7bff);
	CHECK_FALSE(Float32ToFloat16(std::nextafter(65504.0f, std::numeric_limits<float>::infinity()), encoded));
	REQUIRE(Float32ToFloat16(std::numeric_limits<float>::infinity(), encoded));
	CHECK(encoded == 0x7c00);
	CHECK(std::isinf(Float16ToFloat32(encoded)));
	REQUIRE(Float32ToFloat16(std::numeric_limits<float>::quiet_NaN(), encoded));
	CHECK(std::isnan(Float16ToFloat32(encoded)));
	CHECK((encoded & 0x7c00) == 0x7c00);
	CHECK((encoded & 0x03ff) != 0);
}

TEST_CASE("compact depth strips padding and reports finite quantization error", "[render][data-capture]") {
	using namespace data_capture_compact;
	constexpr uint32_t width = 2;
	constexpr uint32_t height = 2;
	constexpr uint32_t sourceStride = 12;
	std::vector<std::byte> source(sourceStride * height, std::byte{0xa5});
	const std::array values{1.0003f, 2.0003f, 3.0003f, 4.0003f};
	for (size_t row = 0; row < height; ++row)
		for (size_t column = 0; column < width; ++column) {
			const uint32_t bits = std::bit_cast<uint32_t>(values[row * width + column]);
			for (size_t byte = 0; byte < 4; ++byte)
				source[row * sourceStride + column * 4 + byte] =
					static_cast<std::byte>((bits >> (byte * 8)) & 0xff);
		}
	std::vector<std::byte> compact;
	std::optional<double> error;
	std::string classification;
	std::string rejection;
	REQUIRE(CompactFloat32DepthBytes(
		source, width, height, sourceStride, compact, error, classification, rejection
	));
	CHECK(compact.size() == width * height * 2);
	REQUIRE(error);
	CHECK(*error > 0.0);
	CHECK(classification == "finite");
	std::vector<std::byte> special(8);
	const std::array specialValues{
		std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()
	};
	for (size_t column = 0; column < specialValues.size(); ++column) {
		const uint32_t bits = std::bit_cast<uint32_t>(specialValues[column]);
		for (size_t byte = 0; byte < 4; ++byte)
			special[column * 4 + byte] = static_cast<std::byte>((bits >> (byte * 8)) & 0xff);
	}
	REQUIRE(CompactFloat32DepthBytes(special, 2, 1, 8, compact, error, classification, rejection));
	CHECK_FALSE(error);
	CHECK(classification == "contains_infinity_and_nan");
	const uint32_t overflowBits = std::bit_cast<uint32_t>(65536.0f);
	for (size_t byte = 0; byte < 4; ++byte)
		special[byte] = static_cast<std::byte>((overflowBits >> (byte * 8)) & 0xff);
	compact.assign(3, std::byte{0x7f});
	error = 42.0;
	classification = "stale";
	rejection = "stale";
	CHECK_FALSE(CompactFloat32DepthBytes(
		std::span(special).first(4), 1, 1, 4, compact, error, classification, rejection
	));
	CHECK(compact.empty());
	CHECK_FALSE(error);
	CHECK(classification == "finite_overflow");
	CHECK(rejection == "finite_overflow_above_65504");
}

TEST_CASE("data capture channel names are stable", "[render][data-capture]") {
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::RgbLinearHdr)) == "rgb_linear_hdr");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::LinearDepth)) == "linear_depth");
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::AmbientOcclusion)) == "ambient_occlusion"
	);
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::DirectionalResponse)) ==
		"directional_response"
	);
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::ShadowVisibility)) == "shadow_visibility"
	);
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::SemanticMask)) == "semantic_ids");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::PartMask)) == "part_ids");
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::FirstSurfaceValidity)) ==
		"first_surface_validity"
	);
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::SecondSurfaceDepth)) ==
		"second_surface_depth"
	);
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::SecondSurfaceValidity)) ==
		"second_surface_validity"
	);
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::OpticalFlow)) == "optical_flow");
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::PbrSpecular)) == "pbr_specular");
	CHECK(
		std::string_view(DataCaptureChannelName(DataCaptureChannel::PbrTransmission)) == "pbr_transmission"
	);
	CHECK(std::string_view(DataCaptureChannelName(DataCaptureChannel::MeshUv)) == "mesh_uv");
}

TEST_CASE("packed capture planes retain depth and resample scalar sources", "[render][data-capture]") {
	DataCapturePlane depth;
	depth.Channel = DataCaptureChannel::LinearDepth;
	depth.Status = DataCaptureStatus::Ready;
	depth.Width = 2;
	depth.Height = 2;
	depth.RowStride = 8;
	depth.Scalar = DataCaptureScalar::Float32;
	depth.Bytes.resize(16);
	const std::array depthValues{1.25f, 2.5f, 3.75f, 5.0f};
	for (size_t index = 0; index < depthValues.size(); ++index)
		std::memcpy(depth.Bytes.data() + index * sizeof(float), &depthValues[index], sizeof(float));

	DataCapturePlane occlusion;
	occlusion.Channel = DataCaptureChannel::AmbientOcclusion;
	occlusion.Status = DataCaptureStatus::Ready;
	occlusion.Width = 1;
	occlusion.Height = 1;
	occlusion.RowStride = 1;
	occlusion.Scalar = DataCaptureScalar::UNorm8;
	occlusion.Bytes = {std::byte{128}};

	DataCapturePlane material;
	material.Channel = DataCaptureChannel::PbrMaterial;
	material.Status = DataCaptureStatus::Ready;
	material.Width = 2;
	material.Height = 2;
	material.RowStride = 8;
	material.Scalar = DataCaptureScalar::UNorm8;
	material.Bytes = {
		std::byte{10},
		std::byte{20},
		std::byte{30},
		std::byte{40},
		std::byte{50},
		std::byte{60},
		std::byte{70},
		std::byte{80},
		std::byte{90},
		std::byte{100},
		std::byte{110},
		std::byte{120},
		std::byte{130},
		std::byte{140},
		std::byte{150},
		std::byte{160},
	};
	const std::array<const DataCapturePlane *, 4> planes{&depth, &occlusion, &material, &material};
	std::array components{
		engine::script::DataCaptureBridgePackedComponent{"linear_depth", 0},
		engine::script::DataCaptureBridgePackedComponent{"ambient_occlusion", 0},
		engine::script::DataCaptureBridgePackedComponent{"pbr_material", 0},
		engine::script::DataCaptureBridgePackedComponent{"pbr_material", 2},
	};
	data_capture_packing::Result packed;
	std::string rejection;
	REQUIRE(data_capture_packing::PackRgba32F(planes, components, packed, rejection));
	CHECK(packed.Width == 2);
	CHECK(packed.Height == 2);
	CHECK(packed.Bytes.size() == 64);
	for (size_t index = 0; index < depthValues.size(); ++index) {
		float packedDepth = 0.0f;
		std::memcpy(&packedDepth, packed.Bytes.data() + index * 16, sizeof(packedDepth));
		CHECK(std::bit_cast<uint32_t>(packedDepth) == std::bit_cast<uint32_t>(depthValues[index]));
		float packedAo = 0.0f;
		std::memcpy(&packedAo, packed.Bytes.data() + index * 16 + 4, sizeof(packedAo));
		CHECK(packedAo == 128.0f / 255.0f);
	}
	float roughness = 0.0f;
	float materialOcclusion = 0.0f;
	std::memcpy(&roughness, packed.Bytes.data() + 2 * 16 + 8, sizeof(roughness));
	std::memcpy(&materialOcclusion, packed.Bytes.data() + 2 * 16 + 12, sizeof(materialOcclusion));
	CHECK(roughness == 90.0f / 255.0f);
	CHECK(materialOcclusion == 110.0f / 255.0f);
	components[0].SourceComponent = 1;
	CHECK_FALSE(data_capture_packing::PackRgba32F(planes, components, packed, rejection));
	CHECK(rejection == "unsupported_source_layout");
	DataCapturePlane oversized = depth;
	oversized.Width = 8193;
	oversized.Height = 8193;
	oversized.RowStride = 4;
	const std::array<const DataCapturePlane *, 4> oversizedPlanes{
		&oversized, &oversized, &oversized, &oversized
	};
	components[0].SourceComponent = 0;
	CHECK_FALSE(data_capture_packing::PackRgba32F(oversizedPlanes, components, packed, rejection));
	CHECK(rejection == "packed_plane_size_overflow");
}

TEST_CASE("object label sidecars require bounded UTF-8 labels", "[render][data-capture]") {
	std::vector<DataCaptureObjectLabel> labels{{1, "alpha"}};
	CHECK(ValidDataCaptureObjectLabels(labels));
	labels[0].StableId = std::string{"\xC0\x80", 2};
	CHECK_FALSE(ValidDataCaptureObjectLabels(labels));

	labels.assign(MAX_DATA_CAPTURE_OBJECT_LABELS + 1, {});
	for (size_t index = 0; index < labels.size(); ++index) {
		labels[index].Label = static_cast<uint32_t>(index + 1);
		labels[index].StableId = "a";
	}
	CHECK_FALSE(ValidDataCaptureObjectLabels(labels));

	labels.assign(MAX_DATA_CAPTURE_OBJECT_LABEL_BYTES / 256 + 1, {});
	for (size_t index = 0; index < labels.size(); ++index) {
		labels[index].Label = static_cast<uint32_t>(index + 1);
		labels[index].StableId.assign(256, 'a');
	}
	CHECK_FALSE(ValidDataCaptureObjectLabels(labels));
}

TEST_CASE(
	"frame result keeps headless command submission separate from presentation", "[render][data-capture]"
) {
	FrameResult frame;
	FrameResult headless;
	headless.Submitted = true;
	frame.Accumulate(headless);
	CHECK(frame.Submitted);
	CHECK_FALSE(frame.Presented);
}

TEST_CASE("data capture camera convention records the engine projection", "[render][data-capture]") {
	const DataCaptureCameraConvention convention = DataCaptureCameraConventions();
	CHECK(convention.RightHandedWorld);
	CHECK(convention.CameraLooksNegativeZ);
	CHECK(convention.ClipYUp);
	CHECK(convention.DepthZeroToOne);
	CHECK(convention.ProjectionIsColumnMajor);
	CHECK(convention.MetresPerWorldUnit == 1.0f);
}

TEST_CASE(
	"native-resolution data-factory capture retains every starter plane", "[render][gpu][data-capture][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipeline("native-resolution-data-capture");
	REQUIRE(renderer.SetPipeline(pipeline, graph));

	assets::MeshData plane;
	plane.Vertices = {
		{{-1, -1, 0}, {0, 0, 1}, {0, 1}},
		{{1, -1, 0}, {0, 0, 1}, {1, 1}},
		{{1, 1, 0}, {0, 0, 1}, {1, 0}},
		{{-1, 1, 0}, {0, 0, 1}, {0, 0}}
	};
	plane.Indices = {0, 1, 2, 0, 2, 3};
	plane.ComputeBounds();
	const core::Name mesh("first-surface-validity-plane");
	REQUIRE(renderer.AddMesh(mesh, plane));
	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Mesh = mesh;
	instance.Frame.Position = {0, 0, -3};
	instance.HalfExtent = {1, 1, 1};
	instance.EmissiveTint = {0.25f, 0.5f, 1.0f};
	instance.EmissiveStrength = 4.0f;
	instance.CastShadow = false;
	render::SceneTarget target{1280, 720};
	render::View view;
	view.Pipeline = pipeline;
	view.Target = &target;
	view.SnapshotId = "native-resolution-snapshot";
	view.Instances = std::span(&instance, 1);
	render::DataCaptureRequest request{
		.SnapshotId = view.SnapshotId,
		.Pipeline = pipeline,
		.CaptureNode = core::Name("data-capture"),
		.Channels =
			{render::DataCaptureChannel::RgbLinearHdr,
			 render::DataCaptureChannel::LinearDepth,
			 render::DataCaptureChannel::ShadingNormal,
			 render::DataCaptureChannel::PbrAlbedo,
			 render::DataCaptureChannel::PbrMaterial,
			 render::DataCaptureChannel::PbrEmissive,
			 render::DataCaptureChannel::ObjectIds,
			 render::DataCaptureChannel::SemanticMask,
			 render::DataCaptureChannel::PartMask,
			 render::DataCaptureChannel::AmbientOcclusion,
			 render::DataCaptureChannel::DirectionalResponse,
			 render::DataCaptureChannel::ShadowVisibility,
			 render::DataCaptureChannel::FirstSurfaceValidity,
			 render::DataCaptureChannel::SecondSurfaceDepth,
			 render::DataCaptureChannel::SecondSurfaceValidity},
		.ObjectLabels = {},
		.SemanticLabels = {},
		.PartLabels = {},
	};
	render::DataCaptureTicket ticket;
	REQUIRE(renderer.QueueDataCapture(request, ticket));
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);

	render::DataCapturePoll poll;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		poll = renderer.PollDataCapture(ticket);
		if (poll.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (poll.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == render::DataCaptureStatus::Ready);
	REQUIRE(poll.Planes.size() == request.Channels.size());
	bool hasNativeResolutionPlane = false;
	const render::DataCapturePlane *firstSurfaceValidity = nullptr;
	const render::DataCapturePlane *pbrEmissive = nullptr;
	const render::DataCapturePlane *directionalResponse = nullptr;
	const render::DataCapturePlane *shadowVisibility = nullptr;
	for (const render::DataCapturePlane &plane : poll.Planes) {
		CHECK(plane.Status == render::DataCaptureStatus::Ready);
		hasNativeResolutionPlane =
			hasNativeResolutionPlane || (plane.Width >= target.Width && plane.Height >= target.Height);
		if (plane.Channel == render::DataCaptureChannel::FirstSurfaceValidity) firstSurfaceValidity = &plane;
		if (plane.Channel == render::DataCaptureChannel::PbrEmissive) pbrEmissive = &plane;
		if (plane.Channel == render::DataCaptureChannel::DirectionalResponse) directionalResponse = &plane;
		if (plane.Channel == render::DataCaptureChannel::ShadowVisibility) shadowVisibility = &plane;
	}
	CHECK(hasNativeResolutionPlane);
	REQUIRE(firstSurfaceValidity != nullptr);
	CHECK(firstSurfaceValidity->Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(
		firstSurfaceValidity->Provenance ==
		"first_surface_depth_test/v1;surface=visible_builtin_opaque_or_masked;"
		"background=0;validity=0_or_255;transparent_geometry=excluded;amodal_ground_truth=false"
	);
	const auto values = std::span(firstSurfaceValidity->Bytes);
	CHECK(std::find(values.begin(), values.end(), std::byte{255}) != values.end());
	CHECK(std::find(values.begin(), values.end(), std::byte{0}) != values.end());
	REQUIRE(pbrEmissive != nullptr);
	REQUIRE(pbrEmissive->Scalar == render::DataCaptureScalar::Float16);
	float maximumEmissive = 0.0f;
	for (size_t offset = 0; offset + 1 < pbrEmissive->Bytes.size(); offset += sizeof(uint16_t)) {
		uint16_t packed = 0;
		std::memcpy(&packed, pbrEmissive->Bytes.data() + offset, sizeof(packed));
		maximumEmissive = std::max(maximumEmissive, data_capture_compact::Float16ToFloat32(packed));
	}
	CHECK(maximumEmissive > instance.EmissiveStrength - 16.0f / 255.0f);
	CHECK(maximumEmissive < instance.EmissiveStrength + 16.0f / 255.0f);
	REQUIRE(directionalResponse != nullptr);
	REQUIRE(shadowVisibility != nullptr);
	CHECK(directionalResponse->Scalar == render::DataCaptureScalar::Float32);
	CHECK(directionalResponse->RowStride == directionalResponse->Width * 16);
	CHECK(shadowVisibility->Scalar == render::DataCaptureScalar::UNorm8);
	CHECK(shadowVisibility->RowStride == shadowVisibility->Width);
	CHECK(shadowVisibility->Width == directionalResponse->Width);
	CHECK(shadowVisibility->Height == directionalResponse->Height);
	REQUIRE_FALSE(directionalResponse->Bytes.empty());
	REQUIRE_FALSE(shadowVisibility->Bytes.empty());
	const auto visibilityMatches = [&] {
		for (uint32_t y = 0; y < directionalResponse->Height; ++y) {
			for (uint32_t x = 0; x < directionalResponse->Width; ++x) {
				float responseVisibility = 0.0f;
				std::memcpy(
					&responseVisibility,
					directionalResponse->Bytes.data() + y * directionalResponse->RowStride + x * 16 + 12,
					sizeof(responseVisibility)
				);
				if (!std::isfinite(responseVisibility)) return false;
				const auto expected = static_cast<unsigned char>(
					std::lround(std::clamp(responseVisibility, 0.0f, 1.0f) * 255.0f)
				);
				if (std::to_integer<unsigned char>(
						shadowVisibility->Bytes[y * shadowVisibility->RowStride + x]
					) != expected) {
					return false;
				}
			}
		}
		return true;
	};
	CHECK(visibilityMatches());
}

TEST_CASE("script bridge retains an explicit packed capture plane", "[render][gpu][data-capture][.]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipeline("bridge-packed-data-capture");
	REQUIRE(renderer.SetPipeline(pipeline, graph));

	world::Universe worlds;
	const world::WorldId world = worlds.Create({.Name = core::Name("data-world")});
	world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[world](world::WorldId candidate, world::DataFactoryPauseScope, bool, std::string &) {
			return candidate == world;
		}
	);
	REQUIRE(
		session.Pause("data-world", world::DataFactoryPauseScope::AllSystems, 0).Status ==
		world::DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-world", snapshot).Status == world::DataFactoryStatus::Ok);
	ScriptDataCaptureBridge bridge(session, renderer);
	script::DataCaptureBridgeRequest request{
		.InstanceId = "data-world",
		.SnapshotId = snapshot,
		.Pipeline = std::string(pipeline.Text()),
		.CaptureNode = "data-capture",
		.Channels = {"linear_depth", "ambient_occlusion", "pbr_material", "packed_gpu"},
		.TemporalHistory = "preserve",
		.PackedPlanes = {},
	};
	request.PackedPlanes.push_back({
		.Name = "depth_ao_roughness",
		.Components = {
			{{"linear_depth", 0}, {"ambient_occlusion", 0}, {"pbr_material", 0}, {"pbr_material", 2}}
		},
	});
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	render::SceneTarget target{64, 64};
	render::View view;
	view.WorldName = core::Name("data-world");
	view.Pipeline = pipeline;
	view.Target = &target;
	view.SnapshotId = snapshot;
	bridge.PrepareView(view);
	render::OverlayImage overlay;
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);

	script::DataCaptureBridgePoll poll;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		bridge.Pump();
		REQUIRE(bridge.Poll("data-world", ticket, poll, detail));
		if (poll.Status == "pending") SDL_Delay(1);
	} while (poll.Status == "pending" && std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == "ready");
	const auto packed = std::ranges::find_if(poll.Planes, [](const script::DataCaptureBridgePlane &plane) {
		return plane.Channel == "packed/depth_ao_roughness";
	});
	REQUIRE(packed != poll.Planes.end());
	CHECK(packed->Scalar == "float32");
	CHECK(packed->Packing == "rgba32_float");
	CHECK(packed->Resampling == "pixel_center_nearest/v1");
	REQUIRE(packed->Packed);
	CHECK(packed->Packed->Components[0].SourceChannel == "linear_depth");
	const auto gpuPacked = std::ranges::find_if(poll.Planes, [](const script::DataCaptureBridgePlane &plane) {
		return plane.Channel == "packed_gpu";
	});
	REQUIRE(gpuPacked != poll.Planes.end());
	CHECK(gpuPacked->Scalar == "float32");
	CHECK(gpuPacked->Packing == "rgba32_float");
	CHECK(
		gpuPacked->Provenance ==
		"render_graph_pack_channels/v1;mapping=author_defined;resampling=pixel_center_nearest;extent=r"
	);
	std::vector<std::byte> bytes, gpuBytes;
	REQUIRE(bridge.ReadPlane("data-world", ticket, packed->Resource, 0, packed->ByteSize, bytes, detail));
	REQUIRE(
		bridge.ReadPlane("data-world", ticket, gpuPacked->Resource, 0, gpuPacked->ByteSize, gpuBytes, detail)
	);
	CHECK(bytes.size() == packed->ByteSize);
	CHECK(packed->RowStride == packed->Width * 16);
	CHECK(gpuPacked->RowStride == gpuPacked->Width * 16);
	CHECK(gpuBytes == bytes);
	REQUIRE(bridge.Release("data-world", ticket, detail));
}

TEST_CASE(
	"data capture timestamps its later batch camera while profiling is off", "[render][gpu][data-capture][.]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipeline("data-capture-gpu-timing");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	renderer.SetProfiling(render::ProfilingTier::Off);

	render::SceneTarget firstTarget{64, 64};
	render::SceneTarget captureTarget{64, 64};
	render::View first;
	first.Pipeline = pipeline;
	first.Target = &firstTarget;
	first.Slot = 1;
	render::View capture;
	capture.Pipeline = pipeline;
	capture.Target = &captureTarget;
	capture.Slot = 2;
	capture.SnapshotId = "gpu-timing-snapshot";
	render::OverlayImage overlay;

	render::DataCaptureTicket ticket;
	REQUIRE(renderer.QueueDataCapture(
		{.SnapshotId = capture.SnapshotId,
		 .Pipeline = pipeline,
		 .CaptureNode = core::Name("data-capture"),
		 .ViewSlot = capture.Slot,
		 .Channels = {render::DataCaptureChannel::RgbLinearHdr},
		 .ObjectLabels = {},
		 .SemanticLabels = {},
		 .PartLabels = {}},
		ticket
	));
	const std::array views{first, capture};
	REQUIRE(renderer.Render(views, overlay, nullptr, false).Submitted);

	render::DataCapturePoll poll;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		poll = renderer.PollDataCapture(ticket);
		if (poll.Status == render::DataCaptureStatus::Pending) SDL_Delay(1);
	} while (poll.Status == render::DataCaptureStatus::Pending &&
			 std::chrono::steady_clock::now() < deadline);
	REQUIRE(poll.Status == render::DataCaptureStatus::Ready);
	REQUIRE(poll.GpuNanoseconds);
	CHECK(*poll.GpuNanoseconds > 0);
	CHECK(poll.GpuTimingReason.empty());
	CHECK(poll.HostReadbackReservedCapacityBytes > 0);
	CHECK(poll.DeviceReadbackStagingReservedCapacityBytes > 0);
}

TEST_CASE("capture bundle alignment includes every camera and extent field", "[render][data-capture]") {
	ResourceImage first;
	first.SnapshotId = "snapshot";
	first.CaptureFrame = 4;
	first.CameraWorldFromCamera[0] = 1;
	first.CameraProjectionAvailable = true;
	first.CameraProjection[0] = 2;
	first.CameraFieldOfViewRadians = 1;
	first.CameraNearPlane = .1f;
	first.CameraFarPlane = 100;
	first.CameraCropLeft = .1f;
	first.CameraCropTop = .2f;
	first.CameraCropWidth = .7f;
	first.CameraCropHeight = .6f;
	first.CaptureWidth = 64;
	first.CaptureHeight = 48;
	CHECK(capture_record_validation::SameCaptureBundle(first, first));
	const auto rejects = [&](auto change) {
		ResourceImage candidate = first;
		change(candidate);
		CHECK_FALSE(capture_record_validation::SameCaptureBundle(first, candidate));
	};
	rejects([](ResourceImage &image) { image.SnapshotId = "other"; });
	rejects([](ResourceImage &image) { ++image.CaptureFrame; });
	rejects([](ResourceImage &image) { image.CameraWorldFromCamera[0] = 3; });
	rejects([](ResourceImage &image) { image.CameraProjectionAvailable = false; });
	rejects([](ResourceImage &image) { image.CameraProjection[0] = 3; });
	rejects([](ResourceImage &image) { image.CameraFieldOfViewRadians = 2; });
	rejects([](ResourceImage &image) { image.CameraNearPlane = .2f; });
	rejects([](ResourceImage &image) { image.CameraFarPlane = 200; });
	rejects([](ResourceImage &image) { image.CameraCropLeft = .2f; });
	rejects([](ResourceImage &image) { image.CameraCropTop = .3f; });
	rejects([](ResourceImage &image) { image.CameraCropWidth = .6f; });
	rejects([](ResourceImage &image) { image.CameraCropHeight = .5f; });
	rejects([](ResourceImage &image) { ++image.CaptureWidth; });
	rejects([](ResourceImage &image) { ++image.CaptureHeight; });
}

TEST_CASE("data capture refuses a non-rendering history policy before queueing", "[render][data-capture]") {
	Renderer renderer;
	DataCaptureRequest request{
		.SnapshotId = "snapshot-1",
		.Pipeline = engine::core::Name("capture-pipeline"),
		.CaptureNode = engine::core::Name("capture"),
		.Channels = {DataCaptureChannel::RgbLinearHdr},
		.ObjectLabels = {},
		.SemanticLabels = {},
		.PartLabels = {},
		.TemporalHistory = DataCaptureTemporalHistory::Reset,
	};
	DataCaptureTicket ticket;
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	CHECK(ticket.ResourceTokens.empty());

	request.TemporalHistory = DataCaptureTemporalHistory::Preserve;
	request.SnapshotId.clear();
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	request.SnapshotId = "snapshot-1";
	request.Channels = {DataCaptureChannel::SecondSurfaceDepth};
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	CHECK(ticket.ChannelResourceIndices.empty());

	request.Channels = {DataCaptureChannel::LocalLightContribution};
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
	request.LocalLightIds = {"light/key", "light/key"};
	CHECK_FALSE(renderer.QueueDataCapture(request, ticket));
}

TEST_CASE("data capture expands each requested local light into one logical plane", "[render][gpu][data-capture]") {
	using namespace engine;
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	graph::RenderGraph graph;
	core::Name offender;
	REQUIRE(
		graph::Build(graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		graph::PipelineDocumentStatus::Ok
	);
	const core::Name pipeline("local-light-capture-expansion");
	REQUIRE(renderer.SetPipeline(pipeline, graph));

	DataCaptureRequest request{
		.SnapshotId = "local-light-snapshot",
		.Pipeline = pipeline,
		.CaptureNode = core::Name("data-capture"),
		.Channels = {DataCaptureChannel::LocalLightContribution},
		.ObjectLabels = {},
		.SemanticLabels = {},
		.PartLabels = {},
		.LocalLightIds = {"light/key", "light/fill"},
	};
	DataCaptureTicket ticket;
	REQUIRE(renderer.QueueDataCapture(request, ticket));
	CHECK(
		ticket.Channels ==
		std::vector<DataCaptureChannel>{
			DataCaptureChannel::LocalLightContribution, DataCaptureChannel::LocalLightContribution
		}
	);
	CHECK(ticket.LightIds == std::vector<std::string>{"light/key", "light/fill"});
	CHECK(ticket.LocalLightMatched == std::vector<uint8_t>{0, 0});
	CHECK(ticket.ChannelResourceIndices.size() == 2);
	CHECK(ticket.ResourceTokens.size() == 2);
	renderer.CancelDataCapture(ticket);
}

TEST_CASE(
	"capture record validation rejects hostile planes and status mismatches", "[render][data-capture]"
) {
	DataCaptureTicket ticket;
	ticket.CaptureNode = engine::core::Name("capture");
	ticket.Channels = {DataCaptureChannel::RgbLinearHdr};
	DataCapturePlane plane;
	plane.Channel = DataCaptureChannel::RgbLinearHdr;
	plane.CaptureNode = ticket.CaptureNode;
	plane.Status = DataCaptureStatus::Ready;
	plane.Resource = engine::core::Name("lit");
	plane.Width = 1;
	plane.Height = 1;
	plane.RowStride = 0;
	plane.Scalar = DataCaptureScalar::Float16;
	plane.ColourSpace = DataCaptureColourSpace::Linear;
	capture_record_validation::State state;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 1, state));
	plane.Status = DataCaptureStatus::Pending;
	plane.Resource = engine::core::Name{};
	plane.Width = 0;
	plane.Height = 0;
	plane.RowStride = 0;
	plane.Scalar = DataCaptureScalar::Unknown;
	plane.ColourSpace = DataCaptureColourSpace::Unknown;
	capture_record_validation::State pending;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 1, pending));
	plane.Status = DataCaptureStatus::Unsupported;
	plane.Resource = engine::core::Name{};
	plane.Scalar = DataCaptureScalar::Unknown;
	plane.ColourSpace = DataCaptureColourSpace::Unknown;
	CHECK_FALSE(capture_record_validation::Plane(ticket, plane, 1, state));
	DataCapturePlane duplicate = plane;
	duplicate.Width = 0;
	duplicate.Height = 0;
	duplicate.RowStride = 0;
	capture_record_validation::State duplicates;
	CHECK(capture_record_validation::Plane(ticket, duplicate, 1, duplicates));
	CHECK_FALSE(capture_record_validation::Plane(ticket, duplicate, 1, duplicates));

	ticket.Channels = {DataCaptureChannel::AmbientOcclusion};
	DataCapturePlane ambient;
	ambient.Channel = DataCaptureChannel::AmbientOcclusion;
	ambient.CaptureNode = ticket.CaptureNode;
	ambient.Status = DataCaptureStatus::Ready;
	ambient.Resource = engine::core::Name("occlusion");
	ambient.Width = 2;
	ambient.Height = 1;
	ambient.RowStride = 2;
	ambient.Scalar = DataCaptureScalar::UNorm8;
	ambient.ColourSpace = DataCaptureColourSpace::NotApplicable;
	ambient.Bytes = {std::byte{0}, std::byte{255}};
	ambient.Hash = engine::assets::Hasher::Of(ambient.Bytes);
	ambient.AmbientOcclusion = {
		.SourceState = AmbientOcclusionSourceState::Unavailable,
		.ProducerFrame = std::nullopt,
		.Enabled = std::nullopt,
		.SampleCount = std::nullopt,
		.RadiusWorldUnits = std::nullopt,
		.Denoiser = std::nullopt,
		.TemporalHistory = std::nullopt,
		.BackgroundValue = std::nullopt,
		.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
	};
	capture_record_validation::State ambientState;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, ambientState));
	ambient.AmbientOcclusion->Enabled = true;
	capture_record_validation::State unavailableWithFacts;
	CHECK_FALSE(capture_record_validation::Plane(ticket, ambient, 1, unavailableWithFacts));
	ambient.AmbientOcclusion = {
		.SourceState = AmbientOcclusionSourceState::Estimated,
		.ProducerFrame = 7,
		.Enabled = true,
		.SampleCount = 12,
		.RadiusWorldUnits = 0.65f,
		.Denoiser = AmbientOcclusionDenoiser::None,
		.TemporalHistory = AmbientOcclusionTemporalHistory::Disabled,
		.BackgroundValue = 1.0f,
		.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
	};
	capture_record_validation::State estimated;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, estimated));
	ambient.AmbientOcclusion->SourceState = AmbientOcclusionSourceState::ClearedDisabled;
	ambient.AmbientOcclusion->Enabled = false;
	capture_record_validation::State disabled;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, disabled));
	ambient.AmbientOcclusion = {
		.SourceState = AmbientOcclusionSourceState::ClearedNoPass,
		.ProducerFrame = 8,
		.Enabled = true,
		.SampleCount = std::nullopt,
		.RadiusWorldUnits = std::nullopt,
		.Denoiser = std::nullopt,
		.TemporalHistory = std::nullopt,
		.BackgroundValue = 1.0f,
		.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
	};
	capture_record_validation::State noPass;
	CHECK(capture_record_validation::Plane(ticket, ambient, 1, noPass));
	ambient.RowStride = 1;
	capture_record_validation::State malformedAmbient;
	CHECK_FALSE(capture_record_validation::Plane(ticket, ambient, 1, malformedAmbient));
}

TEST_CASE("local-light planes validate per light and preserve unavailable identity", "[render][data-capture]") {
	DataCaptureTicket ticket;
	ticket.CaptureNode = engine::core::Name("capture");
	ticket.Channels = {
		DataCaptureChannel::LocalLightContribution,
		DataCaptureChannel::LocalLightContribution,
	};
	ticket.LightIds = {"light/key", "light/culled"};

	DataCapturePlane ready;
	ready.Channel = DataCaptureChannel::LocalLightContribution;
	ready.CaptureNode = ticket.CaptureNode;
	ready.Status = DataCaptureStatus::Ready;
	ready.Resource = engine::core::Name("local-light-key");
	ready.LightId = "light/key";
	ready.Width = 1;
	ready.Height = 1;
	ready.RowStride = 8;
	ready.Scalar = DataCaptureScalar::Float16;
	ready.ColourSpace = DataCaptureColourSpace::Linear;
	ready.Provenance = "local_light_contribution/v1;source=single_selected_local_light;"
					 "radiance=additive_linear_before_tonemap;encoding=rgba16_float";
	ready.Bytes.assign(8, std::byte{0});
	ready.Hash = engine::assets::Hasher::Of(ready.Bytes);

	DataCapturePlane unavailable;
	unavailable.Channel = DataCaptureChannel::LocalLightContribution;
	unavailable.CaptureNode = ticket.CaptureNode;
	unavailable.Status = DataCaptureStatus::Unsupported;
	unavailable.LightId = "light/culled";
	unavailable.Provenance = "unavailable/local_light_not_visible_or_culled/v1";

	capture_record_validation::State state;
	CHECK(capture_record_validation::Plane(ticket, ready, 17, state));
	CHECK(capture_record_validation::Plane(ticket, unavailable, 17, state));

	DataCapturePlane duplicate = ready;
	duplicate.Resource = engine::core::Name("local-light-key-duplicate");
	CHECK_FALSE(capture_record_validation::Plane(ticket, duplicate, 17, state));
}

TEST_CASE("second surface capture records one strict aligned provenance pair", "[render][data-capture]") {
	constexpr std::string_view suffix =
		";eligibility=built_in_plain_opaque_front_facing;invalid_depth_metres=0;"
		"validity=0_or_255;identity=unavailable;amodal_ground_truth=false";
	const std::string provenance =
		"second_surface_depth_peel/v1;source_depth=d24_unorm;equality_bias=one_source_quantum" +
		std::string(suffix);
	CHECK(capture_record_validation::ValidSecondSurfaceProvenance(provenance));
	CHECK_FALSE(
		capture_record_validation::ValidSecondSurfaceProvenance(
			"second_surface_depth_peel/v1;source_depth=d24_unorm;equality_bias=next_representable_float" +
			std::string(suffix)
		)
	);
	const auto d16 = SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_D16_UNORM);
	const auto d24 = SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT);
	const auto d32 = SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
	REQUIRE(d16);
	REQUIRE(d24);
	REQUIRE(d32);
	CHECK(d16->SourceQuantum == 1.0f / 65535.0f);
	CHECK_FALSE(d16->NextRepresentable);
	CHECK(d24->SourceQuantum == 1.0f / 16777215.0f);
	CHECK_FALSE(d24->NextRepresentable);
	CHECK(d32->SourceQuantum == 0);
	CHECK(d32->NextRepresentable);
	for (const SDL_GPUTextureFormat format :
		 {SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTUREFORMAT_D32_FLOAT})
		CHECK(capture_record_validation::ValidSecondSurfaceProvenance(SecondSurfaceProvenance(format)));
	CHECK_FALSE(SecondSurfaceRule(SDL_GPU_TEXTUREFORMAT_R8_UNORM));

	DataCaptureTicket ticket;
	ticket.CaptureNode = engine::core::Name("capture");
	ticket.Channels = {
		DataCaptureChannel::SecondSurfaceDepth,
		DataCaptureChannel::SecondSurfaceValidity,
	};
	DataCapturePlane depth;
	depth.Channel = DataCaptureChannel::SecondSurfaceDepth;
	depth.CaptureNode = ticket.CaptureNode;
	depth.Status = DataCaptureStatus::Ready;
	depth.Resource = engine::core::Name("second-surface-depth");
	depth.Width = 2;
	depth.Height = 1;
	depth.RowStride = 8;
	depth.Scalar = DataCaptureScalar::Float32;
	depth.ColourSpace = DataCaptureColourSpace::NotApplicable;
	depth.Provenance = provenance;
	depth.Bytes.assign(8, std::byte{0});
	depth.Hash = engine::assets::Hasher::Of(depth.Bytes);
	DataCapturePlane validity;
	validity.Channel = DataCaptureChannel::SecondSurfaceValidity;
	validity.CaptureNode = ticket.CaptureNode;
	validity.Status = DataCaptureStatus::Ready;
	validity.Resource = engine::core::Name("second-surface-validity");
	validity.Width = 2;
	validity.Height = 1;
	validity.RowStride = 2;
	validity.Scalar = DataCaptureScalar::UNorm8;
	validity.ColourSpace = DataCaptureColourSpace::NotApplicable;
	validity.Provenance = provenance;
	validity.Bytes = {std::byte{0}, std::byte{255}};
	validity.Hash = engine::assets::Hasher::Of(validity.Bytes);

	capture_record_validation::State aligned;
	CHECK(capture_record_validation::Plane(ticket, depth, 1, aligned));
	CHECK(capture_record_validation::Plane(ticket, validity, 1, aligned));
	validity.Width = 1;
	validity.RowStride = 1;
	validity.Bytes = {std::byte{255}};
	validity.Hash = engine::assets::Hasher::Of(validity.Bytes);
	capture_record_validation::State wrongExtent;
	CHECK(capture_record_validation::Plane(ticket, depth, 1, wrongExtent));
	CHECK_FALSE(capture_record_validation::Plane(ticket, validity, 1, wrongExtent));
	validity.Status = DataCaptureStatus::Unsupported;
	validity.Resource = {};
	validity.Width = validity.Height = validity.RowStride = 0;
	validity.Scalar = DataCaptureScalar::Unknown;
	validity.ColourSpace = DataCaptureColourSpace::Unknown;
	validity.Bytes.clear();
	validity.Hash = {};
	validity.Provenance.clear();
	capture_record_validation::State mismatchedTerminal;
	CHECK(capture_record_validation::Plane(ticket, depth, 1, mismatchedTerminal));
	CHECK_FALSE(capture_record_validation::Plane(ticket, validity, 1, mismatchedTerminal));
}

namespace {
	engine::script::DataCaptureBridgeRequest Request(std::string_view instanceId = "data-world") {
		return {
			.InstanceId = std::string(instanceId),
			.SnapshotId = "snapshot-1",
			.Pipeline = "pipeline",
			.CaptureNode = "capture",
			.Channels = {"rgb_linear_hdr"},
			.TemporalHistory = "preserve",
			.PackedPlanes = {},
		};
	}
	engine::script::ViewCameraMutationRequest MutationRequest(
		std::string_view instanceId = "data-world",
		std::string_view pipeline = "pipeline",
		uint64_t revision = 1,
		std::string_view snapshot = "snapshot-1",
		size_t slot = 0
	) {
		engine::script::ViewCameraMutationRequest request;
		request.InstanceId = instanceId;
		request.SnapshotId = snapshot;
		request.Pipeline = pipeline;
		request.PipelineRevision = revision;
		request.ViewSlot = slot;
		request.Lens = engine::script::ViewCameraMutationRequest::Camera{
			.FieldOfViewRadians = 0.9f,
			.NearPlane = 0.2f,
			.FarPlane = 100.0f,
			.MaxImageWidth = 0,
			.MaxImageHeight = 0,
			.ImageWidth = 0,
			.ImageHeight = 0
		};
		return request;
	}

	std::string
	PauseAndSnapshot(engine::world::Universe &worlds, engine::world::DataFactorySession &session) {
		const auto world = worlds.Create({.Name = engine::core::Name("data-world")});
		session.SetPauseParticipant(
			[world](
				engine::world::WorldId candidate, engine::world::DataFactoryPauseScope, bool, std::string &
			) { return candidate == world; }
		);
		REQUIRE(
			session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
			engine::world::DataFactoryStatus::Ok
		);
		std::string snapshot;
		REQUIRE(session.Snapshot("data-world", snapshot).Status == engine::world::DataFactoryStatus::Ok);
		return snapshot;
	}

	engine::render::ViewMutationIdentity MutationIdentity(
		std::string_view snapshot, engine::core::Name pipeline, uint64_t revision, size_t slot = 0
	) {
		return {
			.WorldName = "data-world",
			.SnapshotId = std::string(snapshot),
			.Pipeline = pipeline,
			.PipelineRevision = revision,
			.ViewSlot = slot,
		};
	}

	engine::render::View
	MutationView(std::string_view snapshot, engine::core::Name pipeline, size_t slot = 0) {
		engine::render::View view;
		view.WorldName = engine::core::Name("data-world");
		view.SnapshotId = snapshot;
		view.Pipeline = pipeline;
		view.Slot = slot;
		return view;
	}
}

TEST_CASE("script view.camera lifecycle is owner-pumped and reclaimable", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	for (size_t index = 0; index < MAX_DATA_FACTORY_BATCHES; ++index) {
		uint64_t ticket = 0;
		REQUIRE(bridge.QueueViewCameraMutation("data-world", MutationRequest(), ticket, detail));
		CHECK(bridge.HasPending());
		bridge.CancelViewCameraMutation("data-world", ticket);
		bridge.Pump();
		engine::script::ViewCameraMutationPoll poll;
		REQUIRE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
		CHECK(poll.Terminal);
		CHECK(poll.Status == "cancelled");
		CHECK_FALSE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
	}
}

TEST_CASE("script view.camera validates a queued snapshot on its owner", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-owner-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t ticket = 0;
	REQUIRE(bridge.QueueViewCameraMutation(
		"missing-world",
		MutationRequest("missing-world", pipeline.Text(), installed->Revision),
		ticket,
		detail
	));
	View view;
	view.WorldName = engine::core::Name("missing-world");
	view.Pipeline = pipeline;
	bridge.PrepareView(view);
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("missing-world", ticket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "stale");
}

TEST_CASE(
	"script view.camera rejects an armed patch after its snapshot becomes stale", "[render][data-capture]"
) {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-armed-stale-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t ticket = 0;
	REQUIRE(bridge.QueueViewCameraMutation(
		"data-world",
		MutationRequest("data-world", pipeline.Text(), installed->Revision, snapshot),
		ticket,
		detail
	));
	View armed = MutationView(snapshot, pipeline);
	bridge.PrepareView(armed);
	REQUIRE(session.Resume("data-world", 0).Status == engine::world::DataFactoryStatus::Ok);
	View fresh;
	fresh.WorldName = engine::core::Name("data-world");
	fresh.Pipeline = pipeline;
	bridge.PrepareView(fresh);
	CHECK(fresh.SnapshotId.empty());
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "stale");
	CHECK_FALSE(
		renderer.Hooks().ConsumeViewMutation(MutationIdentity(snapshot, pipeline, installed->Revision), fresh)
	);
}

TEST_CASE("script view.camera bridge tracks apply, restore and cancellation", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-lifecycle-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t appliedTicket = 0;
	REQUIRE(bridge.QueueViewCameraMutation(
		"data-world",
		MutationRequest("data-world", pipeline.Text(), installed->Revision, snapshot),
		appliedTicket,
		detail
	));
	View armedView = MutationView(snapshot, pipeline);
	SceneTarget target{16, 16};
	armedView.Target = &target;
	armedView.World = 1;
	bridge.PrepareView(armedView);

	// A host can return before Render. The armed patch must still choose its
	// admitted snapshot for the next fresh view rather than waiting forever.
	View view;
	view.WorldName = engine::core::Name("data-world");
	view.Pipeline = pipeline;
	view.Target = &target;
	view.World = 1;
	bridge.PrepareView(view);
	CHECK(view.SnapshotId == snapshot);
	const auto identity = MutationIdentity(snapshot, pipeline, installed->Revision);
	OverlayImage overlay;
	const FrameResult appliedFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(appliedFrame.Submitted);
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("data-world", appliedTicket, poll, detail));
	CHECK_FALSE(poll.Terminal);
	CHECK(poll.Status == "pending");

	// The completed patched image may need its unmodified replacement even if
	// the scene advances before the restore view is prepared.
	REQUIRE(session.Resume("data-world", 0).Status == engine::world::DataFactoryStatus::Ok);
	bridge.PrepareView(view);
	const FrameResult restoredFrame = renderer.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(restoredFrame.Submitted);
	bridge.Pump();
	REQUIRE(bridge.PollViewCameraMutation("data-world", appliedTicket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "applied");

	REQUIRE(
		session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	std::string cancellationSnapshot;
	REQUIRE(
		session.Snapshot("data-world", cancellationSnapshot).Status == engine::world::DataFactoryStatus::Ok
	);
	view.SnapshotId = cancellationSnapshot;
	uint64_t cancelledTicket = 0;
	REQUIRE(bridge.QueueViewCameraMutation(
		"data-world",
		MutationRequest("data-world", pipeline.Text(), installed->Revision, cancellationSnapshot),
		cancelledTicket,
		detail
	));
	bridge.PrepareView(view);
	REQUIRE(renderer.Render(std::span(&view, 1), overlay, nullptr, false).Submitted);
	bridge.Pump();
	bridge.CancelViewCameraMutation("data-world", cancelledTicket);
	bridge.Pump();
	REQUIRE(bridge.PollViewCameraMutation("data-world", cancelledTicket, poll, detail));
	CHECK_FALSE(poll.Terminal);
	// Shutdown owns no future view to restore. It must retire this applied patch
	// without waiting for another render frame.
	bridge.CancelPending();
	bridge.Pump();
	CHECK_FALSE(bridge.HasPending());
	REQUIRE(bridge.PollViewCameraMutation("data-world", cancelledTicket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "cancelled");
}

TEST_CASE(
	"script view.camera bridge reclaims applied slots after stale pipeline and teardown",
	"[render][data-capture]"
) {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("mutation-stale-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	const auto installed = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(installed);
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t staleTicket = 0;
	REQUIRE(bridge.QueueViewCameraMutation(
		"data-world",
		MutationRequest("data-world", pipeline.Text(), installed->Revision, snapshot),
		staleTicket,
		detail
	));
	View staleView = MutationView(snapshot, pipeline);
	bridge.PrepareView(staleView);
	const auto staleIdentity = MutationIdentity(snapshot, pipeline, installed->Revision);
	View prepared = staleView;
	REQUIRE(renderer.Hooks().ConsumeViewMutation(staleIdentity, prepared));
	bridge.Pump();
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	bridge.Pump();
	engine::script::ViewCameraMutationPoll poll;
	REQUIRE(bridge.PollViewCameraMutation("data-world", staleTicket, poll, detail));
	CHECK(poll.Terminal);
	CHECK(poll.Status == "stale");

	const auto current = renderer.ResolvePipelineIdentity(pipeline);
	REQUIRE(current);
	for (size_t slot = 0; slot < MAX_DATA_FACTORY_BATCHES; ++slot) {
		uint64_t ticket = 0;
		REQUIRE(bridge.QueueViewCameraMutation(
			"data-world",
			MutationRequest("data-world", pipeline.Text(), current->Revision, snapshot, slot),
			ticket,
			detail
		));
		View view = MutationView(snapshot, pipeline, slot);
		bridge.PrepareView(view);
		View applied = view;
		REQUIRE(renderer.Hooks().ConsumeViewMutation(
			MutationIdentity(snapshot, pipeline, current->Revision, slot), applied
		));
		bridge.Pump();
	}
	REQUIRE(bridge.TeardownInstance("data-world", detail));
	for (size_t slot = 0; slot < MAX_DATA_FACTORY_BATCHES; ++slot) {
		uint64_t ticket = 0;
		REQUIRE(bridge.QueueViewCameraMutation(
			"data-world",
			MutationRequest("data-world", pipeline.Text(), current->Revision, snapshot, slot),
			ticket,
			detail
		));
		View view = MutationView(snapshot, pipeline, slot);
		bridge.PrepareView(view);
		View applied = view;
		REQUIRE(renderer.Hooks().ConsumeViewMutation(
			MutationIdentity(snapshot, pipeline, current->Revision, slot), applied
		));
		bridge.Pump();
		REQUIRE(bridge.PollViewCameraMutation("data-world", ticket, poll, detail));
		CHECK_FALSE(poll.Terminal);
	}
	REQUIRE(bridge.TeardownInstance("data-world", detail));
}

TEST_CASE("script capture retains terminal tickets until release", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	const auto request = Request();
	std::array<uint64_t, 6> tickets{};
	std::string detail;
	for (uint64_t &ticket : tickets)
		REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	uint64_t refused = 0;
	CHECK_FALSE(bridge.Queue("data-world", request, refused, detail));
	bridge.Cancel("data-world", tickets[2]);
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", tickets[2], reply, detail));
	CHECK(reply.Status == "cancelled");
	std::vector<std::byte> bytes;
	CHECK_FALSE(bridge.ReadPlane("data-world", tickets[2], "capture/3/rgb_linear_hdr", 0, 8, bytes, detail));
	REQUIRE(bridge.Release("data-world", tickets[2], detail));
	uint64_t reused = 0;
	CHECK(bridge.Queue("data-world", request, reused, detail));
}

TEST_CASE("script capture does not retain a scene sidecar for a stale snapshot", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("stale-sidecar-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	ScriptDataCaptureBridge bridge(session, renderer);
	auto request = Request();
	request.SnapshotId = snapshot;
	request.Pipeline = pipeline.Text();
	request.IncludeSceneData = true;
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	REQUIRE(session.Resume("data-world", 0).Status == engine::world::DataFactoryStatus::Ok);
	View view = MutationView(snapshot, pipeline);
	bridge.PrepareView(view);
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
	CHECK(reply.Status == "stale_snapshot");
	CHECK_FALSE(reply.SceneSidecar);
	REQUIRE(bridge.Release("data-world", ticket, detail));
	CHECK_FALSE(bridge.Poll("data-world", ticket, reply, detail));
}

TEST_CASE(
	"script capture terminally refuses invalid resource labels without advancing the world",
	"[render][data-capture]"
) {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	const std::string snapshot = PauseAndSnapshot(worlds, session);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("invalid-capture-label-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	ScriptDataCaptureBridge bridge(session, renderer);
	auto request = Request();
	request.SnapshotId = snapshot;
	request.Pipeline = pipeline.Text();
	request.Channels = {"object_ids"};
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	const auto before = session.Inspect("data-world");

	View view = MutationView(snapshot, pipeline);
	view.ObjectLabelsValid = false;
	bridge.PrepareView(view);
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
	CHECK(reply.Status == "invalid");
	CHECK(reply.SnapshotId == snapshot);
	CHECK(reply.Planes.empty());
	std::vector<std::byte> bytes;
	CHECK_FALSE(bridge.ReadPlane(
		"data-world", ticket, "capture/" + std::to_string(ticket) + "/object_ids", 0, 16, bytes, detail
	));
	CHECK(bytes.empty());
	CHECK(detail == "unknown capture resource");
	const auto after = session.Inspect("data-world");
	CHECK(after.Clock.Tick == before.Clock.Tick);
	CHECK(after.Clock.TimeNanoseconds == before.Clock.TimeNanoseconds);
	CHECK(after.WorldVersion == before.WorldVersion);
	CHECK(
		session.RenderSnapshotBarrier("data-world", snapshot).Status == engine::world::DataFactoryStatus::Ok
	);
	REQUIRE(bridge.Release("data-world", ticket, detail));
}

TEST_CASE(
	"named capture cameras resolve stable identities without changing a refused view",
	"[render][data-capture]"
) {
	engine::scene::RegisterSceneComponents();
	engine::world::Universe worlds;
	const auto world = worlds.Create({.Name = engine::core::Name("data-world")});
	worlds.Enter(world, [](engine::ecs::Store &store) {
		auto identify = [&](engine::ecs::Entity entity, std::string_view id) {
			engine::ecs::AttributeValue value;
			value.Type = engine::ecs::PropertyType::String;
			value.String = id;
			REQUIRE(engine::ecs::SetAttribute(store, entity, engine::core::Name("DataFactoryId"), value));
		};
		const auto first = store.Create();
		store.Set(first, engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{1, 2, 3})});
		engine::scene::Camera lens;
		lens.FieldOfViewRadians = .7f;
		store.Set(first, lens);
		identify(first, "camera/first");
		const auto second = store.Create();
		store.Set(second, engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{9, 8, 7})});
		lens.FieldOfViewRadians = 1.1f;
		store.Set(second, lens);
		identify(second, "camera/second");
		const auto duplicate = store.Create();
		identify(duplicate, "camera/second");
		const auto nonCamera = store.Create();
		store.Set(nonCamera, engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{4, 5, 6})});
		identify(nonCamera, "object/not-camera");
	});
	engine::world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[world](engine::world::WorldId candidate, engine::world::DataFactoryPauseScope, bool, std::string &) {
			return candidate == world;
		}
	);
	REQUIRE(
		session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-world", snapshot).Status == engine::world::DataFactoryStatus::Ok);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("named-camera-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	ScriptDataCaptureBridge bridge(session, renderer);
	auto request = Request();
	request.SnapshotId = snapshot;
	request.Pipeline = pipeline.Text();
	request.CameraId = "camera/first";
	uint64_t ticket = 0;
	std::string detail;
	REQUIRE(bridge.Queue("data-world", request, ticket, detail));
	View view = MutationView(snapshot, pipeline);
	view.CameraFrame = engine::core::CFrame(engine::core::Vector3{-1, -1, -1});
	view.Camera.FieldOfViewRadians = .2f;
	CHECK(bridge.PrepareView(view));
	CHECK(view.CameraFrame.Position.X == 1);
	CHECK(view.Camera.FieldOfViewRadians == .7f);
	bridge.Cancel("data-world", ticket);
	bridge.Pump();
	REQUIRE(bridge.Release("data-world", ticket, detail));

	for (const std::string_view rejected : {"missing", "camera/second", "object/not-camera"}) {
		request.CameraId = rejected;
		REQUIRE(bridge.Queue("data-world", request, ticket, detail));
		View refused = MutationView(snapshot, pipeline);
		refused.CameraFrame = engine::core::CFrame(engine::core::Vector3{-2, -2, -2});
		refused.Camera.FieldOfViewRadians = .3f;
		CHECK_FALSE(bridge.PrepareView(refused));
		CHECK(refused.CameraFrame.Position.X == -2);
		CHECK(refused.Camera.FieldOfViewRadians == .3f);
		CHECK(refused.SnapshotId == snapshot);
		engine::script::DataCaptureBridgePoll poll;
		REQUIRE(bridge.Poll("data-world", ticket, poll, detail));
		CHECK(poll.Status == "invalid");
	}
}

TEST_CASE(
	"interleaved named and current capture snapshots each get a preparation turn", "[render][data-capture]"
) {
	engine::scene::RegisterSceneComponents();
	engine::world::Universe worlds;
	const auto world = worlds.Create({.Name = engine::core::Name("data-world")});
	worlds.Enter(world, [](engine::ecs::Store &store) {
		const auto camera = store.Create();
		store.Set(camera, engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{3, 0, 0})});
		store.Set(camera, engine::scene::Camera{});
		engine::ecs::AttributeValue identity;
		identity.Type = engine::ecs::PropertyType::String;
		identity.String = "camera/selected";
		REQUIRE(engine::ecs::SetAttribute(store, camera, engine::core::Name("DataFactoryId"), identity));
	});
	engine::world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[world](engine::world::WorldId candidate, engine::world::DataFactoryPauseScope, bool, std::string &) {
			return candidate == world;
		}
	);
	REQUIRE(
		session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	std::string firstSnapshot, secondSnapshot;
	REQUIRE(session.Snapshot("data-world", firstSnapshot).Status == engine::world::DataFactoryStatus::Ok);
	REQUIRE(session.Snapshot("data-world", secondSnapshot).Status == engine::world::DataFactoryStatus::Ok);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("interleaved-capture-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	auto named = Request();
	named.SnapshotId = firstSnapshot;
	named.Pipeline = pipeline.Text();
	named.CameraId = "camera/selected";
	uint64_t namedTicket = 0;
	REQUIRE(bridge.Queue("data-world", named, namedTicket, detail));
	auto current = named;
	current.SnapshotId = secondSnapshot;
	current.CameraId = "current_view";
	uint64_t currentTicket = 0;
	REQUIRE(bridge.Queue("data-world", current, currentTicket, detail));
	View namedView = MutationView({}, pipeline);
	ScriptDataCaptureBridge::PreparedView prepared;
	CHECK(bridge.PrepareView(namedView, &prepared));
	CHECK(namedView.SnapshotId == firstSnapshot);
	CHECK_FALSE(namedView.CameraTemporalId.empty());
	REQUIRE(prepared.Captures == std::vector<uint64_t>{namedTicket});
	bridge.AbortPreparedView(prepared);
	engine::script::DataCaptureBridgePoll cancelled;
	REQUIRE(bridge.Poll("data-world", namedTicket, cancelled, detail));
	REQUIRE(bridge.Release("data-world", namedTicket, detail));
	View currentView = MutationView({}, pipeline);
	CHECK_FALSE(bridge.PrepareView(currentView));
	CHECK(currentView.SnapshotId == secondSnapshot);
	engine::script::DataCaptureBridgePoll poll;
	REQUIRE(bridge.Poll("data-world", currentTicket, poll, detail));
	CHECK(poll.Status == "failed");
	REQUIRE(bridge.Release("data-world", currentTicket, detail));
}

TEST_CASE("cancelling a same-frame camera group unblocks later groups", "[render][data-capture]") {
	engine::scene::RegisterSceneComponents();
	engine::world::Universe worlds;
	const auto world = worlds.Create({.Name = engine::core::Name("data-world")});
	worlds.Enter(world, [](engine::ecs::Store &store) {
		auto addCamera = [&](std::string_view id) {
			const auto camera = store.Create();
			store.Set(camera, engine::scene::Transform{});
			store.Set(camera, engine::scene::Camera{});
			engine::ecs::AttributeValue identity;
			identity.Type = engine::ecs::PropertyType::String;
			identity.String = id;
			REQUIRE(engine::ecs::SetAttribute(store, camera, engine::core::Name("DataFactoryId"), identity));
		};
		addCamera("camera/first");
		addCamera("camera/second");
	});
	engine::world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[world](engine::world::WorldId candidate, engine::world::DataFactoryPauseScope, bool, std::string &) {
			return candidate == world;
		}
	);
	REQUIRE(
		session.Pause("data-world", engine::world::DataFactoryPauseScope::AllSystems, 0).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	std::string snapshot;
	REQUIRE(session.Snapshot("data-world", snapshot).Status == engine::world::DataFactoryStatus::Ok);
	Renderer renderer;
	engine::graph::RenderGraph graph;
	engine::core::Name offender;
	REQUIRE(
		engine::graph::Build(engine::graph::DefaultPbrDataCaptureDocument(), graph, offender) ==
		engine::graph::PipelineDocumentStatus::Ok
	);
	const engine::core::Name pipeline("same-frame-cancellation-pipeline");
	REQUIRE(renderer.SetPipeline(pipeline, graph));
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	auto firstCamera = Request();
	firstCamera.SnapshotId = snapshot;
	firstCamera.Pipeline = pipeline.Text();
	firstCamera.CameraId = "camera/first";
	auto secondCamera = firstCamera;
	secondCamera.CameraId = "camera/second";
	const std::array requests{firstCamera, secondCamera};
	std::array<uint64_t, 2> cancelledTickets{};
	std::array<uint64_t, 2> laterTickets{};
	REQUIRE(bridge.QueueGroup("data-world", requests, cancelledTickets, detail));
	REQUIRE(bridge.QueueGroup("data-world", requests, laterTickets, detail));
	// Physical slot one belongs to the first prepared group view. An ordinary
	// logical-slot-one request must not attach to it merely because the numbers
	// match.
	auto ordinary = firstCamera;
	ordinary.CameraId = "camera/ordinary";
	ordinary.ViewSlot = 1;
	uint64_t ordinaryTicket = 0;
	REQUIRE(bridge.Queue("data-world", ordinary, ordinaryTicket, detail));

	bridge.Cancel("data-world", cancelledTickets.front());
	bridge.Pump();
	engine::script::DataCaptureBridgePoll poll;
	for (const uint64_t ticket : cancelledTickets) {
		REQUIRE(bridge.Poll("data-world", ticket, poll, detail));
		CHECK(poll.Status == "cancelled");
	}
	for (const uint64_t ticket : cancelledTickets)
		REQUIRE(bridge.Release("data-world", ticket, detail));

	std::vector<View> views;
	ScriptDataCaptureBridge::PreparedBatch prepared;
	REQUIRE(bridge.PrepareBatch(MutationView(snapshot, pipeline), views, &prepared));
	REQUIRE(views.size() == laterTickets.size());
	REQUIRE(prepared.Views.size() == laterTickets.size());
	CHECK_FALSE(views[0].CameraTemporalId.empty());
	CHECK_FALSE(views[1].CameraTemporalId.empty());
	CHECK(views[0].CameraTemporalId != views[1].CameraTemporalId);
	for (size_t index = 0; index < laterTickets.size(); ++index)
		CHECK(prepared.Views[index].Captures == std::vector<uint64_t>{laterTickets[index]});
	ScriptDataCaptureBridge::PreparedView firstPrepared;
	REQUIRE(bridge.PrepareView(views[0], &firstPrepared));
	CHECK(firstPrepared.Captures == std::vector<uint64_t>{laterTickets[0]});
	bridge.AbortPreparedView(firstPrepared);
	bridge.AbortPreparedBatch(prepared);
	for (const uint64_t ticket : laterTickets)
		REQUIRE(bridge.Release("data-world", ticket, detail));
	bridge.Cancel("data-world", ordinaryTicket);
	bridge.Pump();
	REQUIRE(bridge.Release("data-world", ordinaryTicket, detail));
}

TEST_CASE("script capture advertises the SSAO estimator channel", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	const auto capabilities = bridge.Capabilities();
	CHECK(
		std::find(capabilities.Channels.begin(), capabilities.Channels.end(), "ambient_occlusion") !=
		capabilities.Channels.end()
	);
	CHECK(capabilities.NamedCameraSelection);
	CHECK(capabilities.SameFrameMultiCamera);
	CHECK(capabilities.MaximumSameFrameCameraViews == 6);
	CHECK(capabilities.MaximumCameraIdBytes == engine::script::MAX_DATA_SCENE_ID_BYTES);
	const size_t observationHooks = static_cast<size_t>(
		std::count_if(capabilities.HookRecords.begin(), capabilities.HookRecords.end(), [](const auto &hook) {
			return hook.Access == "observation";
		})
	);
	REQUIRE(observationHooks == 21);
	CHECK(capabilities.HookRecords.size() == observationHooks + 1);
	for (const auto &hook : capabilities.HookRecords) {
		if (hook.Access != "observation") continue;
		CHECK(hook.Name.starts_with("data_capture."));
		CHECK(hook.SchemaVersion == 1);
		CHECK(hook.NodeKind == "capture");
		CHECK(hook.Required);
		REQUIRE(hook.Channels.size() == 1);
		CHECK(
			std::find(capabilities.Channels.begin(), capabilities.Channels.end(), hook.Channels.front()) !=
			capabilities.Channels.end()
		);
	}
	const auto mutation =
		std::find_if(capabilities.HookRecords.begin(), capabilities.HookRecords.end(), [](const auto &hook) {
			return hook.Name == "view.camera";
		});
	REQUIRE(mutation != capabilities.HookRecords.end());
	CHECK(mutation->Access == "synchronous_mutation");
	CHECK(mutation->MutatedFields.size() == 3);
	CHECK(capabilities.MaximumHooks == MAX_DATA_FACTORY_HOOKS);
	CHECK(capabilities.MaximumConnections == MAX_DATA_FACTORY_CONNECTIONS);
	CHECK(capabilities.MaximumBatches == MAX_DATA_FACTORY_BATCHES);
	CHECK(capabilities.MaximumReadbackNodes == 11);
}

TEST_CASE("script capture validates requests and isolates ticket owners", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge first(session, renderer);
	ScriptDataCaptureBridge second(session, renderer);
	std::string detail;
	uint64_t ticket = 0;

	auto malformed = Request();
	malformed.Channels = {"rgb_linear_hdr", "rgb_linear_hdr"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.CaptureNode = std::string("capture\0node", 12);
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.Channels = {"semantic_mask"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	malformed = Request();
	malformed.Channels = {"second_surface_depth"};
	CHECK_FALSE(first.Queue("data-world", malformed, ticket, detail));
	CHECK_FALSE(first.Queue("another-world", Request(), ticket, detail));
	const auto broad = engine::script::DataCaptureBridgeRequest{
		.InstanceId = "data-world",
		.SnapshotId = "snapshot-1",
		.Pipeline = "pipeline",
		.CaptureNode = "capture",
		.Channels =
			{"rgb_linear_hdr",
			 "linear_depth",
			 "shading_normal",
			 "pbr_albedo",
			 "pbr_material",
			 "pbr_emissive",
			 "ambient_occlusion",
			 "object_ids",
			 "semantic_ids",
			 "part_ids",
			 "second_surface_depth",
			 "second_surface_validity"},
		.TemporalHistory = "preserve",
		.PackedPlanes = {},
	};
	uint64_t broadTicket = 0;
	REQUIRE(first.Queue("data-world", broad, broadTicket, detail));

	uint64_t firstTicket = 0;
	uint64_t secondTicket = 0;
	std::array<uint64_t, 2> groupTickets{};
	auto firstCamera = Request();
	firstCamera.CameraId = "camera/first";
	auto secondCamera = firstCamera;
	secondCamera.CameraId = "camera/second";
	const std::array groupRequests{firstCamera, secondCamera};
	REQUIRE(first.QueueGroup("data-world", groupRequests, groupTickets, detail));
	CHECK(groupTickets[0] != 0);
	CHECK(groupTickets[1] != 0);
	CHECK(groupTickets[0] != groupTickets[1]);
	const std::array duplicateRequests{firstCamera, firstCamera};
	groupTickets = {17, 19};
	CHECK_FALSE(first.QueueGroup("data-world", duplicateRequests, groupTickets, detail));
	CHECK(groupTickets == std::array<uint64_t, 2>{});
	auto nonzeroSlot = groupRequests;
	nonzeroSlot[0].ViewSlot = 1;
	nonzeroSlot[1].ViewSlot = 1;
	groupTickets = {17, 19};
	CHECK_FALSE(first.QueueGroup("data-world", nonzeroSlot, groupTickets, detail));
	CHECK(groupTickets == std::array<uint64_t, 2>{});
	REQUIRE(first.Queue("data-world", Request(), firstTicket, detail));
	REQUIRE(second.Queue("data-world", Request(), secondTicket, detail));
	first.Cancel("another-world", firstTicket);
	first.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(first.Poll("data-world", firstTicket, reply, detail));
	CHECK(reply.Status == "pending");
	CHECK_FALSE(first.Poll("another-world", firstTicket, reply, detail));
	second.Cancel("data-world", secondTicket);
	second.Pump();
	REQUIRE(second.Poll("data-world", secondTicket, reply, detail));
	CHECK(reply.Status == "cancelled");
	REQUIRE(second.Release("data-world", secondTicket, detail));
	CHECK_FALSE(second.Poll("data-world", firstTicket, reply, detail));
}

TEST_CASE("script capture synchronizes cancellation with an owner pump", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t ticket = 0;
	REQUIRE(bridge.Queue("data-world", Request(), ticket, detail));
	std::atomic<bool> started = false;
	std::thread owner([&] {
		started = true;
		for (size_t index = 0; index < 16; ++index)
			bridge.Pump();
	});
	while (!started.load()) {}
	bridge.Cancel("data-world", ticket);
	owner.join();
	bridge.Pump();
	engine::script::DataCaptureBridgePoll reply;
	REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
	CHECK(reply.Status == "cancelled");
	REQUIRE(bridge.Release("data-world", ticket, detail));
}

TEST_CASE("script capture reuses capacity after sequential terminal releases", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;

	for (size_t index = 0; index < 8; ++index) {
		uint64_t ticket = 0;
		REQUIRE(bridge.Queue("data-world", Request(), ticket, detail));
		bridge.Cancel("data-world", ticket);
		bridge.Pump();
		engine::script::DataCaptureBridgePoll reply;
		REQUIRE(bridge.Poll("data-world", ticket, reply, detail));
		CHECK(reply.Status == "cancelled");
		std::vector<std::byte> bytes;
		CHECK_FALSE(bridge.ReadPlane("data-world", ticket, "capture/invalid", 0, 16, bytes, detail));
		REQUIRE(bridge.Release("data-world", ticket, detail));
	}
}

TEST_CASE("script capture owner cancellation drains every pending kind", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t captureTicket = 0;
	uint64_t mutationTicket = 0;
	REQUIRE(bridge.Queue("data-world", Request(), captureTicket, detail));
	REQUIRE(bridge.QueueViewCameraMutation("data-world", MutationRequest(), mutationTicket, detail));
	REQUIRE(bridge.HasPending());

	bridge.CancelPending();
	bridge.Pump();
	CHECK_FALSE(bridge.HasPending());

	engine::script::DataCaptureBridgePoll capture;
	REQUIRE(bridge.Poll("data-world", captureTicket, capture, detail));
	CHECK(capture.Status == "cancelled");
	REQUIRE(bridge.Release("data-world", captureTicket, detail));
	engine::script::ViewCameraMutationPoll mutation;
	REQUIRE(bridge.PollViewCameraMutation("data-world", mutationTicket, mutation, detail));
	CHECK(mutation.Terminal);
	CHECK(mutation.Status == "cancelled");
}

TEST_CASE("script capture tears down one instance without touching another", "[render][data-capture]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	Renderer renderer;
	ScriptDataCaptureBridge bridge(session, renderer);
	std::string detail;
	uint64_t retiring = 0;
	uint64_t retained = 0;
	REQUIRE(bridge.Queue("retiring-world", Request("retiring-world"), retiring, detail));
	REQUIRE(bridge.Queue("retained-world", Request("retained-world"), retained, detail));
	REQUIRE(bridge.TeardownInstance("retiring-world", detail));
	engine::script::DataCaptureBridgePoll reply;
	CHECK_FALSE(bridge.Poll("retiring-world", retiring, reply, detail));
	CHECK(bridge.Poll("retained-world", retained, reply, detail));
	uint64_t replacement = 0;
	CHECK(bridge.Queue("retiring-world", Request("retiring-world"), replacement, detail));
	CHECK_FALSE(bridge.TeardownInstance("", detail));
}
