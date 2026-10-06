#include <engine/bake/ComposerModel.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraphexport/GraphMeshHost.hpp>
#include <engine/imagegraphexport/Runner.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &invocation, std::string_view port) {
			const auto found =
				std::find_if(invocation.Inputs.begin(), invocation.Inputs.end(), [&](const auto &value) {
					return value.Port == port;
				});
			return found == invocation.Inputs.end() ? nullptr : &found->Data;
		}
		template <class T> const T *Get(const HostNodeInvocation &invocation, std::string_view port) {
			const auto *value = Input(invocation, port);
			return value ? std::get_if<T>(value) : nullptr;
		}
		bool Fail(std::string &failure, std::string reason) {
			failure = "mesh host: " + std::move(reason);
			return false;
		}
		glm::dvec3 Point(Vector3 value) {
			return {value.X, value.Y, value.Z};
		}
		Vector3 Point(glm::dvec3 value) {
			return {value.x, value.y, value.z};
		}
		bool Finite(glm::dvec3 value) {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}
		struct ObjIndexBudget {
			uint64_t Maximum = 0, Used = 4096;
			bool Spend(uint64_t bytes) {
				if (Used > Maximum || bytes > Maximum - Used) return false;
				Used += bytes;
				return true;
			}
		};
		struct ObjIndices {
			std::unordered_map<std::string, uint32_t> Records;
			bool Reserve(size_t count, ObjIndexBudget &budget) {
				// grug reserve the bounded buckets before inserting first-seen source records.
				const uint64_t quote = (2 * uint64_t{count} + 8) * 4 * sizeof(void *);
				if (!budget.Spend(quote)) return false;
				Records.reserve(count);
				return Records.bucket_count() <= 2 * count + 8;
			}
			bool Intern(std::string record, ObjIndexBudget &budget, uint32_t &index, bool &added) {
				const auto found = Records.find(record);
				if (found != Records.end()) {
					index = found->second;
					added = false;
					return true;
				}
				// grug charge key capacity and node links beside the already reserved bucket table.
				const uint64_t node = sizeof(decltype(Records)::value_type) + 4 * sizeof(void *);
				if (!budget.Spend(record.capacity() + 1 + node)) return false;
				index = static_cast<uint32_t>(Records.size() + 1);
				Records.emplace(std::move(record), index);
				added = true;
				return true;
			}
		};
		bool Import(
			const HostNodeInvocation &invocation,
			const GraphFileGrant &grant,
			HostNodeCapture &capture,
			std::string &failure
		) {
			std::error_code error;
			const auto size = std::filesystem::file_size(grant.File, error);
			if (error || size > 8 * 1024 * 1024 || size > invocation.MaximumOperationBytes / 4)
				return Fail(failure, "import source exceeds its byte budget");
			std::vector<std::byte> bytes(static_cast<size_t>(size));
			std::ifstream stream(grant.File, std::ios::binary);
			stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!stream || static_cast<size_t>(stream.gcount()) != bytes.size())
				return Fail(failure, "cannot read complete import source");
			const auto *scale = Get<double>(invocation, "import_scale");
			const auto *axis = Get<EnumValue>(invocation, "axis");
			const auto *flip = Get<bool>(invocation, "flip_uv");
			const auto *position = Get<Vector3>(invocation, "position"),
					   *anchor = Get<Vector3>(invocation, "anchor"),
					   *localScale = Get<Vector3>(invocation, "scale");
			const auto *rotation = Get<Quaternion>(invocation, "rotation");
			if (!scale || !axis || !flip || !position || !anchor || !localScale || !rotation ||
				axis->Value < 0 || axis->Value > 2)
				return Fail(failure, "import controls have invalid types or axis");
			engine::bake::ComposerModel imported;
			const bool obj = invocation.Authored.Type == "pc.3_d_mesh_obj";
			if (!(obj ? engine::bake::ReadComposerObj(
							bytes,
							*scale,
							static_cast<uint8_t>(axis->Value),
							imported,
							failure,
							invocation.MaximumOperationBytes / 4
						)
					  : engine::bake::ReadComposerElementJson(
							bytes,
							*scale,
							static_cast<uint8_t>(axis->Value),
							imported,
							failure,
							invocation.MaximumOperationBytes / 4
						)))
				return false;
			MeshValue3D mesh;
			auto &data = mesh.Data.emplace();
			data.LocalTransforms.push_back({*position, *anchor, *rotation, *localScale});
			uint64_t retained = 0;
			for (size_t index = 0; index < imported.Parts.size(); index++) {
				const auto &part = imported.Parts[index];
				MeshPart3D converted;
				converted.LocalMatrix = part.LocalMatrix;
				converted.MaterialIndex = static_cast<uint32_t>(data.Materials.size());
				converted.Vertices.reserve(part.Vertices.size());
				for (const auto &vertex : part.Vertices)
					converted.Vertices.push_back(
						{{vertex.Position[0], vertex.Position[1], vertex.Position[2]},
						 {vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]},
						 {vertex.TexCoord[0], *flip ? 1.0 - vertex.TexCoord[1] : vertex.TexCoord[1]},
						 {255, 255, 255, 255}}
					);
				MaterialValue3D material;
				const auto *named = Get<MaterialValue3D>(invocation, part.Material);
				if (!named && index < invocation.Authored.DynamicInputs.size())
					named = Get<MaterialValue3D>(invocation, invocation.Authored.DynamicInputs[index].Id);
				if (named) material = *named;
				const auto &m = material.Get();
				for (const auto *surface : {&m.Surface, &m.Normal, &m.PropertiesMap})
					if (*surface) retained += (*surface)->Pixels.size();
				retained += converted.Vertices.size() * sizeof(MeshVertex3D) + sizeof(MeshPart3D) +
							sizeof(MaterialData3D);
				if (retained > invocation.MaximumOperationBytes / 2)
					return Fail(failure, "converted mesh exceeds its operation budget");
				data.Materials.push_back(std::move(material));
				data.Parts.push_back(std::move(converted));
			}
			data.Edges.reserve(imported.Edges.size());
			for (const auto &edge : imported.Edges)
				data.Edges.push_back(
					{{edge[0][0], edge[0][1], edge[0][2]}, {edge[1][0], edge[1][1], edge[1][2]}}
				);
			capture.Outputs.push_back({"mesh", std::move(mesh)});
			return true;
		}
		struct File {
			std::filesystem::path Target;
			std::string Text;
			const Image *Surface = nullptr;
		};
		bool Publish(const std::vector<File> &files, std::string &failure) {
			if (files.empty() || files.size() > 4096)
				return Fail(failure, "export file count exceeds its bounds");
			const auto parent = files[0].Target.parent_path();
			std::error_code error;
			static std::atomic<uint64_t> sequence{0};
			const auto directory =
				parent / (".graph-mesh-" +
						  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
						  std::to_string(sequence.fetch_add(1)));
			if (!std::filesystem::create_directory(directory, error) || error)
				return Fail(failure, "cannot create exclusive export staging directory");
			struct Cleanup {
				std::filesystem::path Path;
				~Cleanup() {
					std::error_code e;
					if (!Path.empty()) std::filesystem::remove_all(Path, e);
				}
			} cleanup{directory};
			std::vector<std::filesystem::path> staged;
			for (size_t index = 0; index < files.size(); index++) {
				const auto &file = files[index];
				if (file.Target.parent_path() != parent)
					return Fail(failure, "sidecar escapes its granted export directory");
				const auto path = directory / (std::to_string(index) + (file.Surface ? ".png" : ".txt"));
				if (file.Surface) {
					if (!engine::imagegraphexport::runner::WriteStillImage(path, *file.Surface, failure))
						return false;
				} else {
					std::ofstream stream(path, std::ios::binary);
					stream.write(file.Text.data(), static_cast<std::streamsize>(file.Text.size()));
					stream.close();
					if (!stream) return Fail(failure, "cannot write complete staged mesh export");
				}
				staged.push_back(path);
			}
			std::vector<std::filesystem::path> backups;
			backups.reserve(files.size());
			for (size_t index = 0; index < files.size(); index++)
				backups.push_back(directory / ("previous" + std::to_string(index)));
			std::vector<bool> backed(files.size(), false), published(files.size(), false);
			// grug prepare paths before first rename and restore on every failed exit.
			const auto restore = [&]() noexcept {
				bool failed = false;
				for (size_t index = files.size(); index > 0; index--) {
					const auto i = index - 1;
					try {
						std::error_code rollback;
						if (published[i]) {
							std::filesystem::remove(files[i].Target, rollback);
							failed |= bool(rollback);
						}
						if (backed[i]) {
							std::filesystem::rename(backups[i], files[i].Target, rollback);
							failed |= bool(rollback);
						}
					} catch (...) {
						failed = true;
					}
				}
				if (failed) {
					cleanup.Path.clear();
					try {
						failure += "; prior export recovery remains at " + directory.string();
					} catch (...) {}
				}
			};
			struct Rollback {
				const decltype(restore) &Restore;
				bool Active = true;
				~Rollback() {
					if (Active) Restore();
				}
			} rollback{restore};
			for (size_t index = 0; index < files.size(); index++) {
				const auto status = std::filesystem::symlink_status(files[index].Target, error);
				if (error && error != std::errc::no_such_file_or_directory) {
					failure = "mesh host: cannot inspect prior export target";
					return false;
				}
				error.clear();
				if (std::filesystem::exists(status)) {
					if (status.type() != std::filesystem::file_type::regular) {
						failure = "mesh host: prior export target is not a regular file";
						return false;
					}
					std::filesystem::rename(files[index].Target, backups[index], error);
					if (error) {
						failure = "mesh host: cannot stage prior export target";
						return false;
					}
					backed[index] = true;
				}
				std::filesystem::rename(staged[index], files[index].Target, error);
				if (error) {
					failure = "mesh host: cannot publish complete export set";
					return false;
				}
				published[index] = true;
			}
			rollback.Active = false;
			return true;
		}
		bool Export(const HostNodeInvocation &invocation, const GraphFileGrant &grant, std::string &failure) {
			ENGINE_PROFILE("imagegraphexport.mesh.obj");
			const auto *mesh = Get<MeshValue3D>(invocation, "mesh");
			const auto *textures = Get<bool>(invocation, "export_texture"),
					   *invertUv = Get<bool>(invocation, "invert_uv"),
					   *invertAxis = Get<bool>(invocation, "invert_yz_axis"),
					   *apply = Get<bool>(invocation, "apply_transform");
			if (!mesh || !mesh->Data || !textures || !invertUv || !invertAxis || !apply ||
				mesh->Data->LocalTransforms.empty() || mesh->Data->LocalTransforms.size() > 64)
				return Fail(failure, "export mesh or typed controls are invalid");
			glm::dvec3 position{}, anchor{}, scale{1};
			glm::dquat rotation{1, 0, 0, 0};
			for (auto iterator = mesh->Data->LocalTransforms.rbegin();
				 iterator != mesh->Data->LocalTransforms.rend();
				 iterator++) {
				position += Point(iterator->Position);
				anchor += Point(iterator->Anchor);
				scale *= Point(iterator->Scale);
				const auto &q = iterator->Rotation;
				rotation = rotation * glm::dquat{q.W, q.X, q.Y, q.Z};
			}
			if (!Finite(position) || !Finite(anchor) || !Finite(scale) ||
				!std::isfinite(glm::dot(rotation, rotation)) || glm::dot(rotation, rotation) <= 0)
				return Fail(failure, "export transform is not finite");
			rotation = glm::normalize(rotation);
			const auto base = std::filesystem::absolute(grant.File).lexically_normal();
			size_t boundedVertices = 0;
			if (mesh->Data->Parts.size() > 2048) return Fail(failure, "export part count exceeds its bounds");
			for (const auto &part : mesh->Data->Parts) {
				if (part.Vertices.size() > 65536 - boundedVertices)
					return Fail(failure, "export vertex count exceeds its bounds");
				boundedVertices += part.Vertices.size();
			}
			// Fixed-point double text may need hundreds of digits per component.
			if (boundedVertices > invocation.MaximumOperationBytes / 4096)
				return Fail(failure, "export text working set exceeds its operation budget");
			if (!boundedVertices) return Fail(failure, "export mesh has no triangles");
			ObjIndexBudget indexBudget{invocation.MaximumOperationBytes / 4};
			ObjIndices positions, normals, coordinates;
			if (!positions.Reserve(boundedVertices, indexBudget) ||
				!normals.Reserve(boundedVertices, indexBudget) ||
				!coordinates.Reserve(boundedVertices, indexBudget))
				return Fail(failure, "OBJ index storage exceeds its operation budget");
			std::ostringstream obj, mtl;
			obj.imbue(std::locale::classic());
			obj << std::fixed << std::setprecision(5);
			obj << "# Pixel Composer\n";
			mtl << "# Pixel Composer\n";
			const auto mtlPath = base.parent_path() / (base.stem().string() + ".mtl");
			if (*textures) obj << "mtllib " << mtlPath.filename().string() << "\n";
			std::vector<File> files;
			size_t vertexCount = 0;
			uint64_t textureBytes = 0;
			for (size_t partIndex = 0; partIndex < mesh->Data->Parts.size(); partIndex++) {
				const auto &part = mesh->Data->Parts[partIndex];
				if (part.Vertices.size() % 3 || part.Vertices.size() > 65536 - vertexCount ||
					part.MaterialIndex >= mesh->Data->Materials.size())
					return Fail(failure, "export part vertices or material index are invalid");
				obj << "o shape" << partIndex << "\n";
				if (*textures) {
					const auto name = "mat." + std::to_string(partIndex + 1);
					obj << "usemtl " << name << "\n";
					mtl << "newmtl " << name
						<< "\nKd 1.00 1.00 1.00\nKs 0.00 0.00 0.00\nNs 0.00\nNi 1.00\nd 1.00\nillum 0\n";
					const auto &surface = mesh->Data->Materials[part.MaterialIndex].Get().Surface;
					if (surface) {
						if (surface->Pixels.size() > invocation.MaximumOperationBytes / 4 - textureBytes ||
							!ValidSurfaceLayout(
								*surface, invocation.Request.MaximumImageDimension, Limits::MaximumOutputBytes
							) ||
							!FiniteSurfaceSamples(*surface))
							return Fail(failure, "export material surface exceeds its budget");
						textureBytes += surface->Pixels.size();
						const auto path = base.parent_path() / (base.stem().string() + "_texture" +
																std::to_string(partIndex + 1) + ".png");
						files.push_back({path, {}, &*surface});
						mtl << "map_Kd " << path.filename().string() << "\n";
					}
				}
				std::array<std::array<uint32_t, 3>, 3> corners{};
				for (size_t vertexIndex = 0; vertexIndex < part.Vertices.size(); ++vertexIndex) {
					const auto &vertex = part.Vertices[vertexIndex];
					auto p = Point(vertex.Position), n = Point(vertex.Normal);
					if (*apply) {
						p = rotation * ((p - anchor) * scale) + position;
						n = rotation * n;
					}
					if (*invertAxis) {
						std::swap(p.y, p.z);
						std::swap(n.y, n.z);
					}
					if (!Finite(p) || !Finite(n) || !std::isfinite(vertex.UV.X) ||
						!std::isfinite(vertex.UV.Y))
						return Fail(failure, "export vertex is not finite");
					auto &indices = corners[vertexIndex % 3];
					const auto emit = [&](ObjIndices &table, std::string record, uint32_t &index) {
						if (record.size() > 1024) return false;
						bool added = false;
						if (!table.Intern(record, indexBudget, index, added)) return false;
						if (added) {
							const uint64_t textBytes =
								static_cast<uint64_t>(obj.tellp()) + static_cast<uint64_t>(mtl.tellp());
							if (textBytes > invocation.MaximumOperationBytes / 4 ||
								record.size() + 1 > invocation.MaximumOperationBytes / 4 - textBytes)
								return false;
							obj << record << '\n';
						}
						return true;
					};
					std::ostringstream record;
					record.imbue(std::locale::classic());
					record << std::fixed << std::setprecision(5);
					record << "v " << p.x << ' ' << p.y << ' ' << p.z;
					if (!emit(positions, record.str(), indices[0]))
						return Fail(failure, "OBJ position records exceed byte budget");
					record.str({});
					record << "vn " << n.x << ' ' << n.y << ' ' << n.z;
					if (!emit(normals, record.str(), indices[2]))
						return Fail(failure, "OBJ normal records exceed byte budget");
					record.str({});
					record << "vt " << vertex.UV.X << ' ' << (*invertUv ? 1 - vertex.UV.Y : vertex.UV.Y);
					if (!emit(coordinates, record.str(), indices[1]))
						return Fail(failure, "OBJ UV records exceed byte budget");
					if (vertexIndex % 3 == 2) {
						const uint64_t textBytes =
							static_cast<uint64_t>(obj.tellp()) + static_cast<uint64_t>(mtl.tellp());
						if (textBytes > invocation.MaximumOperationBytes / 4 ||
							64 > invocation.MaximumOperationBytes / 4 - textBytes)
							return Fail(failure, "OBJ face records exceed byte budget");
						obj << "f";
						for (const auto &corner : corners)
							obj << ' ' << corner[0] << '/' << corner[1] << '/' << corner[2];
						obj << '\n';
					}
				}
				vertexCount += part.Vertices.size();
				if (static_cast<uint64_t>(obj.tellp()) + static_cast<uint64_t>(mtl.tellp()) >
					invocation.MaximumOperationBytes / 4)
					return Fail(failure, "serialized mesh exceeds its byte budget");
			}
			core::Metrics::Count("imagegraphexport.mesh.obj_vertices", vertexCount);
			core::Metrics::Count("imagegraphexport.mesh.obj_positions", positions.Records.size());
			core::Metrics::Count("imagegraphexport.mesh.obj_normals", normals.Records.size());
			core::Metrics::Count("imagegraphexport.mesh.obj_uvs", coordinates.Records.size());
			core::Metrics::Count(
				"imagegraphexport.mesh.obj_payload_bytes", static_cast<uint64_t>(obj.tellp())
			);
			files.push_back({base, obj.str(), nullptr});
			if (*textures) files.push_back({mtlPath, mtl.str(), nullptr});
			return Publish(files, failure);
		}
	}
	bool CaptureGraphMeshFile(
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		const HostNodeInvocation &invocation,
		HostNodeCapture &output,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraphexport.mesh.capture");
		const auto &type = invocation.Authored.Type;
		const bool write = type == "pc.3_d_mesh_export";
		if (!write && type != "pc.3_d_mesh_obj" && type != "pc.3_d_mesh_json")
			return Fail(failure, "unsupported model capability node");
		const auto *path = Get<std::string>(invocation, write ? "paths" : "file_path");
		const GraphFileGrant *grant = nullptr;
		for (const auto &candidate : grants)
			if (candidate.NodeId == invocation.Authored.Id && candidate.Resource.empty()) {
				if (grant) return Fail(failure, "node grants are duplicated");
				grant = &candidate;
			}
		if (!path || !grant || *path != grant->File.string() || write != grant->Write ||
			!policy.AllowsName(*path))
			return Fail(failure, "model requires an exact node, path and operation grant");
		const uint64_t maximum = std::min(invocation.MaximumOperationBytes, Limits::MaximumEvaluationBytes);
		const auto prior = HostCaptureRetainedPayloadBytes(output);
		if (!prior || *prior > maximum) return Fail(failure, "prior model receipt exceeds operation budget");
		HostNodeCapture capture;
		uint64_t retained = 0;
		Diagnostic diagnostic;
		if (PrepareResolvedHostCapture(invocation, maximum - *prior, capture, retained, diagnostic) !=
			Status::Ok)
			return Fail(failure, diagnostic.Message);
		auto operation = invocation;
		if (retained > (maximum - *prior) / 2)
			return Fail(failure, "model receipt coexistence exceeds operation budget");
		// grug keep borrowed controls, admitted receipt and prior receipt outside encoding workspace.
		operation.MaximumOperationBytes = maximum - *prior - 2 * retained;
		if (!(write ? Export(operation, *grant, failure) : Import(operation, *grant, capture, failure)))
			return false;
		output = std::move(capture);
		return true;
	} catch (const std::bad_alloc &) {
		if (failure.find("; prior export recovery remains at ") != std::string::npos) return false;
		return Fail(failure, "model allocation failed");
	} catch (const std::length_error &) {
		if (failure.find("; prior export recovery remains at ") != std::string::npos) return false;
		return Fail(failure, "model allocation exceeds length bound");
	}
}
