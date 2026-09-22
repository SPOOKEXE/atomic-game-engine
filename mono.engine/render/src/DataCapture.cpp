#include <engine/render/DataCapture.hpp>

namespace engine::render {
	const char *DataCaptureChannelName(DataCaptureChannel channel) {
		switch (channel) {
		case DataCaptureChannel::RgbLinearHdr:
			return "rgb_linear_hdr";
		case DataCaptureChannel::LinearDepth:
			return "linear_depth";
		case DataCaptureChannel::ShadingNormal:
			return "shading_normal";
		case DataCaptureChannel::PbrAlbedo:
			return "pbr_albedo";
		case DataCaptureChannel::PbrMaterial:
			return "pbr_material";
		case DataCaptureChannel::PbrEmissive:
			return "pbr_emissive";
		case DataCaptureChannel::PbrSpecular:
			return "pbr_specular";
		case DataCaptureChannel::PbrTransmission:
			return "pbr_transmission";
		case DataCaptureChannel::MeshUv:
			return "mesh_uv";
		case DataCaptureChannel::AmbientOcclusion:
			return "ambient_occlusion";
		case DataCaptureChannel::ObjectIds:
			return "object_ids";
		case DataCaptureChannel::SemanticMask:
			return "semantic_ids";
		case DataCaptureChannel::PartMask:
			return "part_ids";
		case DataCaptureChannel::FirstSurfaceValidity:
			return "first_surface_validity";
		case DataCaptureChannel::SecondSurfaceDepth:
			return "second_surface_depth";
		case DataCaptureChannel::SecondSurfaceValidity:
			return "second_surface_validity";
		case DataCaptureChannel::MotionVectors:
			return "motion_vectors";
		case DataCaptureChannel::OpticalFlow:
			return "optical_flow";
		case DataCaptureChannel::DirectionalResponse:
			return "directional_response";
		case DataCaptureChannel::ShadowVisibility:
			return "shadow_visibility";
		case DataCaptureChannel::PackedGpu:
			return "packed_gpu";
		}
		return "unknown";
	}

	DataCaptureCameraConvention DataCaptureCameraConventions() {
		return {};
	}
}
