#pragma once
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
namespace engine::render::imagegraph {
	template <class T> struct alignas(16) SourceUniformValue {
		T Value{};
	};
	struct alignas(16) SourceCameraUniforms {
		alignas(16) int32_t use_8bit{};
		alignas(16) glm::vec4 light_ambient{};
		alignas(16) float shadowBias{};
		alignas(16) int32_t light_dir_count{};
		alignas(16) std::array<SourceUniformValue<glm::vec3>, 8> light_dir_direction{};
		alignas(16) std::array<SourceUniformValue<glm::vec4>, 8> light_dir_color{};
		alignas(16) std::array<SourceUniformValue<float>, 8> light_dir_intensity{};
		alignas(16) std::array<SourceUniformValue<glm::mat4>, 8> light_dir_view{};
		alignas(16) std::array<SourceUniformValue<glm::mat4>, 8> light_dir_proj{};
		alignas(16) std::array<SourceUniformValue<int32_t>, 8> light_dir_shadow_active{};
		alignas(16) std::array<SourceUniformValue<float>, 8> light_dir_shadow_bias{};
		alignas(16) int32_t light_pnt_count{};
		alignas(16) std::array<SourceUniformValue<glm::vec3>, 8> light_pnt_position{};
		alignas(16) std::array<SourceUniformValue<glm::vec4>, 8> light_pnt_color{};
		alignas(16) std::array<SourceUniformValue<float>, 8> light_pnt_intensity{};
		alignas(16) std::array<SourceUniformValue<float>, 8> light_pnt_radius{};
		alignas(16) std::array<SourceUniformValue<glm::mat4>, 48> light_pnt_view{};
		alignas(16) std::array<SourceUniformValue<glm::mat4>, 8> light_pnt_proj{};
		alignas(16) std::array<SourceUniformValue<int32_t>, 8> light_pnt_shadow_active{};
		alignas(16) std::array<SourceUniformValue<float>, 8> light_pnt_shadow_bias{};
		alignas(16) int32_t shader{};
		alignas(16) glm::vec2 mat_texDimension{};
		alignas(16) int32_t mat_texInterpolate{};
		alignas(16) glm::vec2 mat_texScale{};
		alignas(16) glm::vec2 mat_texShift{};
		alignas(16) int32_t mat_flip{};
		alignas(16) int32_t mat_defer_normal{};
		alignas(16) float mat_normal_strength{};
		alignas(16) float mat_diffuse{};
		alignas(16) float mat_specular{};
		alignas(16) float mat_shine{};
		alignas(16) int32_t mat_metalic{};
		alignas(16) float mat_reflective{};
		alignas(16) glm::vec2 mat_pbr_metalic{};
		alignas(16) glm::vec2 mat_pbr_roughness{};
		alignas(16) int32_t mat_pbr_metalic_use_map{};
		alignas(16) int32_t mat_pbr_roughness_use_map{};
		alignas(16) glm::vec4 obj_color{};
		alignas(16) glm::vec3 cameraPosition{};
		alignas(16) int32_t gammaCorrection{};
		alignas(16) float alphaThreshold{};
		alignas(16) int32_t env_use_mapping{};
		alignas(16) glm::vec2 env_map_dimension{};
		alignas(16) glm::mat4 viewProjMat{};
		alignas(16) int32_t show_wireframe{};
		alignas(16) int32_t wireframe_aa{};
		alignas(16) int32_t wireframe_shade{};
		alignas(16) int32_t wireframe_only{};
		alignas(16) float wireframe_width{};
		alignas(16) glm::vec4 wireframe_color{};
		alignas(16) glm::vec4 backface_blending{};
		alignas(16) int32_t NativePass{};
		alignas(16) int32_t UseMaterialNormal{};
	};
	static_assert(sizeof(SourceCameraUniforms) == 6672);
}
