#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/render/SourceTransformImage3D.hpp>

#include <algorithm>
#include <cmath>
#include <new>

namespace engine::render::imagegraph {
	namespace {
		namespace source = engine::imagegraph;
		const source::Value *Input(const source::HostNodeInvocation &i, std::string_view port) {
			for (const auto &v : i.Inputs)
				if (v.Port == port) return &v.Data;
			return nullptr;
		}
		template <class T> const T *Typed(const source::HostNodeInvocation &i, std::string_view port) {
			const auto *v = Input(i, port);
			return v ? std::get_if<T>(v) : nullptr;
		}
		const source::Image *Surface(const source::HostNodeInvocation &i, std::string_view port) {
			for (const auto &v : i.Images)
				if (v.Port == port) return v.Data;
			return nullptr;
		}
		std::optional<assets::TextureFormat> Format(source::SurfaceFormat f) {
			switch (f) {
			case source::SurfaceFormat::RGBA8Unorm:
				return assets::TextureFormat::RGBA8_LINEAR;
			case source::SurfaceFormat::RGBA4Unorm:
				return assets::TextureFormat::RGBA4_UNORM;
			case source::SurfaceFormat::RGBA16Float:
				return assets::TextureFormat::RGBA16_FLOAT;
			case source::SurfaceFormat::RGBA32Float:
				return assets::TextureFormat::RGBA32_FLOAT;
			case source::SurfaceFormat::R8Unorm:
				return assets::TextureFormat::R8;
			case source::SurfaceFormat::R16Float:
				return assets::TextureFormat::R16_FLOAT;
			case source::SurfaceFormat::R32Float:
				return assets::TextureFormat::R32_FLOAT;
			}
			return {};
		}
		std::optional<uint64_t> MeshBytes(const source::MeshValue3D &mesh) {
			uint64_t bytes = sizeof(mesh);
			const auto add = [&](uint64_t n) {
				if (n > MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES - bytes) return false;
				bytes += n;
				return true;
			};
			if (!mesh.Data) return bytes;
			const auto &d = *mesh.Data;
			if (!add(sizeof(d)) || !add(d.Parts.capacity() * sizeof(source::MeshPart3D)) ||
				!add(d.Edges.capacity() * sizeof(source::MeshEdge3D)) ||
				!add(d.Materials.capacity() * sizeof(source::MaterialValue3D)) ||
				!add(d.LocalTransforms.capacity() * sizeof(source::MeshTransform3D)) ||
				!add(d.Instances.capacity() * sizeof(source::MeshInstance3D)))
				return {};
			for (const auto &p : d.Parts)
				if (!add(p.Vertices.capacity() * sizeof(source::MeshVertex3D))) return {};
			for (const auto &m : d.Materials)
				if (m.Data) {
					if (!add(sizeof(source::MaterialData3D))) return {};
					for (const auto *image : {&m.Data->Surface, &m.Data->Normal, &m.Data->PropertiesMap})
						if (*image && !add((*image)->Pixels.capacity())) return {};
				}
			return bytes;
		}
		constexpr std::array<source::MeshVertex3D, 6> PLANE{
			{{{-.5, -.5, 0}, {0, 0, 1}, {0, 0}},
			 {{.5, .5, 0}, {0, 0, 1}, {1, 1}},
			 {{.5, -.5, 0}, {0, 0, 1}, {0, 1}},
			 {{-.5, -.5, 0}, {0, 0, 1}, {0, 0}},
			 {{-.5, .5, 0}, {0, 0, 1}, {1, 0}},
			 {{.5, .5, 0}, {0, 0, 1}, {1, 1}}}
		};
	}
	bool BuildSourceTransformImage3DRequest(
		const source::HostNodeInvocation &invocation,
		TransformImage3DRequest &output,
		source::MeshValue3D &outputMesh,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraph source transform request");
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		if (invocation.Inputs.size() > 64 || invocation.Images.size() > 2)
			return fail("Source Transform selected-input count exceeds budget");
		if (invocation.Authored.Type != "pc.3_d_transform_image" &&
			invocation.Authored.Type != "image.transform_3d")
			return fail("Source Transform host requires pc.3_d_transform_image");
		const auto *front = Surface(invocation, "surface");
		const auto *back = Surface(invocation, "back_surface");
		if (!front || !source::ValidSurfaceLayout(*front, 4096, MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES) ||
			!source::FiniteSurfaceSamples(*front) ||
			(back && (!source::ValidSurfaceLayout(*back, 4096, MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES) ||
					  !source::FiniteSurfaceSamples(*back))))
			return fail("Source Transform requires valid finite surfaces");
		if (!back) back = front;
		if (invocation.Authored.Type == "image.transform_3d" &&
			(front->Width != back->Width || front->Height != back->Height))
			return fail("Source Transform back surface dimensions differ");
		const auto frontFormat = Format(front->Format), backFormat = Format(back->Format);
		const auto *position = Typed<source::Vector3>(invocation, "position");
		const auto *anchor = Typed<source::Vector3>(invocation, "anchor");
		const auto *rotation = Typed<source::Quaternion>(invocation, "rotation");
		const auto *scale = Typed<source::Vector3>(invocation, "scale");
		const auto *tiling = Typed<source::Vector2>(invocation, "texture_tiling");
		const auto *projection = Typed<source::EnumValue>(invocation, "projection");
		const auto *fov = Typed<double>(invocation, "fov");
		const auto *view = Typed<source::Vector2>(invocation, "view_range");
		const auto *depth = Typed<source::Vector2>(invocation, "depth_range");
		const auto *interpolate = Typed<source::EnumValue>(invocation, "interpolate");
		if (!frontFormat || !backFormat || !position || !anchor || !rotation || !scale || !tiling ||
			!projection || !fov || !view || !depth || (projection->Value != 0 && projection->Value != 1))
			return fail("Source Transform requires selected scalar controls");
		const uint64_t maximum =
			std::min(invocation.MaximumOperationBytes, MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES);
		const auto previousRequest =
			sizeof(output) + output.Front.Pixels.capacity() + output.Back.Pixels.capacity();
		const auto previousMesh = MeshBytes(outputMesh);
		if (!previousMesh || previousRequest > maximum || *previousMesh > maximum - previousRequest)
			return fail("Source Transform previous output exceeds operation budget");
		// Request pixels and two independent source materials overlap during publication.
		uint64_t needed = sizeof(TransformImage3DRequest) + sizeof(source::MeshValue3D) +
						  sizeof(source::MeshData3D) + 2 * sizeof(source::MeshPart3D) +
						  12 * sizeof(source::MeshVertex3D) + 4 * sizeof(source::MeshEdge3D) +
						  2 * sizeof(source::MaterialValue3D) + 2 * sizeof(source::MaterialData3D) +
						  sizeof(source::MeshTransform3D);
		for (auto bytes :
			 {front->Pixels.size(), back->Pixels.size(), front->Pixels.capacity(), back->Pixels.capacity()}) {
			if (bytes > maximum || needed > maximum - bytes)
				return fail("Source Transform captures exceed operation budget");
			needed += bytes;
		}
		if (needed > maximum - previousRequest - *previousMesh)
			return fail("Source Transform replacement exceeds operation budget");
		TransformImage3DRequest request;
		request.SourcePlane = invocation.Authored.Type == "pc.3_d_transform_image";
		request.LinearFilter =
			(interpolate ? (interpolate->Value == 0 ? invocation.Interpolation : interpolate->Value)
						 : invocation.Interpolation) > 1;
		request.Position = {float(position->X), float(position->Y), float(position->Z)};
		request.Anchor = {float(anchor->X), float(anchor->Y), float(anchor->Z)};
		request.Rotation = {float(rotation->X), float(rotation->Y), float(rotation->Z), float(rotation->W)};
		const bool sourcePlane = invocation.Authored.Type == "pc.3_d_transform_image";
		const double aspect =
			sourcePlane && projection->Value == 0 ? double(front->Width) / front->Height : 1;
		request.Scale = {float(scale->X), float(scale->Y * aspect), float(scale->Z)};
		request.TextureTiling = {float(tiling->X), float(tiling->Y)};
		request.ViewRange = {float(view->X), float(view->Y)};
		request.DepthRange = {float(depth->X), float(depth->Y)};
		request.FieldOfViewDegrees = float(*fov);
		request.Projection = projection->Value == 0 ? TransformImage3DProjection::Perspective
													: TransformImage3DProjection::Orthographic;
		request.Front = {front->Width, front->Height, *frontFormat, {}};
		request.Back = {back->Width, back->Height, *backFormat, {}};
		// Validate all controls before retaining source-sized buffers.
		{
			auto probe = request;
			probe.Front.Width = probe.Front.Height = 1;
			probe.Front.Pixels.resize(assets::BytesPerPixel(*frontFormat));
			probe.Back = {};
			if (ValidateTransformImage3D(probe) != TransformImage3DStatus::Ok)
				return fail("Source Transform controls are invalid");
		}
		request.Front.Pixels.resize(front->Pixels.size());
		request.Back.Pixels.resize(back->Pixels.size());
		std::transform(
			front->Pixels.begin(), front->Pixels.end(), request.Front.Pixels.begin(), [](uint8_t b) {
				return std::byte(b);
			}
		);
		std::transform(back->Pixels.begin(), back->Pixels.end(), request.Back.Pixels.begin(), [](uint8_t b) {
			return std::byte(b);
		});
		source::MeshValue3D mesh;
		auto &data = mesh.Data.emplace();
		data.Parts.resize(sourcePlane ? 2 : 1);
		data.Parts[0].Vertices.assign(PLANE.begin(), PLANE.end());
		if (sourcePlane) {
			data.Parts[1].MaterialIndex = 1;
			data.Parts[1].Vertices.reserve(6);
			for (auto v = PLANE.rbegin(); v != PLANE.rend(); ++v) {
				auto vertex = *v;
				vertex.Normal.Z = -1;
				data.Parts[1].Vertices.push_back(vertex);
			}
		} else {
			const std::array<source::Vector3, 6> points{
				{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}
			};
			const std::array<source::Vector2, 6> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 1}, {1, 0}, {0, 0}}};
			for (size_t i = 0; i < 6; ++i) {
				data.Parts[0].Vertices[i].Position = points[i];
				data.Parts[0].Vertices[i].UV = uv[i];
			}
		}
		data.Edges = {
			{{-.5, -.5, 0}, {.5, -.5, 0}},
			{{.5, -.5, 0}, {.5, .5, 0}},
			{{.5, .5, 0}, {-.5, .5, 0}},
			{{-.5, .5, 0}, {-.5, -.5, 0}}
		};
		if (!sourcePlane)
			for (auto &edge : data.Edges) {
				edge.From.X *= 2;
				edge.From.Y *= 2;
				edge.To.X *= 2;
				edge.To.Y *= 2;
			}
		data.Materials.resize(sourcePlane ? 2 : 1);
		data.Materials[0].Edit().Surface = sourcePlane ? *front : *back;
		if (sourcePlane) data.Materials[1].Edit().Surface = *back;
		data.LocalTransforms.push_back(
			{*position, *anchor, *rotation, {scale->X, scale->Y * aspect, scale->Z}}
		);
		const auto meshBytes = MeshBytes(mesh);
		const uint64_t actual =
			sizeof(request) + request.Front.Pixels.capacity() + request.Back.Pixels.capacity();
		if (!meshBytes || actual > maximum - previousRequest - *previousMesh ||
			*meshBytes > maximum - previousRequest - *previousMesh - actual)
			return fail("Source Transform actual retained capacities exceed operation budget");
		core::Metrics::Count(
			"imagegraph.source_transform.copied_surface_bytes",
			front->Pixels.size() + back->Pixels.size() +
				(sourcePlane ? front->Pixels.size() + back->Pixels.size() : back->Pixels.size())
		);
		core::Metrics::Count("imagegraph.source_transform.surface_copies", sourcePlane ? 4 : 3);
		output = std::move(request);
		outputMesh = std::move(mesh);
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "Source Transform request allocation failed";
		return false;
	}
}
