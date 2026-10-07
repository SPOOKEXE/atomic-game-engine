#include "SourceShape3DExecutor.hpp"

#include "Families.hpp"
#include "Processor.hpp"
#include "SourceShape3DGeometry.hpp"
#include "SourceShape3DProjection.hpp"

#include <new>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace source_shape3d_executor {
		struct Inputs {
			SourceShape3DRecipe Recipe;
			AllocationReservation Charge;
			std::vector<const Image *> Textures;
			const Image *Background = nullptr;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
		};
		bool Prepare(NodeContext &c, Inputs &in) {
			if (!BuildSourceShape3DRecipe(c, in.Recipe)) return false;

			in.Background = c.Input("background");
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			auto charge = c.ReserveWorkspace(Limits::MaximumArrayElements * sizeof(const Image *), "texture");
			if (!charge) return false;
			in.Charge = std::move(*charge);
			in.Textures.reserve(Limits::MaximumArrayElements);
			if (const auto *single = c.Input("texture"))
				in.Textures.push_back(single);
			else {
				const ImageArray *array = nullptr;
				for (const auto &[port, value] : c.ImageArrays)
					if (port == "texture") array = value;
				if (array) {
					const auto *items = &array->Items;
					if (const auto *row = c.Find("__shape3d_texture_row")) {
						const auto *index = std::get_if<int64_t>(row);
						if (!index || *index < 0 || size_t(*index) >= items->size())
							return c.Fail(Status::InvalidValue, "Shape 3D texture row is invalid", "texture");
						items = std::get_if<std::vector<ImageArrayItem>>(&(*items)[size_t(*index)].Data);
						if (!items)
							return c.Fail(
								Status::InvalidValue,
								"Shape 3D texture row requires a surface list",
								"texture"
							);
					}
					if (items->size() > Limits::MaximumArrayElements)
						return c.Fail(
							Status::LimitExceeded, "Shape 3D texture list exceeds bounds", "texture"
						);
					for (const auto &item : *items) {
						const auto *index = std::get_if<size_t>(&item.Data);
						if (!index || *index >= array->Images.size())
							return c.Fail(
								Status::InvalidValue,
								"Shape 3D texture list contains an invalid surface",
								"texture"
							);
						in.Textures.push_back(&array->Images[*index]);
					}
				} else if (const auto *value = c.Find("texture")) {
					const auto *arrayValue = std::get_if<ArrayValue>(value);
					if (!arrayValue || !arrayValue->Elements.empty() || !arrayValue->Nested.empty())
						return c.Fail(Status::TypeMismatch, "Shape 3D texture requires surfaces", "texture");
					for (const auto &item : arrayValue->Items) {
						const auto *image = std::get_if<Image>(&item.Data);
						if (!image || in.Textures.size() == Limits::MaximumArrayElements)
							return c.Fail(
								Status::InvalidValue,
								"Shape 3D texture list contains a non-surface",
								"texture"
							);
						in.Textures.push_back(image);
					}
				}
			}
			return c.FailureCode == Status::Ok;
		}
		bool Quote(NodeContext &c, const Inputs &in, uint64_t &work) {
			// Conservative quote includes full triangle bounding boxes, not only covered fragments.
			const uint64_t pixels = uint64_t(in.Recipe.Width) * in.Recipe.Height;
			const uint64_t rows = std::max<size_t>(c.ProcessorCount, 1);
			const uint64_t limit = 64000000 / rows;
			uint64_t vertices = 0;
			const uint64_t side = static_cast<uint64_t>(in.Recipe.Side);
			switch (in.Recipe.Shape) {
			case SourceShape3DKind::Plane:
				vertices = 6;
				break;
			case SourceShape3DKind::Cube:
				vertices = 36;
				break;
			case SourceShape3DKind::Octahedron:
				vertices = 24;
				break;
			case SourceShape3DKind::Cylinder:
				vertices = side * (in.Recipe.Caps ? 12 : 6);
				break;
			case SourceShape3DKind::Cone:
				vertices = side * 6;
				break;
			case SourceShape3DKind::Capsule:
				vertices = side * side * 12 + side * 6;
				break;
			case SourceShape3DKind::Sphere:
				vertices = uint64_t(in.Recipe.Sides.X) * uint64_t(in.Recipe.Sides.Y) * 6;
				break;
			case SourceShape3DKind::CutSphere:
				vertices = uint64_t(in.Recipe.Sides.Y) *
						   (uint64_t(std::ceil(in.Recipe.Sides.X * in.Recipe.Ratio)) * 6 + 3);
				break;
			case SourceShape3DKind::Torus:
				vertices = 768;
				break;
			}
			uint64_t rowWork = vertices * 128 + in.Recipe.Colours.size();
			const auto add = [&](uint64_t amount) {
				if (amount > limit - rowWork) return false;
				rowWork += amount;
				return true;
			};
			if (rowWork > limit)
				return c.Fail(
					Status::LimitExceeded, "Shape 3D geometry and projection exceed whole batch work bounds"
				);
			const int64_t interpolation =
				in.Recipe.Interpolation ? in.Recipe.Interpolation : c.InheritedInterpolation;
			const uint64_t sample = interpolation == 4 ? 144 : interpolation == 1 ? 1 : 4;
			if (!add(pixels * (in.Background ? 8 : 3)))
				return c.Fail(Status::LimitExceeded, "Shape 3D output work exceeds whole batch bounds");
			for (const Image *image : in.Textures) {
				if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
					return c.Fail(Status::InvalidValue, "Shape 3D texture layout is invalid", "texture");
				if (!add(uint64_t(image->Width) * image->Height * 4))
					return c.Fail(
						Status::LimitExceeded, "Shape 3D texture validation exceeds whole batch bounds"
					);
			}
			if (in.Background &&
				(!ValidSurfaceLayout(*in.Background, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				 !add(uint64_t(in.Background->Width) * in.Background->Height * 4)))
				return c.Fail(Status::LimitExceeded, "Shape 3D background validation exceeds bounds");
			// Bound every triangle by the complete target before any geometry or sample scans.
			if (!add(pixels * (sample + 16) * (vertices / 3)))
				return c.Fail(Status::LimitExceeded, "Shape 3D sample work exceeds whole batch bounds");
			if (rowWork > 64000000 - work)
				return c.Fail(Status::LimitExceeded, "Shape 3D accumulated batch work exceeds bounds");
			work += rowWork;
			return true;
		}
		bool Draw(NodeContext &c) try {
			ENGINE_PROFILE("imagegraph.shape3d.executor");
			Inputs in;
			uint64_t work = 0;
			if (!Prepare(c, in) || !Quote(c, in, work)) return false;
			MeshValue3D geometry;
			AllocationReservation geometryCharge;
			SourceShape3DProjectionResult projected;
			SourceShape3DRasterResult raster;
			if (!BuildSourceShape3DGeometry(c, in.Recipe, geometry, geometryCharge) ||
				!BuildSourceShape3DProjection(
					c, in.Recipe, geometry, SourceShape3DFramebufferProfile::NativeTopOrigin, projected
				) ||
				!RasterSourceShape3D(
					c, in.Recipe, projected.Triangles, in.Textures, in.Background, in.Format, raster
				))
				return false;
			constexpr std::array<std::string_view, 3> ports{"surface_out", "depth", "rim_normal"};
			uint64_t bytes = 0;
			for (size_t i = 0; i < 3; ++i)
				bytes += raster.Images[i].Pixels.size() + std::max(ports[i].size(), std::string{}.capacity());
			if (!c.ReserveOutput(bytes)) return false;
			for (size_t i = 0; i < 3; ++i) {
				Image *image = c.NewImage(ports[i], in.Recipe.Width, in.Recipe.Height, in.Format);
				if (!image) {
					c.ClearOutputs();
					return false;
				}
				*image = std::move(raster.Images[i]);
			}
			return true;
		} catch (const std::bad_alloc &) {
			c.ClearOutputs();
			return c.Fail(Status::LimitExceeded, "Shape 3D executor allocation failed");
		} catch (const std::length_error &) {
			c.ClearOutputs();
			return c.Fail(Status::LimitExceeded, "Shape 3D executor container bounds exceeded");
		}
	}
	bool AdmitSourceShape3D(NodeContext &c, uint64_t &work) try {
		source_shape3d_executor::Inputs in;
		return source_shape3d_executor::Prepare(c, in) && source_shape3d_executor::Quote(c, in, work);
	} catch (const std::bad_alloc &) {
		return c.Fail(Status::LimitExceeded, "Shape 3D admission allocation failed");
	} catch (const std::length_error &) {
		return c.Fail(Status::LimitExceeded, "Shape 3D admission container bounds exceeded");
	}
	std::span<const ExecutorEntry> SourceShape3DExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.shape_3_d", source_shape3d_executor::Draw, true}};
		return entries;
	}
}
