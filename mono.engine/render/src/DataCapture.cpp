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
		case DataCaptureChannel::AmbientOcclusion:
			return "ambient_occlusion";
		case DataCaptureChannel::ObjectIds:
			return "object_ids";
		case DataCaptureChannel::SemanticMask:
			return "semantic_mask";
		case DataCaptureChannel::PartMask:
			return "part_mask";
		case DataCaptureChannel::MotionVectors:
			return "motion_vectors";
		case DataCaptureChannel::OpticalFlow:
			return "optical_flow";
		}
		return "unknown";
	}

	DataCaptureCameraConvention DataCaptureCameraConventions() {
		return {};
	}
}
