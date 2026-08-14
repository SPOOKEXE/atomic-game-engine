#include <engine/assets/Builtin.hpp>
#include <engine/assets/Resample.hpp>
#include <engine/bake/Graph.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/Model.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>

namespace engine::bake {

	namespace {
		const Payload EMPTY;

		bool IsInput(NodeKind kind) {
			return kind == NodeKind::Source || kind == NodeKind::Builtin;
		}
	}

	NodeId Graph::Append(Node node) {
		if (Nodes.size() >= MAXIMUM_NODES) {
			return {};
		}
		Nodes.push_back(std::move(node));

		// Ids are one-based so that a default-constructed `NodeId` names no
		// node rather than the first one - the same reason `RequestId::NONE` is
		// zero.
		return NodeId{static_cast<uint32_t>(Nodes.size())};
	}

	NodeId Graph::AddSource(std::string_view name, std::span<const std::byte> bytes) {
		Node node;
		node.Kind = NodeKind::Source;
		node.Name.assign(name);
		node.Result.Bytes.assign(bytes.begin(), bytes.end());
		return Append(std::move(node));
	}

	NodeId Graph::AddBuiltin(std::string_view name) {
		assets::BuiltinMesh mesh = assets::BuiltinMesh::Cube;
		if (!assets::BuiltinFromName(name, mesh)) {
			return {};
		}

		Node node;
		node.Kind = NodeKind::Builtin;
		node.Name.assign(name);
		return Append(std::move(node));
	}

	NodeId Graph::Add(NodeKind kind) {
		if (IsInput(kind)) {
			// The input kinds carry data the other overloads take, and one
			// added through here would evaluate to nothing with no way to say
			// what it was meant to hold.
			return {};
		}
		Node node;
		node.Kind = kind;
		return Append(std::move(node));
	}

	NodeId Graph::AddFit(float size) {
		Node node;
		node.Kind = NodeKind::Fit;
		node.Size = size;
		return Append(std::move(node));
	}

	NodeId Graph::AddScale(const core::Vector3 &amount) {
		Node node;
		node.Kind = NodeKind::Scale;
		node.Amount = amount;
		return Append(std::move(node));
	}

	NodeId Graph::AddResize(uint32_t width, uint32_t height) {
		Node node;
		node.Kind = NodeKind::Resize;
		node.Width = width;
		node.Height = height;
		return Append(std::move(node));
	}

	NodeId Graph::AddRasterize(uint32_t width, uint32_t height) {
		Node node;
		node.Kind = NodeKind::Rasterize;
		node.Width = width;
		node.Height = height;
		return Append(std::move(node));
	}

	NodeId Graph::AddRetime(float fps) {
		Node node;
		node.Kind = NodeKind::Retime;
		node.Size = fps;
		return Append(std::move(node));
	}

	NodeId Graph::AddWrite(std::string_view name) {
		Node node;
		node.Kind = NodeKind::Write;
		node.Name.assign(name);
		return Append(std::move(node));
	}

	bool Graph::Connect(NodeId from, NodeId to) {
		if (!from.IsValid() || !to.IsValid() || from.Value > Nodes.size() || to.Value > Nodes.size()) {
			return false;
		}
		if (from == to) {
			return false;
		}

		Node &target = Nodes[to.Value - 1];
		if (IsInput(target.Kind) || target.Input.IsValid()) {
			return false;
		}

		// The cycle check, and it is a walk up rather than a search: a node has
		// at most one input, so the only way this wire closes a loop is if `to`
		// is already somewhere above `from`.
		NodeId walk = from;
		size_t steps = 0;
		while (walk.IsValid()) {
			if (walk == to) {
				return false;
			}
			if (++steps > Nodes.size()) {
				// Unreachable while every wire has passed this check, and a
				// cheap standing guarantee that a corrupted graph cannot make
				// this spin.
				return false;
			}
			walk = Nodes[walk.Value - 1].Input;
		}

		target.Input = from;
		return true;
	}

	bool Graph::Evaluate(size_t index, std::string &failure) {
		Node &node = Nodes[index];

		if (IsInput(node.Kind)) {
			if (node.Kind == NodeKind::Source) {
				node.Result.Kind = PayloadKind::Bytes;
				node.Result.Source = node.Name;
				return true;
			}

			assets::BuiltinMesh builtin = assets::BuiltinMesh::Cube;
			if (!assets::BuiltinFromName(node.Name, builtin)) {
				failure = "graph: unknown built-in mesh '" + node.Name + "'";
				return false;
			}
			node.Result.Kind = PayloadKind::Mesh;
			node.Result.Source = node.Name;
			node.Result.Mesh = assets::MakeBuiltin(builtin);
			return true;
		}

		if (!node.Input.IsValid()) {
			failure = "graph: a node has no input";
			return false;
		}

		// Copied rather than referenced: the input may fan out to several
		// consumers, and a processor writing through a reference would change
		// what its siblings see.
		const Payload &input = Nodes[node.Input.Value - 1].Result;
		Payload result = input;

		const auto wrongKind = [&failure, &input](const char *wanted) {
			failure = "graph: node wants " + std::string(wanted) + " and its input is '" + input.Source + "'";
			return false;
		};

		switch (node.Kind) {
		case NodeKind::Import: {
			if (input.Kind != PayloadKind::Bytes) {
				return wrongKind("bytes");
			}

			// **The image sniff runs first and the model sniff second**, and the
			// order matters for exactly one case: a `.gltf` is JSON and JSON is
			// text, so a model sniff that ran first on an arbitrary file would
			// claim anything beginning with a brace. An image signature is
			// unambiguous, so trying it first costs nothing and removes the
			// overlap.
			if (ImageFormatOfBytes(input.Bytes) != ImageFormat::Unknown) {
				assets::TextureData texture;
				if (!ReadImage(input.Bytes, texture, failure)) {
					return false;
				}
				result.Kind = PayloadKind::Texture;
				result.Texture = std::move(texture);
				result.Bytes.clear();
				break;
			}

			// **The name is asked for exactly one format, and only once the
			// bytes have said nothing.** An SVG is XML and carries no signature,
			// so nothing but its name can identify it - and a `<svg` or `<?xml`
			// prefix sniff would be the same mistake the paragraph above
			// describes, a claim over text that the next XML-shaped format would
			// walk into. A signature still wins, so a `.svg` holding a PNG
			// decoded as one above.
			if (ImageFormatOfName(input.Source) == ImageFormat::Svg) {
				failure = "graph: '" + input.Source +
						  "' is an svg, which states a coordinate system and no pixels - import it with "
						  "a rasterize node, which carries the size";
				return false;
			}

			ModelFormat format = ModelFormatOfName(input.Source);
			if (format == ModelFormat::Unknown) {
				format = ModelFormatOfBytes(input.Bytes);
			}
			if (format == ModelFormat::Unknown) {
				failure = "graph: '" + input.Source + "' is not a format this imports";
				return false;
			}

			ImportedModel model;
			if (!ReadModel(format, input.Bytes, model, failure)) {
				return false;
			}
			result.Kind = PayloadKind::Mesh;
			result.Mesh = std::move(model.Mesh);
			result.Bytes.clear();
			break;
		}
		case NodeKind::Fit:
			if (input.Kind != PayloadKind::Mesh) {
				return wrongKind("a mesh");
			}
			if (!FitMesh(result.Mesh, node.Size)) {
				failure = "graph: '" + input.Source + "' cannot be fitted - it has no extent";
				return false;
			}
			break;
		case NodeKind::Scale: {
			if (input.Kind != PayloadKind::Mesh) {
				return wrongKind("a mesh");
			}
			const float amount[3] = {node.Amount.X, node.Amount.Y, node.Amount.Z};
			for (assets::MeshVertex &vertex : result.Mesh.Vertices) {
				for (int axis = 0; axis < 3; axis++) {
					vertex.Position[axis] *= amount[axis];
				}
			}

			// **A non-uniform scale invalidates the normals**, so they are
			// rebuilt rather than left pointing where the unscaled surface
			// faced. A uniform one does not, and rebuilding anyway would turn a
			// flat-shaded model smooth as a side effect of resizing it.
			if (amount[0] != amount[1] || amount[1] != amount[2]) {
				SmoothNormals(result.Mesh);
			}
			result.Mesh.ComputeBounds();
			break;
		}
		case NodeKind::Smooth:
			if (input.Kind != PayloadKind::Mesh) {
				return wrongKind("a mesh");
			}
			SmoothNormals(result.Mesh);
			break;
		case NodeKind::Rasterize: {
			if (input.Kind != PayloadKind::Bytes) {
				return wrongKind("bytes");
			}

			// **A format that already has pixels is refused here rather than
			// handed to an XML parser**, which would fail somewhere inside a
			// PNG's compressed data with a message about markup. This node is
			// for the one format with no size of its own.
			const ImageFormat sniffed = ImageFormatOfBytes(input.Bytes);
			if (sniffed != ImageFormat::Unknown) {
				failure = "graph: '" + input.Source + "' is a " + std::string(Describe(sniffed)) +
						  " and already has a size - import it, and resize it if it is the wrong one";
				return false;
			}

			assets::TextureData texture;
			if (!RasterizeSvg(input.Bytes, node.Width, node.Height, texture, failure)) {
				return false;
			}
			result.Kind = PayloadKind::Texture;
			result.Texture = std::move(texture);
			result.Bytes.clear();
			break;
		}
		case NodeKind::Resize: {
			if (input.Kind != PayloadKind::Texture) {
				return wrongKind("a texture");
			}
			assets::TextureData resized;
			if (!assets::ResizeImage(input.Texture, node.Width, node.Height, resized)) {
				failure = "graph: '" + input.Source + "' cannot be resized to that size";
				return false;
			}
			result.Texture = std::move(resized);
			break;
		}
		case NodeKind::Opaque:
			if (input.Kind != PayloadKind::Texture) {
				return wrongKind("a texture");
			}
			if (result.Texture.Format == assets::TextureFormat::RGBA8) {
				for (size_t pixel = 3; pixel < result.Texture.Pixels.size(); pixel += 4) {
					result.Texture.Pixels[pixel] = std::byte{255};
				}
			}
			break;
		case NodeKind::Mipmap:
			if (input.Kind != PayloadKind::Texture) {
				return wrongKind("a texture");
			}
			// **Last of the texture nodes, and the graph does not enforce it.**
			// A resize after this one drops the chain and an opaque pass after it
			// would leave the levels' alpha as it was, so the order is the
			// pipeline's to get right - `Bake.cpp` puts it immediately before the
			// write for that reason.
			if (!assets::BuildMipChain(result.Texture)) {
				failure = "graph: '" + input.Source + "' cannot carry a mip chain";
				return false;
			}
			break;
		case NodeKind::Retime:
			if (input.Kind != PayloadKind::Texture) {
				return wrongKind("a texture");
			}
			// **Only a flipbook is retimed.** A frame rate on a still image is
			// a number nothing would read, and stamping one would make
			// `TextureData::IsFlipbook` - which asks about the grid, not the
			// rate - the only thing keeping them apart.
			if (node.Size > 0.0f && result.Texture.IsFlipbook()) {
				result.Texture.FlipbookFrameRate = node.Size;
			}
			break;
		case NodeKind::Write: {
			core::ByteWriter writer;
			BakedAsset baked;
			baked.Name = node.Name;

			if (input.Kind == PayloadKind::Mesh) {
				if (!assets::Mesh::Write(writer, input.Mesh)) {
					failure = "graph: '" + node.Name + "' is not a mesh the format can hold";
					return false;
				}
				baked.Kind = assets::AssetKind::Mesh;
			} else if (input.Kind == PayloadKind::Texture) {
				if (!assets::Texture::Write(writer, input.Texture)) {
					failure = "graph: '" + node.Name + "' is not a texture the format can hold";
					return false;
				}
				baked.Kind = assets::AssetKind::Texture;
			} else {
				return wrongKind("a mesh or a texture");
			}

			const std::span<const std::byte> bytes = writer.Bytes();
			baked.Bytes.assign(bytes.begin(), bytes.end());
			Exports.push_back(std::move(baked));

			// A write passes its input through unchanged, so a chain may write
			// a mesh and then go on doing something else with it.
			break;
		}
		case NodeKind::Source:
		case NodeKind::Builtin:
			break;
		}

		node.Result = std::move(result);
		return true;
	}

	bool Graph::Run(std::string &failure) {
		Exports.clear();
		for (Node &node : Nodes) {
			node.Result.Kind = PayloadKind::None;
		}

		// Kahn's order, over a graph where in-degree is zero or one. Repeated
		// scans rather than a queue because a bake graph is tens of nodes and
		// the simple version is the one a reader can check.
		std::vector<bool> done(Nodes.size(), false);
		size_t remaining = Nodes.size();

		while (remaining > 0) {
			bool progressed = false;

			for (size_t index = 0; index < Nodes.size(); index++) {
				if (done[index]) {
					continue;
				}
				const NodeId input = Nodes[index].Input;
				if (input.IsValid() && !done[input.Value - 1]) {
					continue;
				}
				if (!Evaluate(index, failure)) {
					return false;
				}
				done[index] = true;
				remaining--;
				progressed = true;
			}

			if (!progressed) {
				// Unreachable while `Connect` refuses cycles, and left in
				// because the alternative to an impossible-state check here is
				// an infinite loop.
				failure = "graph: a cycle survived the wiring check";
				return false;
			}
		}
		return true;
	}

	const Payload &Graph::Output(NodeId node) const {
		if (!node.IsValid() || node.Value > Nodes.size()) {
			return EMPTY;
		}
		return Nodes[node.Value - 1].Result;
	}

	std::span<const BakedAsset> Graph::Baked() const {
		return Exports;
	}

	size_t Graph::NodeCount() const {
		return Nodes.size();
	}
}
