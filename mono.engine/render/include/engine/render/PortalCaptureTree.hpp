#pragma once
#include <engine/render/PortalExchange.hpp>

#include <functional>
namespace engine::render {
	inline constexpr size_t MAX_PORTAL_CAPTURE_TREE_NODES = 16;
	inline constexpr size_t MAX_PORTAL_CAPTURE_TREE_DEPTH = 4;
	struct PortalCaptureTreeEndpointView {
		std::string_view World, Channel;
		uint64_t Session = 0, Generation = 0;
		bool operator==(const PortalCaptureTreeEndpointView &) const = default;
	};
	using PortalCaptureTreeAllowChild = std::function<bool(PortalCaptureTreeEndpointView)>;
	struct PortalCaptureTreeEndpoint {
		std::string World, Channel;
		uint64_t Session = 0, Generation = 0;
		bool operator==(const PortalCaptureTreeEndpoint &) const = default;
	};
	struct PortalCaptureTreeCamera {
		std::array<float, 3> Position{};
		std::array<float, 4> Orientation{0, 0, 0, 1};
		std::array<float, 6> Frustum{-1, 1, -1, 1, 1, 1000};
		std::array<float, 4> ClipPlane{};
		PortalImageProjection Projection = PortalImageProjection::Eye;
		bool operator==(const PortalCaptureTreeCamera &) const = default;
	};
	struct PortalCaptureTreeAdmission {
		const PortalExchangeKey &Key;
		uint32_t Width = 0, Height = 0;
		PortalCaptureTreeCamera Camera;
		PortalCaptureTreeEndpointView Root;
		uint32_t MaxDepth = 0, PixelBudget = 0;
		std::string_view RetainedBodyPlayer{};
	};
	struct PortalCaptureTreeNode {
		PortalCaptureTreeEndpoint Producer;
		PortalCaptureTreeCamera Camera;
		PortalImageLayerSet Layers;
		std::string RetainedBodyPlayer{};
		bool operator==(const PortalCaptureTreeNode &) const = default;
	};
	struct PortalCaptureTreeEdge {
		uint8_t Parent = 0, Child = 1;
		std::string PortalKey;
		std::array<float, 3> Centre{}, First{1, 0, 0}, Second{0, 1, 0};
		// Child point = Position + rotate(parent point, Orientation) * Scale.
		std::array<float, 3> Position{};
		std::array<float, 4> Orientation{0, 0, 0, 1};
		float Scale = 1;
		// Encoded PortalGeometry in the parent frame; no renderer surface index crosses.
		std::vector<std::byte> Geometry;
		bool operator==(const PortalCaptureTreeEdge &) const = default;
	};
	struct PortalCaptureTree {
		std::vector<PortalCaptureTreeNode> Nodes;
		std::vector<PortalCaptureTreeEdge> Edges;
		bool operator==(const PortalCaptureTree &) const = default;
	};
	struct PortalCaptureTreeMeasure {
		size_t Nodes = 0, Images = 0, Pixels = 0, DepthBytes = 0, AmbientBytes = 0, MetadataBytes = 0;
		uint64_t CaptureTick = 0;
	};
	// Admission only: borrows all records before any image decompression. Endpoints
	// remain claims until the presentation adapter authenticates their provenance.
	bool MatchPortalCaptureTree(
		std::span<const std::byte>,
		const PortalCaptureTreeAdmission &,
		const PortalCaptureTreeAllowChild &,
		PortalCaptureTreeMeasure &,
		std::string &error
	);
	bool ValidPortalCaptureTree(const PortalCaptureTree &tree);
	bool MeasurePortalCaptureTree(std::span<const std::byte>, PortalCaptureTreeMeasure &, std::string &error);
	bool EncodePortalCaptureTree(const PortalCaptureTree &, std::vector<std::byte> &, std::string &error);
	bool DecodePortalCaptureTree(std::span<const std::byte>, PortalCaptureTree &, std::string &error);
}
