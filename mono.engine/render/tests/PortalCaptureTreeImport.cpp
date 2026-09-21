#include "RenderFixture.hpp"

#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalGeometry.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.render.portalcapturetreeimport")
TEST_DEPENDS("engine.render.portalcapturetree")

namespace {
	using namespace engine;
	using namespace engine::render;

	PortalCaptureTree
	ImportTree(const PortalCaptureLensProgram &program, size_t nodes = 2, bool wide = false) {
		PortalCaptureTree tree;
		for (size_t index = 0; index < nodes; ++index) {
			PortalCaptureTreeNode node;
			node.Producer = {"producer" + std::to_string(index), "capture", 7, index + 1};
			node.Camera.Position[0] = static_cast<float>(index);
			auto &opaque = node.Layers.Opaque;
			opaque.Key = {index + 1, "door", 2, 3};
			opaque.Status = PortalImageStatus::Ok;
			opaque.Scope = PortalImageScope::OpaqueLighting;
			opaque.CaptureLighting.emplace();
			opaque.Width = opaque.Height = 1;
			opaque.RowStride = 8;
			opaque.Pixels.resize(8);
			opaque.Depth.resize(4);
			core::ByteWriter response;
			opaque.Normal.assign(4, std::byte{255});
			response.WriteFloat(.25f);
			response.WriteFloat(1);
			response.WriteFloat(2);
			response.WriteFloat(.5f);
			opaque.AmbientResponse.assign(response.Bytes().begin(), response.Bytes().end());
			opaque.LightingBaseline = opaque.AmbientResponse;
			opaque.DirectionalResponse = opaque.AmbientResponse;
			opaque.PixelHash = assets::Hasher::Of(opaque.Pixels);
			opaque.DepthHash = assets::Hasher::Of(opaque.Depth);
			opaque.NormalHash = assets::Hasher::Of(opaque.Normal);
			opaque.AmbientResponseHash = assets::Hasher::Of(opaque.AmbientResponse);
			opaque.LightingBaselineHash = assets::Hasher::Of(opaque.LightingBaseline);
			opaque.DirectionalResponseHash = assets::Hasher::Of(opaque.DirectionalResponse);
			node.Layers.Transparent.assign(2, opaque);
			for (auto &transparent : node.Layers.Transparent) {
				transparent.Normal.clear();
				transparent.AmbientResponse.clear();
				transparent.LightingBaseline.clear();
				transparent.DirectionalResponse.clear();
				transparent.NormalHash = {};
				transparent.AmbientResponseHash = {};
				transparent.LightingBaselineHash = {};
				transparent.DirectionalResponseHash = {};
			}
			if (index != 0) {
				node.Layers.SpatialOverlay = opaque;
				node.Layers.SpatialOverlay->Depth.clear();
				node.Layers.SpatialOverlay->Normal.clear();
				node.Layers.SpatialOverlay->AmbientResponse.clear();
				node.Layers.SpatialOverlay->LightingBaseline.clear();
				node.Layers.SpatialOverlay->DirectionalResponse.clear();
				node.Layers.SpatialOverlay->DepthHash = {};
				node.Layers.SpatialOverlay->NormalHash = {};
				node.Layers.SpatialOverlay->AmbientResponseHash = {};
				node.Layers.SpatialOverlay->LightingBaselineHash = {};
				node.Layers.SpatialOverlay->DirectionalResponseHash = {};
			}
			node.Layers.Lenses.Programs.push_back(program);
			PortalCaptureLens lens;
			lens.Shader = "tree-lens";
			lens.ProgramHash = program.Hash;
			node.Layers.Lenses.Entries.push_back(lens);
			tree.Nodes.push_back(std::move(node));
			if (index == 0) continue;
			PortalCaptureTreeEdge edge;
			edge.Parent = wide ? 0 : index - 1;
			edge.Child = index;
			edge.PortalKey = wide ? "door-" + std::to_string(index) : "door";
			edge.Scale = .25f;
			PortalGeometry geometry;
			geometry.Rows.emplace_back();
			std::string error;
			REQUIRE(EncodePortalGeometry(geometry, edge.Geometry, error));
			tree.Edges.push_back(std::move(edge));
		}
		REQUIRE(ValidPortalCaptureTree(tree));
		return tree;
	}
}

TEST_CASE(
	"capture tree imports publish and retire as one owned group", "[render][gpu][portal-tree-import][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	ShaderCompiler compiler;
	const auto compiled = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour; void main(){colour=vec4(1);}",
		ShaderStage::Fragment,
		"tree-lens"
	);
	INFO(compiled.Error);
	REQUIRE_FALSE(compiled.Failed);
	PortalCaptureLensProgram program{
		assets::Hasher::Of(std::as_bytes(std::span(compiled.SpirV))), compiled.SpirV
	};
	auto original = ImportTree(program);
	const auto lenses = original.Nodes.front().Layers.Lenses;
	PortalImageBinding binding;
	binding.World = 7;
	binding.WorldName = core::Name("consumer");
	binding.Portal = core::Name("door");
	binding.Expected = original.Nodes.front().Layers.Opaque.Key;
	binding.ExpectedScope = PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = PortalImageProjection::Eye;
	const auto token = fixture.Render.QueuePortalCaptureTree(binding, std::move(original));
	REQUIRE(token != 0);
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(token));
	CHECK(fixture.Render.FindPortalCaptureTree(token) == nullptr);
	CHECK(fixture.Render.PortalImageUsage().Images == 7);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes > 0);
	SceneTarget target{17, 17};
	View view;
	view.World = binding.World;
	view.WorldName = binding.WorldName;
	view.Target = &target;
	OverlayImage overlay;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(fixture.Render.PortalCaptureTreeReady(token));
	const auto *imported = fixture.Render.FindPortalCaptureTree(token);
	REQUIRE(imported);
	REQUIRE(imported->Nodes.size() == 2);
	CHECK(imported->Edges.front().Scale == .25f);
	CHECK(imported->MetadataBytes <= MAX_IMPORTED_PORTAL_TREE_METADATA_BYTES);
	CHECK(imported->Nodes[0].Producer.World != imported->Nodes[1].Producer.World);
	CHECK(imported->Nodes[0].Images[0] != imported->Nodes[1].Images[0]);
	CHECK(imported->Nodes[0].Images[3] == 0);
	CHECK(imported->Nodes[1].Images[3] != 0);
	CHECK(imported->Nodes[1].Binding.WorldName == binding.WorldName);
	CHECK(imported->Nodes[1].Camera.Position[0] == 1);
	CHECK(imported->Nodes[0].Lenses.Programs.empty());
	CHECK(imported->Nodes[1].Lenses.Programs.empty());
	const auto programToken = imported->Nodes[0].LensPrograms;
	REQUIRE(programToken != 0);
	CHECK(imported->Nodes[1].LensPrograms == programToken);
	const auto usage = fixture.Render.PortalImageUsage();
	CHECK(usage.PendingCpuBytes == 0);
	for (size_t physical : {size_t(0), size_t(1)}) {
		CAPTURE(physical);
		auto sparse = ImportTree(program, 1);
		auto &layers = sparse.Nodes[0].Layers;
		layers.Transparent.resize(physical);
		layers.SpatialOverlay = layers.Opaque;
		layers.SpatialOverlay->Depth.clear();
		layers.SpatialOverlay->Normal.clear();
		layers.SpatialOverlay->AmbientResponse.clear();
		layers.SpatialOverlay->LightingBaseline.clear();
		layers.SpatialOverlay->DirectionalResponse.clear();
		layers.SpatialOverlay->DepthHash = {};
		layers.SpatialOverlay->NormalHash = {};
		layers.SpatialOverlay->AmbientResponseHash = {};
		layers.SpatialOverlay->LightingBaselineHash = {};
		layers.SpatialOverlay->DirectionalResponseHash = {};
		REQUIRE(ValidPortalCaptureTree(sparse));
		const auto sparseToken = fixture.Render.QueuePortalCaptureTree(binding, std::move(sparse));
		REQUIRE(sparseToken != 0);
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		const auto *sparseImport = fixture.Render.FindPortalCaptureTree(sparseToken);
		REQUIRE(sparseImport);
		const auto &images = sparseImport->Nodes[0].Images;
		CHECK(images[0] != 0);
		CHECK((images[1] != 0) == (physical == 1));
		CHECK(images[2] == 0);
		CHECK(images[3] != 0);
		CHECK(fixture.Render.DropPortalImage(images[3]));
		CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(sparseToken));
		CHECK(fixture.Render.PortalImageUsage().Images == usage.Images);
		CHECK(fixture.Render.PortalCaptureTreeReady(token));
	}

	// The parent group was queued before this child's malformed program is refused.
	auto malformed = ImportTree(program);
	auto &badLenses = malformed.Nodes[1].Layers.Lenses;
	REQUIRE(badLenses.Programs[0].SpirV.size() > 5);
	badLenses.Programs[0].SpirV[5] = 0;
	badLenses.Programs[0].Hash = assets::Hasher::Of(std::as_bytes(std::span(badLenses.Programs[0].SpirV)));
	badLenses.Entries[0].ProgramHash = badLenses.Programs[0].Hash;
	REQUIRE(ValidPortalCaptureTree(malformed));
	CHECK(fixture.Render.QueuePortalCaptureTree(binding, std::move(malformed)) == 0);
	CHECK(fixture.Render.PortalImageUsage().Images == usage.Images);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == usage.TextureBytes);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == usage.PendingCpuBytes);
	CHECK(fixture.Render.FindPortalCaptureTree(token) == imported);
	CHECK(fixture.Render.RetainPortalLensPrograms(lenses) == programToken);
	fixture.Render.ReleasePortalLensPrograms(programToken);

	std::vector<uint64_t> fillers;
	while (fixture.Render.PortalImageUsage().Images + usage.Images <= MAX_IMPORTED_PORTAL_IMAGES) {
		auto filler = ImportTree(program, 1).Nodes.front().Layers.Opaque;
		auto owner = binding;
		owner.Portal = core::Name("capacity-filler-" + std::to_string(fillers.size()));
		filler.Key.PortalKey = owner.Portal.Text();
		owner.Expected = filler.Key;
		const auto handle = fixture.Render.QueuePortalImage(owner, std::move(filler));
		REQUIRE(handle != 0);
		fillers.push_back(handle);
	}
	const auto saturated = fixture.Render.PortalImageUsage();
	CHECK(fixture.Render.QueuePortalCaptureTree(binding, ImportTree(program)) == 0);
	CHECK(fixture.Render.PortalImageUsage().Images == saturated.Images);
	CHECK(fixture.Render.PortalCaptureTreeReady(token));
	for (const auto handle : fillers)
		CHECK(fixture.Render.DropPortalImage(handle));
	CHECK(fixture.Render.PortalImageUsage().Images == usage.Images);
	auto wrongBinding = binding;
	wrongBinding.Expected.CameraRevision++;
	CHECK(fixture.Render.QueuePortalCaptureTree(wrongBinding, ImportTree(program)) == 0);
	CHECK(fixture.Render.PortalCaptureTreeReady(token));

	const auto replacement = fixture.Render.QueuePortalCaptureTree(binding, ImportTree(program));
	REQUIRE(replacement != 0);
	CHECK(replacement != token);
	CHECK(fixture.Render.PortalCaptureTreeReady(token));
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(replacement));
	// Retiring any child layer invalidates its whole tree without touching a newer tree.
	CHECK(fixture.Render.DropPortalImage(imported->Nodes[1].Images[1]));
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(token));
	CHECK(fixture.Render.FindPortalCaptureTree(token) == nullptr);
	CHECK(fixture.Render.PortalImageUsage().Images == 7);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(fixture.Render.PortalCaptureTreeReady(replacement));
	fixture.Render.ForgetWorld(binding.World, core::Name("other-consumer"));
	CHECK(fixture.Render.PortalCaptureTreeReady(replacement));
	fixture.Render.ForgetWorld(binding.World, binding.WorldName);
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(replacement));
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	CHECK(fixture.Render.PortalImageUsage().PendingCpuBytes == 0);
	const auto freshProgram = fixture.Render.RetainPortalLensPrograms(lenses);
	REQUIRE(freshProgram != 0);
	CHECK(freshProgram != programToken);
	fixture.Render.ReleasePortalLensPrograms(freshProgram);
	const auto pending = fixture.Render.QueuePortalCaptureTree(binding, ImportTree(program));
	REQUIRE(pending != 0);
	fixture.Render.Shutdown();
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(pending));
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
}

TEST_CASE(
	"ordered capture replacement retains the complete old tree until publication",
	"[render][gpu][portal-tree-import][portal-tree-replacement-capacity][.]"
) {
	test::FixtureDevice fixture;
	fixture.Initialise();
	ShaderCompiler compiler;
	const auto compiled = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour; void main(){colour=vec4(1);}",
		ShaderStage::Fragment,
		"tree-replacement-lens"
	);
	INFO(compiled.Error);
	REQUIRE_FALSE(compiled.Failed);
	const PortalCaptureLensProgram program{
		assets::Hasher::Of(std::as_bytes(std::span(compiled.SpirV))), compiled.SpirV
	};
	const size_t nodes = GENERATE(size_t{3}, MAX_PORTAL_CAPTURE_TREE_NODES);
	CAPTURE(nodes);
	const bool maximum = nodes == MAX_PORTAL_CAPTURE_TREE_NODES;
	auto original = ImportTree(program, nodes, maximum);
	for (auto &node : original.Nodes) {
		if (maximum) {
			node.Layers.SpatialOverlay = node.Layers.Opaque;
			node.Layers.SpatialOverlay->Depth.clear();
			node.Layers.SpatialOverlay->Normal.clear();
			node.Layers.SpatialOverlay->AmbientResponse.clear();
			node.Layers.SpatialOverlay->LightingBaseline.clear();
			node.Layers.SpatialOverlay->DirectionalResponse.clear();
			node.Layers.SpatialOverlay->DepthHash = {};
			node.Layers.SpatialOverlay->NormalHash = {};
			node.Layers.SpatialOverlay->AmbientResponseHash = {};
			node.Layers.SpatialOverlay->LightingBaselineHash = {};
			node.Layers.SpatialOverlay->DirectionalResponseHash = {};
		} else
			node.Layers.SpatialOverlay.reset();
	}
	const size_t images = nodes * (maximum ? 4 : 3);
	const auto scratchReply = original.Nodes.front().Layers.Opaque;
	REQUIRE(ValidPortalCaptureTree(original));
	auto replacement = original;
	for (auto &node : replacement.Nodes) {
		node.Layers.Opaque.Key.CameraRevision++;
		if (node.Layers.SpatialOverlay) node.Layers.SpatialOverlay->Key.CameraRevision++;
		for (auto &layer : node.Layers.Transparent)
			layer.Key.CameraRevision++;
	}
	REQUIRE(ValidPortalCaptureTree(replacement));
	PortalImageBinding binding;
	binding.World = 17;
	binding.WorldName = core::Name("tree-replacement-consumer");
	binding.Portal = core::Name("door");
	binding.Expected = original.Nodes.front().Layers.Opaque.Key;
	binding.ExpectedScope = PortalImageScope::OpaqueLighting;
	binding.ExpectedProjection = PortalImageProjection::Eye;
	const auto oldToken = fixture.Render.QueuePortalCaptureTree(binding, std::move(original));
	REQUIRE(oldToken != 0);
	SceneTarget target{17, 17};
	View view;
	view.World = binding.World;
	view.WorldName = binding.WorldName;
	view.Target = &target;
	OverlayImage overlay;
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(fixture.Render.PortalCaptureTreeReady(oldToken));
	const auto oldUsage = fixture.Render.PortalImageUsage();
	REQUIRE(oldUsage.Images == images);
	REQUIRE(oldUsage.PendingCpuBytes == 0);
	REQUIRE(oldUsage.TextureBytes * 2 < MAX_IMPORTED_PORTAL_TEXTURE_BYTES);
	const auto *oldTree = fixture.Render.FindPortalCaptureTree(oldToken);
	REQUIRE(oldTree);
	const auto oldRoot = oldTree->Nodes.front().Images[0];
	binding.Expected = replacement.Nodes.front().Layers.Opaque.Key;
	const auto newToken = fixture.Render.QueuePortalCaptureTree(binding, std::move(replacement));
	REQUIRE(newToken != 0);
	CHECK(newToken != oldToken);
	CHECK(fixture.Render.PortalCaptureTreeReady(oldToken));
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(newToken));
	CHECK(fixture.Render.PortalImageUsage().Images == images * 2);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == oldUsage.TextureBytes * 2);
	fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(fixture.Render.PortalCaptureTreeReady(newToken));
	REQUIRE(fixture.Render.PortalCaptureTreeReady(oldToken));
	const auto *newTree = fixture.Render.FindPortalCaptureTree(newToken);
	REQUIRE(newTree);
	CHECK(newTree->Nodes.front().Images[0] != oldRoot);
	CHECK(newTree->Nodes.front().Binding.Expected == binding.Expected);
	CHECK(oldTree->Nodes.front().Binding.Expected.CameraRevision + 1 == binding.Expected.CameraRevision);
	if (maximum) {
		std::vector<uint64_t> scratch;
		for (size_t index = 0; index <= nodes; ++index) {
			auto reply = scratchReply;
			auto owner = binding;
			owner.Portal = core::Name("tree-composition-scratch-" + std::to_string(index));
			reply.Key.PortalKey = owner.Portal.Text();
			owner.Expected = reply.Key;
			const auto handle = fixture.Render.QueuePortalImage(owner, std::move(reply));
			REQUIRE(handle != 0);
			scratch.push_back(handle);
		}
		CHECK(fixture.Render.PortalImageUsage().Images == MAX_IMPORTED_PORTAL_IMAGES);
		auto overflow = ImportTree(program);
		auto overflowBinding = binding;
		overflowBinding.Expected = overflow.Nodes.front().Layers.Opaque.Key;
		CHECK(fixture.Render.QueuePortalCaptureTree(overflowBinding, std::move(overflow)) == 0);
		CHECK(fixture.Render.PortalCaptureTreeReady(oldToken));
		CHECK(fixture.Render.PortalCaptureTreeReady(newToken));
		for (const auto handle : scratch)
			CHECK(fixture.Render.DropPortalImage(handle));
	}
	fixture.Render.DropPortalCaptureTree(oldToken);
	CHECK_FALSE(fixture.Render.PortalCaptureTreeReady(oldToken));
	CHECK(fixture.Render.PortalCaptureTreeReady(newToken));
	CHECK(fixture.Render.PortalImageUsage().Images == oldUsage.Images);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == oldUsage.TextureBytes);
	fixture.Render.DropPortalCaptureTree(newToken);
	CHECK(fixture.Render.PortalImageUsage().Images == 0);
	CHECK(fixture.Render.PortalImageUsage().TextureBytes == 0);
}

TEST_CASE(
	"capture tree leases survive owner release and obey hard retirement",
	"[render][gpu][portal-tree-import][.]"
) {
	const int retirement = GENERATE(0, 1, 2);
	test::FixtureDevice fixture;
	fixture.Initialise();
	auto &renderer = fixture.Render;
	ShaderCompiler compiler;
	const auto compiled = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour; void main(){colour=vec4(1);}",
		ShaderStage::Fragment,
		"leased-tree-lens"
	);
	REQUIRE_FALSE(compiled.Failed);
	const PortalCaptureLensProgram program{
		assets::Hasher::Of(std::as_bytes(std::span(compiled.SpirV))), compiled.SpirV
	};
	const auto queue = [&](uint64_t revision) {
		auto tree = ImportTree(program, 1);
		tree.Nodes.front().Layers.Lenses = {};
		tree.Nodes.front().Layers.Opaque.Key.CameraRevision = revision;
		for (auto &layer : tree.Nodes.front().Layers.Transparent)
			layer.Key = tree.Nodes.front().Layers.Opaque.Key;
		PortalImageBinding binding;
		binding.World = 7;
		binding.WorldName = core::Name("consumer");
		binding.Portal = core::Name("door");
		binding.Expected = tree.Nodes.front().Layers.Opaque.Key;
		binding.ExpectedScope = PortalImageScope::OpaqueLighting;
		binding.ExpectedProjection = PortalImageProjection::Eye;
		const auto token = renderer.QueuePortalCaptureTree(binding, std::move(tree));
		REQUIRE(token != 0);
		CHECK(renderer.AcquirePortalCaptureTreeLease(token) == 0);
		SceneTarget target{17, 17};
		View view;
		view.World = binding.World;
		view.WorldName = binding.WorldName;
		view.Target = &target;
		OverlayImage overlay;
		renderer.Render(std::span(&view, 1), overlay, nullptr, false);
		REQUIRE(renderer.PortalCaptureTreeReady(token));
		return token;
	};
	const auto oldTree = queue(1);
	const auto oldImage = renderer.FindPortalCaptureTree(oldTree)->Nodes.front().Images[0];
	const auto oldPreviewLease = renderer.AcquirePortalCaptureTreeLease(oldTree);
	REQUIRE(oldPreviewLease != 0);
	SceneTarget target{17, 17};
	View referenceBody;
	const auto &referenceBinding = renderer.FindPortalCaptureTree(oldTree)->Nodes.front().Binding;
	referenceBody.World = referenceBinding.World;
	referenceBody.WorldName = referenceBinding.WorldName;
	referenceBody.Slot = referenceBinding.ViewSlot;
	referenceBody.Target = &target;
	uint64_t oldPreparation = 0;
	REQUIRE(
		renderer.BeginPortalCaptureTreePreparation(oldTree, referenceBody, oldPreparation) ==
		PortalTreeCompositionStatus::Pending
	);
	REQUIRE(oldPreparation != 0);
	const auto usage = renderer.PortalImageUsage();
	renderer.ReleasePortalCaptureTree(oldTree);
	renderer.ReleasePortalCaptureTree(oldTree);
	REQUIRE(renderer.PortalCaptureTreeReady(oldTree));
	// A released preview cannot gain a new owner, but its admitted preparation remains live.
	CHECK(renderer.AcquirePortalCaptureTreeLease(oldTree) == 0);
	CHECK(renderer.PortalImageUsage().Images == usage.Images);
	CHECK(renderer.PortalImageUsage().TextureBytes == usage.TextureBytes);
	const auto replacement = queue(2);
	const auto replacementPreviewLease = renderer.AcquirePortalCaptureTreeLease(replacement);
	REQUIRE(replacementPreviewLease != 0);
	uint64_t replacementPreparation = 0;
	REQUIRE(
		renderer.BeginPortalCaptureTreePreparation(replacement, referenceBody, replacementPreparation) ==
		PortalTreeCompositionStatus::Pending
	);
	REQUIRE(replacementPreparation != 0);
	// Two resident trees own a preview and preparation lease each. A fifth owner is refused.
	CHECK(renderer.AcquirePortalCaptureTreeLease(replacement) == 0);
	CHECK(renderer.AcquirePortalCaptureTreeLease(oldTree) == 0);
	CHECK(renderer.FindPortalCaptureTree(oldTree)->Nodes.front().Binding.Expected.CameraRevision == 1);
	CHECK(renderer.FindPortalCaptureTree(replacement)->Nodes.front().Binding.Expected.CameraRevision == 2);
	if (retirement == 0) {
		renderer.ReleasePortalCaptureTreeLease(oldPreviewLease);
		renderer.CancelPortalCaptureTreePreparation(oldPreparation);
	} else if (retirement == 1)
		renderer.DropPortalCaptureTree(oldTree);
	else
		CHECK(renderer.DropPortalImage(oldImage));
	CHECK_FALSE(renderer.PortalCaptureTreeReady(oldTree));
	REQUIRE(renderer.PortalCaptureTreeReady(replacement));
	const auto third = renderer.AcquirePortalCaptureTreeLease(replacement);
	REQUIRE(third != 0);
	CHECK(third != oldPreviewLease);
	renderer.ReleasePortalCaptureTreeLease(third);
	renderer.ReleasePortalCaptureTreeLease(replacementPreviewLease);
	renderer.CancelPortalCaptureTreePreparation(replacementPreparation);
	CHECK(renderer.PortalCaptureTreeReady(replacement));
	const auto worldLease = renderer.AcquirePortalCaptureTreeLease(replacement);
	REQUIRE(worldLease != 0);
	renderer.ReleasePortalCaptureTree(replacement);
	renderer.ForgetWorld(7, core::Name("consumer"));
	CHECK_FALSE(renderer.PortalCaptureTreeReady(replacement));
	renderer.ReleasePortalCaptureTreeLease(worldLease);
	CHECK(renderer.PortalImageUsage().Images == 0);
	CHECK(renderer.PortalImageUsage().PendingCpuBytes == 0);
}
