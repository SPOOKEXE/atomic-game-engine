#include "SourceRigidFracture.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/RigidMeshAction.hpp>

#include <algorithm>
#include <new>
namespace engine::imagegraph {
	static Status PrepareRigidMeshActionImpl(
		const EvaluationSnapshot &inputs,
		uint64_t maximumBytes,
		ArrayValue &replacement,
		Diagnostic &diagnostic
	) {
		const auto refuse = [&](Status status, std::string message) {
			diagnostic = {};
			diagnostic.Code = status;
			diagnostic.Message = std::move(message);
			diagnostic.Port = "attribute_mesh";
			return status;
		};
		const ArrayValue *prior = nullptr;
		double expansion = 0;
		bool addPixel = true;
		for (const auto &input : inputs.Values()) {
			if (input.Port == "attribute_mesh") prior = std::get_if<ArrayValue>(&input.Data);
			if (input.Port == "mesh_expansion") {
				if (const auto *value = std::get_if<double>(&input.Data))
					expansion = *value;
				else
					return refuse(Status::InvalidValue, "mesh action expansion is not a scalar");
			}
			if (input.Port == "add_pixel_for_empty") {
				if (const auto *value = std::get_if<bool>(&input.Data))
					addPixel = *value;
				else
					return refuse(Status::InvalidValue, "mesh action pixel option is not Boolean");
			}
		}
		if (prior &&
			(!detail::ValidPayload(*prior, true) || !prior->Nested.empty() || !prior->Elements.empty()))
			return refuse(Status::UnsupportedExecution, "mesh action requires owned source mesh rows");
		const uint64_t fixed =
			inputs.RetainedBytes() + sizeof(ArrayValue) + detail::RetainedPayloadBytes(replacement) +
			(prior ? detail::RetainedPayloadBytes(*prior) : 0) +
			256 * (sizeof(const Image *) + sizeof(SourceArrayItem) + 8 * sizeof(SourceArrayItem));
		if (fixed > maximumBytes)
			return refuse(
				Status::LimitExceeded,
				"mesh action copied inputs and attribute replacement exceed byte budget"
			);
		std::vector<const Image *> textures;
		textures.reserve(256);
		const auto add = [&](const Image *image) {
			if (textures.size() == 256) return false;
			textures.push_back(image);
			return true;
		};
		const auto leafImage = [](const ElementValue &leaf) -> const Image * {
			if (const auto *surface = std::get_if<SurfaceValue>(&leaf)) return &surface->Data;
			if (const auto *atlas = std::get_if<AtlasValue>(&leaf);
				atlas && atlas->Data && atlas->Data->Kind == AtlasKind::SurfaceAtlas)
				return &atlas->Data->Surface.Data;
			return nullptr;
		};
		for (const auto &input : inputs.Images())
			if (input.Port == "texture")
				if (!add(&input.Data))
					return refuse(Status::LimitExceeded, "mesh action texture list exceeds 256");
		if (textures.empty())
			for (const auto &input : inputs.Values())
				if (input.Port == "texture") {
					if (const auto *array = std::get_if<ArrayValue>(&input.Data)) {
						if (!array->Nested.empty())
							return refuse(
								Status::UnsupportedExecution,
								"mesh action requires a flat source texture list"
							);
						for (const auto &leaf : array->Elements)
							if (!add(leafImage(leaf)))
								return refuse(Status::LimitExceeded, "mesh action texture list exceeds 256");
						for (const auto &item : array->Items) {
							const Image *image = std::get_if<Image>(&item.Data);
							if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								image = leafImage(*leaf);
							if (!add(image))
								return refuse(Status::LimitExceeded, "mesh action texture list exceeds 256");
						}
					} else if (const auto *surface = std::get_if<SurfaceValue>(&input.Data))
						add(&surface->Data);
					else if (const auto *atlas = std::get_if<AtlasValue>(&input.Data);
							 atlas && atlas->Data && atlas->Data->Kind == AtlasKind::SurfaceAtlas)
						add(&atlas->Data->Surface.Data);
				}
		if (textures.empty())
			for (const auto &input : inputs.ImageArrays())
				if (input.Port == "texture")
					for (const auto &item : input.Data.Items) {
						const auto *index = std::get_if<size_t>(&item.Data);
						if (!index || *index >= input.Data.Images.size())
							return refuse(
								Status::UnsupportedExecution,
								"mesh action requires a flat captured texture array"
							);
						if (!add(&input.Data.Images[*index]))
							return refuse(Status::LimitExceeded, "mesh action texture list exceeds 256");
					}
		ArrayValue candidate;
		if (prior) candidate = *prior;
		candidate.ElementType = ValueType::Any;
		uint64_t retained = fixed;
		for (size_t index = 0; index < textures.size(); ++index) {
			if (!textures[index]) continue;
			std::vector<Vector2> mesh;
			Diagnostic kernel;
			const auto status = detail::SourceRigidGenerateObjectMesh(
				*textures[index], expansion, addPixel, maximumBytes - retained, mesh, kernel
			);
			if (status != Status::Ok) {
				diagnostic = std::move(kernel);
				diagnostic.Port = "attribute_mesh";
				return status;
			}
			if (mesh.empty()) continue;
			const uint64_t rowBytes = mesh.size() * sizeof(SourceArrayItem);
			if (rowBytes > maximumBytes - retained)
				return refuse(Status::LimitExceeded, "mesh action generated points exceed byte budget");
			retained += rowBytes;
			if (candidate.Items.size() <= index) candidate.Items.resize(index + 1);
			std::vector<SourceArrayItem> points;
			points.reserve(mesh.size());
			for (const auto &point : mesh)
				points.push_back({ElementValue{point}});
			candidate.Items[index].Data = std::move(points);
		}
		replacement = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}
	Status PrepareRigidMeshAction(
		const EvaluationSnapshot &inputs,
		uint64_t maximumBytes,
		ArrayValue &replacement,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph rigid mesh action");
		if (maximumBytes == 0 || maximumBytes > Limits::MaximumEvaluationBytes) {
			diagnostic = {
				Status::LimitExceeded, {}, "attribute_mesh", "mesh action byte cap is outside native bounds"
			};
			return diagnostic.Code;
		}
		try {
			return PrepareRigidMeshActionImpl(inputs, maximumBytes, replacement, diagnostic);
		} catch (const std::bad_alloc &) {
			diagnostic = {Status::LimitExceeded, {}, "attribute_mesh", "mesh action allocation failed"};
			return diagnostic.Code;
		}
	}
}
