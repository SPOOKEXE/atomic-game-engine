#pragma once
#include <engine/render/PortalExchange.hpp>

#include <functional>
namespace engine::render {
	// Maximum nodes admitted in one recursively captured portal tree.
	inline constexpr size_t MAX_PORTAL_CAPTURE_TREE_NODES = 16;
	// Maximum portal crossings from the root capture.
	inline constexpr size_t MAX_PORTAL_CAPTURE_TREE_DEPTH = 4;
	// A borrowed presentation endpoint identity used while admitting a tree.
	struct PortalCaptureTreeEndpointView {
		// World named by the endpoint.
		std::string_view World;
		// Presentation channel named by the endpoint.
		std::string_view Channel;
		// Host-issued endpoint session.
		uint64_t Session = 0;
		// Endpoint incarnation within Session.
		uint64_t Generation = 0;
		// Compares endpoint identities.
		bool operator==(const PortalCaptureTreeEndpointView &) const = default;
	};
	// Host policy for admitting one child producer endpoint.
	using PortalCaptureTreeAllowChild = std::function<bool(PortalCaptureTreeEndpointView)>;
	// An owned endpoint identity stored in a completed tree.
	struct PortalCaptureTreeEndpoint {
		// Owned world name.
		std::string World;
		// Owned presentation channel name.
		std::string Channel;
		// Host-issued endpoint session.
		uint64_t Session = 0;
		// Endpoint incarnation within Session.
		uint64_t Generation = 0;
		// Compares complete endpoint identities.
		bool operator==(const PortalCaptureTreeEndpoint &) const = default;
	};
	// The camera used to render one node of a capture tree.
	struct PortalCaptureTreeCamera {
		// Camera position in the producer world.
		std::array<float, 3> Position{};
		// Unit XYZW camera orientation.
		std::array<float, 4> Orientation{0, 0, 0, 1};
		// Off-axis frustum bounds at near and far planes.
		std::array<float, 6> Frustum{-1, 1, -1, 1, 1, 1000};
		// World-space clipping plane.
		std::array<float, 4> ClipPlane{};
		// Whether this camera crossed an eye or seam projection.
		PortalImageProjection Projection = PortalImageProjection::Eye;
		// Compares the exact camera inputs recorded in a tree node.
		bool operator==(const PortalCaptureTreeCamera &) const = default;
	};
	// Borrowed limits and root identity used before decoding a candidate tree.
	struct PortalCaptureTreeAdmission {
		// Correlation key the root tree must match.
		const PortalExchangeKey &Key;
		// Root image width in pixels.
		uint32_t Width = 0;
		// Root image height in pixels.
		uint32_t Height = 0;
		// Root capture camera.
		PortalCaptureTreeCamera Camera;
		// Authenticated root producer endpoint.
		PortalCaptureTreeEndpointView Root;
		// Maximum crossings admitted below Root.
		uint32_t MaxDepth = 0;
		// Aggregate image pixels admitted for the tree.
		uint32_t PixelBudget = 0;
		// Authorized body excluded from the root view.
		std::string_view RetainedBodyPlayer{};
	};
	// One captured producer view and its composited image layers.
	struct PortalCaptureTreeNode {
		// Endpoint that rendered this node.
		PortalCaptureTreeEndpoint Producer;
		// Camera used by the producer.
		PortalCaptureTreeCamera Camera;
		// Opaque, transparent, and spatial image layers.
		PortalImageLayerSet Layers;
		// Authorized body retained by this node.
		std::string RetainedBodyPlayer{};
		// Compares every captured node input and layer.
		bool operator==(const PortalCaptureTreeNode &) const = default;
	};
	// A portal transform from one capture-tree node to a child.
	struct PortalCaptureTreeEdge {
		// Parent node index in PortalCaptureTree::Nodes.
		uint8_t Parent = 0;
		// Child node index in PortalCaptureTree::Nodes.
		uint8_t Child = 1;
		// Stable key of the authored portal.
		std::string PortalKey;
		// Portal aperture centre in the parent frame.
		std::array<float, 3> Centre{};
		// First aperture basis vector in the parent frame.
		std::array<float, 3> First{1, 0, 0};
		// Second aperture basis vector in the parent frame.
		std::array<float, 3> Second{0, 1, 0};
		// Child point = Position + rotate(parent point, Orientation) * Scale.
		std::array<float, 3> Position{};
		// Unit XYZW rotation from parent to child coordinates.
		std::array<float, 4> Orientation{0, 0, 0, 1};
		// Similarity scale from parent to child coordinates.
		float Scale = 1;
		// Encoded PortalGeometry in the parent frame; no renderer surface index crosses.
		std::vector<std::byte> Geometry;
		// Compares complete portal crossings.
		bool operator==(const PortalCaptureTreeEdge &) const = default;
	};
	// A bounded nested portal capture with node-local images and cameras.
	struct PortalCaptureTree {
		// Captures indexed by every tree edge.
		std::vector<PortalCaptureTreeNode> Nodes;
		// Parent-to-child portal crossings.
		std::vector<PortalCaptureTreeEdge> Edges;
		// Compares nodes and crossings in their recorded order.
		bool operator==(const PortalCaptureTree &) const = default;
	};
	// Decoded resource totals used to enforce capture-tree limits.
	struct PortalCaptureTreeMeasure {
		// Decoded capture-tree nodes.
		size_t Nodes = 0;
		// Image payloads represented by the tree.
		size_t Images = 0;
		// Aggregate pixels across image payloads.
		size_t Pixels = 0;
		// Bytes used by depth planes.
		size_t DepthBytes = 0;
		// Bytes used by ambient response planes.
		size_t AmbientBytes = 0;
		// Bytes used by identifiers and structural metadata.
		size_t MetadataBytes = 0;
		// Tick at which the tree root was captured.
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
	// Checks tree topology, endpoint identity, and declared capture limits.
	bool ValidPortalCaptureTree(const PortalCaptureTree &tree);
	// Measures a wire tree without retaining decoded images.
	bool MeasurePortalCaptureTree(std::span<const std::byte>, PortalCaptureTreeMeasure &, std::string &error);
	// Encodes a validated tree for a presentation message.
	bool EncodePortalCaptureTree(const PortalCaptureTree &, std::vector<std::byte> &, std::string &error);
	// Decodes and validates a bounded tree from a presentation message.
	bool DecodePortalCaptureTree(std::span<const std::byte>, PortalCaptureTree &, std::string &error);
}
