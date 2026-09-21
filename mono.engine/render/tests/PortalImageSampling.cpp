#include "PortalImageSampling.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/gtc/matrix_transform.hpp>

TEST_SUITE_ID("engine.render.portalimagesampling")

TEST_CASE(
	"imported portal sampling follows the eye ray through either slab face", "[render][portal-sampling]"
) {
	const float angle = GENERATE(0.f, .8f, 2.3f);
	const float side = GENERATE(-1.f, 1.f);
	const float scale = GENERATE(.25f, 1.f, 3.f);
	const auto rotation = glm::rotate(glm::mat4{1}, angle, glm::vec3{0, 1, 0});
	const glm::vec3 centre{2, 3, -4};
	const glm::vec3 normal = glm::vec3(rotation * glm::vec4{0, 0, 1, 0});
	const glm::vec3 eye = centre + glm::vec3(rotation * glm::vec4{1, 2, side * 8, 0}) * scale;
	const glm::vec3 mouth = centre + glm::vec3(rotation * glm::vec4{.5f, -.7f, 0, 0}) * scale;
	const glm::vec3 captureEye = eye + glm::vec3(rotation * glm::vec4{.7f, .3f, side * 2, 0}) * scale;
	const auto sampling =
		glm::perspective(1.f, 1.f, .1f, 100.f) * glm::lookAt(captureEye, centre, glm::vec3{0, 1, 0});
	const auto vector = [](glm::vec3 value) { return engine::core::Vector3{value.x, value.y, value.z}; };
	const auto projected =
		engine::render::PortalImageSampling(sampling, vector(eye), vector(centre), vector(normal));
	const glm::vec4 expected = sampling * glm::vec4(mouth, 1);
	for (float fraction : {.95f, 1.f, 1.05f}) {
		const glm::vec3 face = eye + (mouth - eye) * fraction;
		const glm::vec4 actual = projected * glm::vec4(face, 1);
		REQUIRE(actual.w > 0);
		CHECK(glm::length(glm::vec3(actual) / actual.w - glm::vec3(expected) / expected.w) < .00001f);
	}
	const auto onPlane =
		engine::render::PortalImageSampling(sampling, vector(centre), vector(centre), vector(normal));
	CHECK(onPlane == sampling);
}
