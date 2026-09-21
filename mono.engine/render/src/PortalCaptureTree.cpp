#include "PortalLayerPreflight.hpp"
#include "PortalPlayerIdentity.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalGeometry.hpp>

#include <cmath>
#include <utility>
namespace engine::render {
	namespace {
		constexpr uint32_t CAPTURE_TREE_MAGIC = 0x474d4950;
		constexpr uint16_t CAPTURE_TREE_VERSION = 19;
		constexpr uint8_t KIND = 6;
		bool Fail(std::string &error) {
			error = "invalid or over-budget portal capture tree";
			return false;
		}
		bool CaptureTreeText(std::string_view value) {
			return !value.empty() && value.size() <= 256 && value.find('\0') == std::string_view::npos;
		}
		template <size_t N> bool CaptureTreeFinite(const std::array<float, N> &a) {
			for (auto v : a)
				if (!std::isfinite(v)) return false;
			return true;
		}
		template <size_t N> void Read(core::ByteReader &reader, std::array<float, N> &a) {
			for (auto &v : a)
				v = reader.ReadFloat();
		}
		template <size_t N> void Write(core::ByteWriter &writer, const std::array<float, N> &a) {
			for (auto v : a)
				writer.WriteFloat(v);
		}
		bool Unit(const std::array<float, 4> &a) {
			double norm = 0;
			for (auto v : a)
				norm += double(v) * v;
			return CaptureTreeFinite(a) && std::abs(norm - 1) <= .001;
		}
		bool Camera(const PortalCaptureTreeCamera &camera) {
			const auto &frustum = camera.Frustum;
			double normal = 0;
			for (size_t i = 0; i < 3; ++i)
				normal += double(camera.ClipPlane[i]) * camera.ClipPlane[i];
			return CaptureTreeFinite(camera.Position) && Unit(camera.Orientation) &&
				   CaptureTreeFinite(frustum) && frustum[0] < frustum[1] && frustum[2] < frustum[3] &&
				   frustum[4] > 0 && frustum[5] > frustum[4] && CaptureTreeFinite(camera.ClipPlane) &&
				   ((camera.Projection == PortalImageProjection::Eye &&
					 camera.ClipPlane == std::array<float, 4>{}) ||
					(camera.Projection == PortalImageProjection::Seam && std::abs(normal - 1) <= .001));
		}
		bool Parse(
			std::span<const std::byte> bytes,
			PortalCaptureTreeMeasure &measure,
			PortalCaptureTree *out,
			std::string &error,
			const PortalCaptureTreeAdmission *admission = nullptr,
			const PortalCaptureTreeAllowChild *allowChild = nullptr
		) {
			if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES) return Fail(error);
			core::ByteReader reader(bytes);
			if (reader.ReadUInt32() != CAPTURE_TREE_MAGIC || reader.ReadUInt16() != CAPTURE_TREE_VERSION ||
				reader.ReadUInt8() != KIND || reader.ReadUInt8() != 0)
				return Fail(error);
			const auto nodes = reader.ReadUInt8(), edges = reader.ReadUInt8();
			if (nodes == 0 || nodes > MAX_PORTAL_CAPTURE_TREE_NODES || size_t(edges) + 1 != nodes)
				return Fail(error);
			PortalCaptureTreeMeasure measured;
			measured.Nodes = nodes;
			measured.MetadataBytes = sizeof(PortalCaptureTree) + nodes * sizeof(PortalCaptureTreeNode) +
									 edges * sizeof(PortalCaptureTreeEdge);
			size_t programBytes = 0, programs = 0, geometryBytes = 0, geometryRows = 0, geometryJoints = 0;
			std::string_view rootRetainedBody;
			std::array<assets::ContentHash, MAX_PORTAL_CAPTURE_LENSES> hashes{};
			if (out) {
				out->Nodes.resize(nodes);
				out->Edges.resize(edges);
			}
			for (size_t i = 0; i < nodes; ++i) {
				const auto world = reader.ReadString(), channel = reader.ReadString();
				const auto session = reader.ReadUInt64(), generation = reader.ReadUInt64();
				PortalCaptureTreeCamera camera;
				Read(reader, camera.Position);
				Read(reader, camera.Orientation);
				Read(reader, camera.Frustum);
				Read(reader, camera.ClipPlane);
				camera.Projection = static_cast<PortalImageProjection>(reader.ReadUInt8());
				const auto retainedBody = reader.ReadString();
				if (!ValidPortalPlayerIdentity(retainedBody)) return Fail(error);
				if (i == 0)
					rootRetainedBody = retainedBody;
				else if (retainedBody != rootRetainedBody)
					return Fail(error);
				const auto length = reader.ReadUInt32();
				const auto layer = reader.ReadRawView(length);
				PortalLayerMeasure layerMeasure;
				if (reader.Failed() || !CaptureTreeText(world) || !CaptureTreeText(channel) || session == 0 ||
					generation == 0 || !Camera(camera) || !PreflightPortalLayers(layer, layerMeasure))
					return Fail(error);
				if (admission) {
					const PortalCaptureTreeEndpointView endpoint{world, channel, session, generation};
					if (i == 0) {
						const auto &key = admission->Key;
						if (endpoint != admission->Root || camera != admission->Camera ||
							retainedBody != admission->RetainedBodyPlayer ||
							layerMeasure.Width != admission->Width ||
							layerMeasure.Height != admission->Height ||
							layerMeasure.RequestId != key.RequestId ||
							layerMeasure.PortalKey != key.PortalKey ||
							layerMeasure.CameraRevision != key.CameraRevision ||
							layerMeasure.SeamRevision != key.SeamRevision)
							return Fail(error);
					} else if (!allowChild || !*allowChild || !(*allowChild)(endpoint))
						return Fail(error);
				}
				const size_t pixels =
					size_t(layerMeasure.Width) * layerMeasure.Height * layerMeasure.Match.ImageCount;
				if (pixels > MAX_PORTAL_IMAGE_PIXELS - measured.Pixels) return Fail(error);
				measured.Pixels += pixels;
				if (i == 0) measured.CaptureTick = layerMeasure.Match.CaptureTick;
				measured.Images += layerMeasure.Match.ImageCount;
				measured.DepthBytes += layerMeasure.Match.DepthBytes;
				measured.AmbientBytes += layerMeasure.Match.AmbientBytes;
				measured.MetadataBytes += world.size() + channel.size() + retainedBody.size() +
										  layerMeasure.Match.MetadataBytes +
										  layerMeasure.Match.DiagnosticBytes +
										  layerMeasure.Match.ImageCount * sizeof(PortalImageReply);
				programBytes += layerMeasure.ProgramBytes;
				for (size_t program = 0; program < layerMeasure.ProgramCount; ++program) {
					bool known = false;
					for (size_t h = 0; h < programs; ++h)
						known |= hashes[h] == layerMeasure.ProgramHashes[program];
					if (!known) {
						if (programs == hashes.size()) return Fail(error);
						hashes[programs++] = layerMeasure.ProgramHashes[program];
					}
				}
				if (programBytes > MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES ||
					programs > MAX_PORTAL_CAPTURE_LENSES)
					return Fail(error);
				if (out) {
					auto &node = out->Nodes[i];
					node.Producer = {std::string(world), std::string(channel), session, generation};
					node.Camera = camera;
					node.RetainedBodyPlayer = retainedBody;
					if (!DecodePortalImageLayerSet(layer, node.Layers, error)) return false;
				}
			}
			std::array<uint8_t, MAX_PORTAL_CAPTURE_TREE_NODES> parents{}, depth{};
			std::array<uint8_t, MAX_PORTAL_CAPTURE_TREE_NODES> parentOf{};
			std::array<std::string_view, MAX_PORTAL_CAPTURE_TREE_NODES> edgeKeys{};
			for (size_t i = 0; i < edges; ++i) {
				PortalCaptureTreeEdge edge;
				edge.Parent = reader.ReadUInt8();
				edge.Child = reader.ReadUInt8();
				const auto key = reader.ReadString();
				Read(reader, edge.Centre);
				Read(reader, edge.First);
				Read(reader, edge.Second);
				Read(reader, edge.Position);
				Read(reader, edge.Orientation);
				edge.Scale = reader.ReadFloat();
				const auto length = reader.ReadUInt32();
				const auto geometry = reader.ReadRawView(length);
				if (reader.Failed() || edge.Parent >= edge.Child || edge.Child >= nodes ||
					parents[edge.Child]++ != 0 || !CaptureTreeText(key) || !CaptureTreeFinite(edge.Centre) ||
					!CaptureTreeFinite(edge.First) || !CaptureTreeFinite(edge.Second) ||
					!CaptureTreeFinite(edge.Position) || !Unit(edge.Orientation) ||
					!std::isfinite(edge.Scale) || edge.Scale <= 0)
					return Fail(error);
				double area = 0;
				for (size_t axis = 0; axis < 3; ++axis) {
					const size_t a = (axis + 1) % 3, b = (axis + 2) % 3;
					const double cross =
						double(edge.First[a]) * edge.Second[b] - double(edge.First[b]) * edge.Second[a];
					area += cross * cross;
				}
				if (area <= 0 || !std::isfinite(area)) return Fail(error);
				for (size_t previous = 1; previous < nodes; ++previous)
					if (!edgeKeys[previous].empty() && parentOf[previous] == edge.Parent &&
						edgeKeys[previous] == key)
						return Fail(error);
				edgeKeys[edge.Child] = key;

				parentOf[edge.Child] = edge.Parent;
				PortalGeometryMeasure gm;
				if (!MeasurePortalGeometry(geometry, gm, error)) return false;
				geometryBytes += geometry.size();
				geometryRows += gm.Rows;
				geometryJoints += gm.Joints;
				if (geometryBytes > MAX_PORTAL_GEOMETRY_BYTES || geometryRows > MAX_PORTAL_GEOMETRY_ROWS ||
					geometryJoints > MAX_PORTAL_GEOMETRY_JOINTS)
					return Fail(error);
				measured.MetadataBytes += key.size() + geometry.size() + gm.MetadataBytes;
				if (out) {
					edge.PortalKey = key;
					edge.Geometry.assign(geometry.begin(), geometry.end());
					out->Edges[i] = std::move(edge);
				}
			}
			for (size_t i = 1; i < nodes; ++i) {
				if (parents[i] != 1) return Fail(error);
				depth[i] = depth[parentOf[i]] + 1;
				if (depth[i] > MAX_PORTAL_CAPTURE_TREE_DEPTH) return Fail(error);
			}
			if (!reader.AtEnd() || reader.Failed()) return Fail(error);
			if (admission) {
				if (measured.Pixels > admission->PixelBudget) return Fail(error);
				for (size_t i = 1; i < nodes; ++i)
					if (depth[i] > admission->MaxDepth) return Fail(error);
			}
			measure = measured;
			error.clear();
			return true;
		}
	}
	bool MatchPortalCaptureTree(
		std::span<const std::byte> bytes,
		const PortalCaptureTreeAdmission &admission,
		const PortalCaptureTreeAllowChild &allowChild,
		PortalCaptureTreeMeasure &out,
		std::string &error
	) {
		if (admission.MaxDepth > MAX_PORTAL_CAPTURE_TREE_DEPTH ||
			admission.PixelBudget > MAX_PORTAL_IMAGE_PIXELS)
			return Fail(error);
		PortalCaptureTreeMeasure measured;
		if (!Parse(bytes, measured, nullptr, error, &admission, &allowChild)) return false;
		out = measured;
		return true;
	}
	bool MeasurePortalCaptureTree(
		std::span<const std::byte> bytes, PortalCaptureTreeMeasure &out, std::string &error
	) {
		PortalCaptureTreeMeasure measure;
		if (!Parse(bytes, measure, nullptr, error)) return false;
		out = measure;
		return true;
	}
	bool
	DecodePortalCaptureTree(std::span<const std::byte> bytes, PortalCaptureTree &out, std::string &error) {
		PortalCaptureTreeMeasure measure;
		if (!MeasurePortalCaptureTree(bytes, measure, error)) return false;
		PortalCaptureTree tree;
		if (!Parse(bytes, measure, &tree, error)) return false;
		out = std::move(tree);
		return true;
	}
	bool ValidPortalCaptureTree(const PortalCaptureTree &tree) {
		if (tree.Nodes.empty() || tree.Nodes.size() > MAX_PORTAL_CAPTURE_TREE_NODES ||
			tree.Edges.size() + 1 != tree.Nodes.size())
			return false;
		size_t pixels = 0, code = 0, programs = 0, bytes = 0, rows = 0, joints = 0;
		std::array<assets::ContentHash, MAX_PORTAL_CAPTURE_LENSES> hashes{};
		for (const auto &node : tree.Nodes) {
			if (!CaptureTreeText(node.Producer.World) || !CaptureTreeText(node.Producer.Channel) ||
				!node.Producer.Session || !node.Producer.Generation || !Camera(node.Camera) ||
				!node.Layers.Opaque.CaptureLighting || !ValidPortalPlayerIdentity(node.RetainedBodyPlayer) ||
				node.RetainedBodyPlayer != tree.Nodes.front().RetainedBodyPlayer ||
				!ValidPortalImageLayerSet(node.Layers))
				return false;
			const auto &layers = node.Layers;
			pixels += size_t(layers.Opaque.Width) * layers.Opaque.Height *
					  (1 + layers.Transparent.size() + layers.SpatialOverlay.has_value());
			for (const auto &program : layers.Lenses.Programs) {
				code += program.SpirV.size() * 4;
				bool known = false;
				for (size_t h = 0; h < programs; ++h)
					known |= hashes[h] == program.Hash;
				if (!known) {
					if (programs == hashes.size()) return false;
					hashes[programs++] = program.Hash;
				}
			}
			if (pixels > MAX_PORTAL_IMAGE_PIXELS || code > MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES)
				return false;
		}
		std::array<uint8_t, MAX_PORTAL_CAPTURE_TREE_NODES> parents{}, parentOf{}, depth{};
		for (const auto &edge : tree.Edges) {
			if (edge.Parent >= edge.Child || edge.Child >= tree.Nodes.size() || parents[edge.Child]++ ||
				!CaptureTreeText(edge.PortalKey) || !CaptureTreeFinite(edge.Centre) ||
				!CaptureTreeFinite(edge.First) || !CaptureTreeFinite(edge.Second) ||
				!CaptureTreeFinite(edge.Position) || !Unit(edge.Orientation) || !std::isfinite(edge.Scale) ||
				edge.Scale <= 0)
				return false;
			double area = 0;
			for (size_t axis = 0; axis < 3; ++axis) {
				const size_t a = (axis + 1) % 3, b = (axis + 2) % 3;
				const double cross =
					double(edge.First[a]) * edge.Second[b] - double(edge.First[b]) * edge.Second[a];
				area += cross * cross;
			}
			if (area <= 0 || !std::isfinite(area)) return false;
			for (const auto &previous : tree.Edges) {
				if (&previous == &edge) break;
				if (previous.Parent == edge.Parent && previous.PortalKey == edge.PortalKey) return false;
			}
			parentOf[edge.Child] = edge.Parent;
			PortalGeometryMeasure measured;
			std::string error;
			if (!MeasurePortalGeometry(edge.Geometry, measured, error)) return false;
			bytes += edge.Geometry.size();
			rows += measured.Rows;
			joints += measured.Joints;
			if (bytes > MAX_PORTAL_GEOMETRY_BYTES || rows > MAX_PORTAL_GEOMETRY_ROWS ||
				joints > MAX_PORTAL_GEOMETRY_JOINTS)
				return false;
		}
		for (size_t i = 1; i < tree.Nodes.size(); ++i) {
			if (parents[i] != 1) return false;
			depth[i] = depth[parentOf[i]] + 1;
			if (depth[i] > MAX_PORTAL_CAPTURE_TREE_DEPTH) return false;
		}
		return true;
	}

	bool
	EncodePortalCaptureTree(const PortalCaptureTree &tree, std::vector<std::byte> &out, std::string &error) {
		if (!ValidPortalCaptureTree(tree)) return Fail(error);
		core::ByteWriter writer;
		writer.WriteUInt32(CAPTURE_TREE_MAGIC);
		writer.WriteUInt16(CAPTURE_TREE_VERSION);
		writer.WriteUInt8(KIND);
		writer.WriteUInt8(0);
		writer.WriteUInt8(tree.Nodes.size());
		writer.WriteUInt8(tree.Edges.size());
		for (const auto &node : tree.Nodes) {
			if (!CaptureTreeText(node.Producer.World) || !CaptureTreeText(node.Producer.Channel) ||
				!Camera(node.Camera))
				return Fail(error);
			writer.WriteString(node.Producer.World);
			writer.WriteString(node.Producer.Channel);
			writer.WriteUInt64(node.Producer.Session);
			writer.WriteUInt64(node.Producer.Generation);
			Write(writer, node.Camera.Position);
			Write(writer, node.Camera.Orientation);
			Write(writer, node.Camera.Frustum);
			Write(writer, node.Camera.ClipPlane);
			writer.WriteUInt8(static_cast<uint8_t>(node.Camera.Projection));
			writer.WriteString(node.RetainedBodyPlayer);
			std::vector<std::byte> layer;
			if (!EncodePortalImageLayerSet(node.Layers, layer, error)) return false;
			if (writer.Bytes().size() > MAX_PORTAL_EXCHANGE_BYTES ||
				layer.size() + 4 > MAX_PORTAL_EXCHANGE_BYTES - writer.Bytes().size())
				return Fail(error);
			writer.WriteUInt32(layer.size());
			writer.WriteRaw(layer.data(), layer.size());
		}
		for (const auto &edge : tree.Edges) {
			if (!CaptureTreeText(edge.PortalKey)) return Fail(error);
			const size_t edgeBytes =
				2 + 4 + edge.PortalKey.size() + 17 * sizeof(float) + 4 + edge.Geometry.size();
			if (writer.Bytes().size() > MAX_PORTAL_EXCHANGE_BYTES ||
				edgeBytes > MAX_PORTAL_EXCHANGE_BYTES - writer.Bytes().size())
				return Fail(error);
			writer.WriteUInt8(edge.Parent);
			writer.WriteUInt8(edge.Child);
			writer.WriteString(edge.PortalKey);
			Write(writer, edge.Centre);
			Write(writer, edge.First);
			Write(writer, edge.Second);
			Write(writer, edge.Position);
			Write(writer, edge.Orientation);
			writer.WriteFloat(edge.Scale);
			writer.WriteUInt32(edge.Geometry.size());
			writer.WriteRaw(edge.Geometry.data(), edge.Geometry.size());
			if (writer.Bytes().size() > MAX_PORTAL_EXCHANGE_BYTES) return Fail(error);
		}
		PortalCaptureTreeMeasure measure;
		if (!MeasurePortalCaptureTree(writer.Bytes(), measure, error)) return false;
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		error.clear();
		return true;
	}
}
