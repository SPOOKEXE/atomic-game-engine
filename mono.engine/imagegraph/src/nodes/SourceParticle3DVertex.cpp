#include "../MeshPayload.hpp"

#include <engine/imagegraph/SourceParticle3DVertex.hpp>

#include <cmath>
#include <numbers>

namespace engine::imagegraph {
	namespace source_particle3d_vertex {
		using Triple = std::array<float, 3>;
		using Matrix = std::array<float, 16>;
		constexpr Matrix IDENTITY{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		bool Finite(Triple value) {
			return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
		}
		Triple Narrow(Vector3 value) {
			return {float(value.X), float(value.Y), float(value.Z)};
		}
		float Length(Triple value) {
			return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
		}
		Triple Cross(Triple left, Triple right) {
			return {
				left[1] * right[2] - left[2] * right[1],
				left[2] * right[0] - left[0] * right[2],
				left[0] * right[1] - left[1] * right[0]
			};
		}
		bool Normalize(Triple input, Triple &output) {
			const float length = Length(input);
			if (!std::isfinite(length) || length == 0) return false;
			output = {input[0] / length, input[1] / length, input[2] / length};
			return Finite(output);
		}
		bool LookAt(Triple target, Matrix &matrix) {
			Triple forward;
			if (!Normalize(target, forward)) return false;
			Triple right = Cross({0, 0, 1}, forward);
			if (Length(right) < .0001f) {
				matrix = IDENTITY;
				return true;
			}
			if (!Normalize(right, right)) return false;
			const Triple up = Cross(forward, right);
			matrix = {
				right[0],
				up[0],
				forward[0],
				0,
				right[1],
				up[1],
				forward[1],
				0,
				right[2],
				up[2],
				forward[2],
				0,
				0,
				0,
				0,
				1
			};
			return true;
		}
		Triple Multiply(const Matrix &matrix, Triple value, float w) {
			return {
				matrix[0] * value[0] + matrix[1] * value[1] + matrix[2] * value[2] + matrix[3] * w,
				matrix[4] * value[0] + matrix[5] * value[1] + matrix[6] * value[2] + matrix[7] * w,
				matrix[8] * value[0] + matrix[9] * value[1] + matrix[10] * value[2] + matrix[11] * w
			};
		}
		Matrix Euler(Triple degrees) {
			Triple cosine, sine;
			for (size_t axis = 0; axis < 3; ++axis) {
				const float radians = degrees[axis] * (std::numbers::pi_v<float> / 180.f);
				cosine[axis] = std::cos(radians);
				sine[axis] = std::sin(radians);
			}
			const auto [cx, cy, cz] = cosine;
			const auto [sx, sy, sz] = sine;
			return {
				cy * cz,
				-cx * sz + sx * sy * cz,
				sx * sz + cx * sy * cz,
				0,
				cy * sz,
				cx * cz + sx * sy * sz,
				-sx * cz + cx * sy * sz,
				0,
				-sy,
				sx * cy,
				cx * cy,
				0,
				0,
				0,
				0,
				1
			};
		}
	}
	bool PrepareSourceParticle3DVertex(
		const MeshVertex3D &vertex,
		const MeshInstance3D &transform,
		const ParticleRecord3D &particle,
		const std::array<double, 16> &objectTransform,
		Vector3 cameraPosition,
		SourceParticle3DVertex &output
	) {
		using namespace source_particle3d_vertex;
		using detail::MeshFinite;
		if (!ValidParticleRecord3D(particle) || !MeshFinite(vertex.Position) || !MeshFinite(vertex.Normal) ||
			!MeshFinite(vertex.UV) || !MeshFinite(cameraPosition))
			return false;
		for (float word : transform.Fields)
			if (!std::isfinite(word)) return false;
		Matrix object;
		for (size_t word = 0; word < object.size(); ++word) {
			object[word] = float(objectTransform[word]);
			if (!std::isfinite(object[word])) return false;
		}
		SourceParticle3DVertex prepared;
		if (particle.Active == 0) {
			output = prepared;
			return true;
		}
		Triple position = Narrow(vertex.Position), normal = Narrow(vertex.Normal);
		if (!Finite(position) || !Finite(normal) || !std::isfinite(float(vertex.UV.X)) ||
			!std::isfinite(float(vertex.UV.Y)))
			return false;
		bool billboard;
		if (!ParticleBillboard3D(particle, billboard)) return false;
		if (billboard) {
			Matrix facing;
			if (!LookAt(Narrow(cameraPosition), facing)) return false;
			position = Multiply(facing, position, 1);
			normal = Multiply(facing, normal, 0);
		}
		const auto &fields = transform.Fields;
		const Matrix rotation = Euler({fields[4], fields[5], fields[6]});
		position = Multiply(object, position, 1);
		position = Multiply(rotation, position, 1);
		const Triple upNormal{fields[12], fields[13], fields[14]};
		const float upLength = Length(upNormal);
		if (!std::isfinite(upLength)) return false;
		if (upLength > 0) {
			Triple normalized;
			Matrix facing;
			if (!Normalize(upNormal, normalized) || !LookAt(normalized, facing)) return false;
			position = Multiply(facing, position, 1);
		}
		for (size_t axis = 0; axis < 3; ++axis)
			position[axis] = position[axis] * fields[8 + axis] + fields[axis];
		// Source omits object, instance scale and up-normal orientation from the normal path.
		normal = Multiply(rotation, normal, 0);
		if (!Finite(position) || !Finite(normal)) return false;
		prepared.Position = {position[0], position[1], position[2]};
		prepared.Normal = {normal[0], normal[1], normal[2]};
		prepared.UV = {float(vertex.UV.X), float(vertex.UV.Y)};
		const std::array<float, 4> vertexColour{
			vertex.Tint.Red / 255.f,
			vertex.Tint.Green / 255.f,
			vertex.Tint.Blue / 255.f,
			vertex.Tint.Alpha / 255.f
		};
		for (size_t channel = 0; channel < 4; ++channel) {
			prepared.Colour[channel] = vertexColour[channel] * particle.Colour[channel];
			if (!std::isfinite(prepared.Colour[channel])) return false;
		}
		prepared.Active = true;
		output = prepared;
		return true;
	}
}
