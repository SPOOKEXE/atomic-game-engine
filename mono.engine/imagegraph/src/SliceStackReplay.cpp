#include "EvaluationBudget.hpp"
#include "MeshPayload.hpp"
#include "SourceGradient.hpp"

#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/SliceStackReplay.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
namespace engine::imagegraph {
	uint64_t RetainedSliceStackReplayBytes(const SliceStackReplayState &state) {
		uint64_t bytes = state.Entries.capacity() * sizeof(SliceStackReplayEntry);
		for (const auto &entry : state.Entries) {
			bytes = detail::MeshAddBytes(bytes, entry.NodeId.capacity());
			bytes = detail::MeshAddBytes(bytes, entry.Faces.capacity() * sizeof(SliceStackFace));
			bytes = detail::MeshAddBytes(bytes, entry.Images.capacity() * sizeof(Image));
			for (const auto &image : entry.Images)
				bytes = detail::MeshAddBytes(bytes, image.Pixels.capacity());
		}
		return bytes;
	}
	Status ValidateSliceStackReplay(
		const SliceStackReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	) {
		auto fail = [&](Status code, std::string message, std::string node = {}) {
			diagnostic = {code, std::move(node), {}, std::move(message)};
			return code;
		};
		if (state.Entries.size() > Limits::MaximumNodes ||
			RetainedSliceStackReplayBytes(state) > maximumBytes)
			return fail(Status::LimitExceeded, "slice replay owner exceeds caps");
		for (size_t index = 0; index < state.Entries.size(); ++index) {
			const auto &entry = state.Entries[index];
			if (entry.NodeId.empty() || entry.NodeId.size() > Limits::MaximumTextBytes ||
				!ValidFrameTime(entry.ActionTime) || !detail::MeshFinite(entry.Minimum) ||
				!detail::MeshFinite(entry.Maximum) || !detail::MeshFinite(entry.Padding) ||
				entry.Width == 0 || entry.Height == 0 || entry.Width > Limits::MaximumDimension ||
				entry.Height > Limits::MaximumDimension || entry.Slices == 0 ||
				entry.Slices > Limits::MaximumArrayElements || entry.Images.size() != entry.Slices ||
				entry.Faces.size() > Limits::MaximumArrayElements || entry.Slice > entry.Slices ||
				entry.Pixel >= uint64_t(entry.Width) * entry.Height ||
				((entry.Slice == entry.Slices) && entry.Active))
				return fail(Status::InvalidValue, "slice replay entry is malformed", entry.NodeId);
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (state.Entries[earlier].NodeId == entry.NodeId)
					return fail(Status::DuplicateId, "slice replay identity is duplicated", entry.NodeId);
			for (size_t faceIndex = 0; faceIndex < entry.Faces.size(); ++faceIndex) {
				const auto &face = entry.Faces[faceIndex];
				if (face.MaximumX != std::max({face.Points[0].X, face.Points[1].X, face.Points[2].X}) ||
					(faceIndex && entry.Faces[faceIndex - 1].MaximumX < face.MaximumX))
					return fail(
						Status::InvalidValue, "slice face order or bounds are malformed", entry.NodeId
					);
				if (!detail::MeshFinite(face.Center) || !std::isfinite(face.MaximumX))
					return fail(Status::InvalidValue, "slice face is nonfinite", entry.NodeId);
				for (auto point : face.Points)
					if (!detail::MeshFinite(point))
						return fail(Status::InvalidValue, "slice face is nonfinite", entry.NodeId);
			}
			for (const auto &image : entry.Images)
				if (image.Width != entry.Width || image.Height != entry.Height ||
					image.Format != SurfaceFormat::RGBA8Unorm ||
					!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
					return fail(Status::InvalidValue, "slice replay image is malformed", entry.NodeId);
		}
		diagnostic = {};
		return Status::Ok;
	}
	Status AdvanceSliceStack(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		uint32_t workPixels,
		SliceStackReplayState &state,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		ENGINE_PROFILE("imagegraph.slice_stack.advance");
		auto fail = [&](Status code, std::string message) {
			diagnostic = {code, std::string(nodeId), {}, std::move(message)};
			return code;
		};
		if (workPixels > Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "slice work budget exceeds pixel cap");
		if (ValidateSliceStackReplay(state, maximumBytes, diagnostic) != Status::Ok) return diagnostic.Code;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
			return n.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.3_d_mesh_stack_slice")
			return fail(Status::InvalidValue, "slice action requires a Slice Stack node");
		const FrameTime time = GetFrameTime(request);
		const auto action = std::find_if(
			document.SliceStackActions.begin(), document.SliceStackActions.end(), [&](const auto &a) {
				return a.NodeId == nodeId && a.Time == time;
			}
		);
		const auto previous = std::find_if(state.Entries.begin(), state.Entries.end(), [&](const auto &e) {
			return e.NodeId == nodeId;
		});
		const bool start =
			action != document.SliceStackActions.end() &&
			(previous == state.Entries.end() || (!previous->Active && previous->ActionTime != time));
		if (!start && (previous == state.Entries.end() || !previous->Active)) {
			diagnostic = {};
			return Status::Ok;
		}
		detail::EvaluationBudget ledger(maximumBytes);
		auto retained = ledger.Reserve(RetainedSliceStackReplayBytes(state));
		if (!retained) return fail(Status::LimitExceeded, "slice owner exceeds evaluation budget");
		EvaluationSnapshot snapshot;
		detail::AllocationReservation snapshotCharge;
		uint64_t candidateBytes =
			RetainedSliceStackReplayBytes(state) + sizeof(SliceStackReplayEntry) + nodeId.size();
		uint32_t width = 0, height = 0, slices = 0;
		Vector3 padding{};
		const MeshValue3D *mesh = nullptr;
		if (start) {
			EvaluationRequest capture = request;
			capture.SliceStackReplay = &state;
			auto status =
				EvaluateNodeInputs(document, plan, nodeId, capture, snapshot, diagnostic, ledger.Available());
			if (status != Status::Ok) return status;
			auto charged = ledger.Reserve(snapshot.RetainedBytes());
			if (!charged) return fail(Status::LimitExceeded, "slice inputs exceed evaluation budget");
			snapshotCharge = std::move(*charged);
			Vector2 dimensions{16, 16};
			int64_t count = 4;
			for (const auto &input : snapshot.Values()) {
				if (input.Port == "mesh")
					mesh = std::get_if<MeshValue3D>(&input.Data);
				else if (input.Port == "output_dimension") {
					const auto *d = std::get_if<Vector2>(&input.Data);
					if (!d) return fail(Status::TypeMismatch, "slice dimensions require Vector2");
					dimensions = *d;
				} else if (input.Port == "bbox_padding") {
					const auto *p = std::get_if<Vector3>(&input.Data);
					if (!p) return fail(Status::TypeMismatch, "slice padding requires Vector3");
					padding = *p;
				} else if (input.Port == "slices") {
					if (const auto *c = std::get_if<int64_t>(&input.Data))
						count = *c;
					else
						return fail(Status::TypeMismatch, "slice count requires integer");
				}
			}
			if (!mesh || !mesh->Data) {
				diagnostic = {};
				return Status::Ok;
			}
			if (!detail::ValidMeshPayload(*mesh) || !detail::MeshFinite(padding) ||
				!detail::MeshFinite(dimensions) || dimensions.X < 1 || dimensions.Y < 1 ||
				dimensions.X > request.MaximumImageDimension ||
				dimensions.Y > request.MaximumImageDimension || count < 1 ||
				count > int64_t(Limits::MaximumArrayElements))
				return fail(Status::InvalidValue, "slice geometry or dimensions are invalid");
			width = uint32_t(detail::SourceRoundEven(dimensions.X));
			height = uint32_t(detail::SourceRoundEven(dimensions.Y));
			slices = uint32_t(count);
			size_t vertices = 0;
			for (const auto &part : mesh->Data->Parts)
				vertices += part.Vertices.size();
			const uint64_t pixelBytes = uint64_t(width) * height * slices * 4;
			if (pixelBytes > Limits::MaximumArrayBytes)
				return fail(Status::LimitExceeded, "slice images exceed array byte cap");
			candidateBytes = detail::MeshAddBytes(
				candidateBytes,
				(vertices / 3 + vertices / 6) * sizeof(SliceStackFace) + slices * sizeof(Image) + pixelBytes
			);
		}
		auto candidateCharge = ledger.Reserve(candidateBytes);
		if (!candidateCharge) return fail(Status::LimitExceeded, "slice candidate exceeds evaluation budget");
		SliceStackReplayState candidate = state;
		candidate.Entries.reserve(state.Entries.size() + (previous == state.Entries.end()));
		auto entry = std::find_if(candidate.Entries.begin(), candidate.Entries.end(), [&](const auto &e) {
			return e.NodeId == nodeId;
		});
		if (entry == candidate.Entries.end()) {
			candidate.Entries.emplace_back();
			entry = std::prev(candidate.Entries.end());
		}
		if (start) {
			*entry = {};
			entry->NodeId = nodeId;
			entry->ActionTime = time;
			entry->Active = true;
			entry->Width = width;
			entry->Height = height;
			entry->Slices = slices;
			entry->Padding = padding;
			entry->Minimum = {99999, 99999, 99999};
			entry->Maximum = {-99999, -99999, -99999};
			size_t count = 0;
			for (const auto &part : mesh->Data->Parts)
				count += part.Vertices.size() / 3;
			entry->Faces.reserve(count);
			for (const auto &part : mesh->Data->Parts) {
				const auto &material = mesh->Data->Materials[part.MaterialIndex].Get();
				for (size_t index = 0; index + 2 < part.Vertices.size(); index += 3) {
					SliceStackFace face;
					double u = 0, v = 0;
					for (size_t i = 0; i < 3; ++i) {
						const auto &vertex = part.Vertices[index + i];
						for (double coordinate :
							 {vertex.Position.X,
							  vertex.Position.Y,
							  vertex.Position.Z,
							  vertex.UV.X,
							  vertex.UV.Y})
							if (std::abs(coordinate) > std::numeric_limits<float>::max())
								return fail(Status::InvalidValue, "slice vertex exceeds source f32 range");
						auto p = Vector3{
							float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)
						};
						face.Points[i] = p;
						entry->Minimum = {
							std::min(entry->Minimum.X, p.X),
							std::min(entry->Minimum.Y, p.Y),
							std::min(entry->Minimum.Z, p.Z)
						};
						entry->Maximum = {
							std::max(entry->Maximum.X, p.X),
							std::max(entry->Maximum.Y, p.Y),
							std::max(entry->Maximum.Z, p.Z)
						};
						u += double(float(vertex.UV.X)) / 3;
						v += double(float(vertex.UV.Y)) / 3;
					}
					const auto [a, b, c] = face.Points;
					if (a.Z == b.Z && b.Z == c.Z) continue;
					face.MaximumX = std::max({a.X, b.X, c.X});
					face.Center = {(a.X + b.X + c.X) / 3, (a.Y + b.Y + c.Y) / 3, (a.Z + b.Z + c.Z) / 3};
					if (material.Surface) {
						u -= std::floor(u);
						v -= std::floor(v);
						auto rgba = detail::ReadPixel(
							*material.Surface,
							uint32_t(detail::SourceRoundEven(u * (material.Surface->Width - 1))),
							uint32_t(detail::SourceRoundEven(v * (material.Surface->Height - 1)))
						);
						face.Color = {
							detail::SourceColorByte(rgba[0] * 255),
							detail::SourceColorByte(rgba[1] * 255),
							detail::SourceColorByte(rgba[2] * 255),
							255
						};
					}
					entry->Faces.push_back(face);
				}
			}
			std::stable_sort(entry->Faces.begin(), entry->Faces.end(), [](const auto &a, const auto &b) {
				return a.MaximumX > b.MaximumX;
			});
			entry->Images.resize(slices);
			for (auto &image : entry->Images) {
				image.Width = width;
				image.Height = height;
				image.Pixels.resize(size_t(width) * height * 4);
			}
		}
		const auto extent = Vector3{
			entry->Maximum.X - entry->Minimum.X + 2 * entry->Padding.X,
			entry->Maximum.Y - entry->Minimum.Y + 2 * entry->Padding.Y,
			entry->Maximum.Z - entry->Minimum.Z + 2 * entry->Padding.Z
		};
		if (!detail::MeshFinite(extent)) return fail(Status::InvalidValue, "slice bounds overflow");
		const Vector3 step{extent.X / entry->Width, extent.Y / entry->Height, extent.Z / entry->Slices},
			origin{
				entry->Minimum.X - entry->Padding.X + step.X / 2,
				entry->Minimum.Y - entry->Padding.Y + step.Y * .6,
				entry->Minimum.Z - entry->Padding.Z + step.Z / 2
			};
		for (uint32_t work = 0; work < workPixels && entry->Active; ++work) {
			const Vector3 p{
				origin.X + (entry->Pixel % entry->Width) * step.X,
				origin.Y + (entry->Pixel / entry->Width) * step.Y,
				origin.Z + entry->Slice * step.Z
			};
			bool inside = false;
			Colour color{255, 255, 255, 255};
			double distance = 99999;
			for (const auto &face : entry->Faces) {
				if (face.MaximumX < p.X - .1) break;
				const auto [a, b, c] = face.Points;
				const Vector3 e1{b.X - a.X, b.Y - a.Y, b.Z - a.Z}, e2{c.X - a.X, c.Y - a.Y, c.Z - a.Z},
					s{p.X - a.X, p.Y - a.Y, p.Z - a.Z};
				const double det = e1.Z * e2.Y - e1.Y * e2.Z;
				if (std::abs(det) < .0001) continue;
				const double inv = 1 / det, u = (s.Z * e2.Y - s.Y * e2.Z) * inv;
				if (u < 0 || u > 1) continue;
				const double crossX = s.Y * e1.Z - s.Z * e1.Y, v = crossX * inv;
				if (v < 0 || u + v > 1) continue;
				const double crossY = s.Z * e1.X - s.X * e1.Z, crossZ = s.X * e1.Y - s.Y * e1.X,
							 t = (e2.X * crossX + e2.Y * crossY + e2.Z * crossZ) * inv;
				if (t <= 0) continue;
				inside = !inside;
				const double dx = p.X - face.Center.X, dy = p.Y - face.Center.Y, dz = p.Z - face.Center.Z,
							 d = dx * dx + dy * dy + dz * dz;
				if (d < distance) {
					distance = d;
					color = face.Color;
				}
			}
			if (inside) {
				auto &pixels = entry->Images[entry->Slice].Pixels;
				const size_t offset = size_t(entry->Pixel) * 4;
				pixels[offset] = color.Red;
				pixels[offset + 1] = color.Green;
				pixels[offset + 2] = color.Blue;
				pixels[offset + 3] = 255;
			}
			++entry->Pixel;
			if (entry->Pixel >= uint64_t(entry->Width) * entry->Height) {
				entry->Pixel = 0;
				++entry->Slice;
				if (entry->Slice == entry->Slices) entry->Active = false;
			}
		}
		if (ValidateSliceStackReplay(candidate, maximumBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		state = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}
}
namespace engine::imagegraph {
	std::string WriteSliceStackReplay(const SliceStackReplayState &state) {
		std::ostringstream stream;
		stream << std::setprecision(17) << "slice_stack_replay 1 " << state.Entries.size() << '\n';
		for (const auto &entry : state.Entries) {
			stream << std::quoted(entry.NodeId) << ' ' << entry.ActionTime.Tick << ' '
				   << entry.ActionTime.Subframe << ' ' << entry.ActionTime.NegativeFrame << ' '
				   << entry.Active << ' ' << entry.Width << ' ' << entry.Height << ' ' << entry.Slices << ' '
				   << entry.Slice << ' ' << entry.Pixel << ' ';
			for (auto p : {entry.Minimum, entry.Maximum, entry.Padding})
				stream << p.X << ' ' << p.Y << ' ' << p.Z << ' ';
			stream << entry.Faces.size() << '\n';
			for (const auto &face : entry.Faces) {
				for (auto p : face.Points)
					stream << p.X << ' ' << p.Y << ' ' << p.Z << ' ';
				stream << face.Center.X << ' ' << face.Center.Y << ' ' << face.Center.Z << ' '
					   << face.MaximumX << ' ' << unsigned(face.Color.Red) << ' '
					   << unsigned(face.Color.Green) << ' ' << unsigned(face.Color.Blue) << ' '
					   << unsigned(face.Color.Alpha) << '\n';
			}
			for (const auto &image : entry.Images) {
				for (auto byte : image.Pixels)
					stream << unsigned(byte) << ' ';
				stream << '\n';
			}
		}
		return stream.str();
	}
	Status ReadSliceStackReplay(
		std::string_view text, SliceStackReplayState &state, Diagnostic &diagnostic, uint64_t maximumBytes
	) {
		auto fail = [&](Status code, std::string message) {
			diagnostic = {code, {}, {}, std::move(message)};
			return code;
		};
		if (text.size() > maximumBytes)
			return fail(Status::LimitExceeded, "slice replay text exceeds budget");
		detail::EvaluationBudget ledger(maximumBytes);
		auto inputCharge = ledger.Reserve(text.size() * 2 + RetainedSliceStackReplayBytes(state));
		if (!inputCharge) return fail(Status::LimitExceeded, "slice replay read exceeds budget");
		std::istringstream stream{std::string(text)};
		std::string marker;
		unsigned version;
		size_t entries;
		if (!(stream >> marker >> version >> entries) || marker != "slice_stack_replay" || version != 1)
			return fail(Status::Malformed, "slice replay header is malformed");
		if (entries > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "slice replay entry count exceeds cap");
		auto candidateCharge = ledger.Reserve(entries * sizeof(SliceStackReplayEntry));
		if (!candidateCharge) return fail(Status::LimitExceeded, "slice replay storage exceeds budget");
		SliceStackReplayState candidate;
		candidate.Entries.resize(entries);
		for (auto &entry : candidate.Entries) {
			size_t faces;
			if (!(stream >> std::quoted(entry.NodeId) >> entry.ActionTime.Tick >> entry.ActionTime.Subframe >>
				  entry.ActionTime.NegativeFrame >> entry.Active >> entry.Width >> entry.Height >>
				  entry.Slices >> entry.Slice >> entry.Pixel))
				return fail(Status::Malformed, "slice replay entry is malformed");
			for (Vector3 *p : {&entry.Minimum, &entry.Maximum, &entry.Padding})
				if (!(stream >> p->X >> p->Y >> p->Z))
					return fail(Status::Malformed, "slice bounds are malformed");
			if (!(stream >> faces)) return fail(Status::Malformed, "slice face count is malformed");
			if (faces > Limits::MaximumArrayElements || entry.Width == 0 || entry.Height == 0 ||
				entry.Width > Limits::MaximumDimension || entry.Height > Limits::MaximumDimension ||
				entry.Slices == 0 || entry.Slices > Limits::MaximumArrayElements)
				return fail(Status::LimitExceeded, "slice replay dimensions exceed caps");
			const uint64_t pixelBytes = uint64_t(entry.Width) * entry.Height * entry.Slices * 4;
			if (pixelBytes > Limits::MaximumArrayBytes)
				return fail(Status::LimitExceeded, "slice replay images exceed cap");
			const uint64_t growth = entry.NodeId.capacity() + faces * sizeof(SliceStackFace) +
									entry.Slices * sizeof(Image) + pixelBytes;
			if (!candidateCharge->Resize(candidateCharge->Bytes() + growth))
				return fail(Status::LimitExceeded, "slice replay payload exceeds budget");
			entry.Faces.resize(faces);
			for (auto &face : entry.Faces) {
				for (auto &p : face.Points)
					if (!(stream >> p.X >> p.Y >> p.Z))
						return fail(Status::Malformed, "slice face is malformed");
				std::array<unsigned, 4> rgba;
				if (!(stream >> face.Center.X >> face.Center.Y >> face.Center.Z >> face.MaximumX >> rgba[0] >>
					  rgba[1] >> rgba[2] >> rgba[3]))
					return fail(Status::Malformed, "slice face color is malformed");
				for (auto byte : rgba)
					if (byte > 255) return fail(Status::InvalidValue, "slice color exceeds byte range");
				face.Color = {uint8_t(rgba[0]), uint8_t(rgba[1]), uint8_t(rgba[2]), uint8_t(rgba[3])};
			}
			entry.Images.resize(entry.Slices);
			for (auto &image : entry.Images) {
				image.Width = entry.Width;
				image.Height = entry.Height;
				image.Pixels.resize(size_t(entry.Width) * entry.Height * 4);
				for (auto &byte : image.Pixels) {
					unsigned value;
					if (!(stream >> value)) return fail(Status::Malformed, "slice pixels are incomplete");
					if (value > 255) return fail(Status::InvalidValue, "slice pixel exceeds byte range");
					byte = uint8_t(value);
				}
			}
		}
		stream >> std::ws;
		if (!stream.eof()) return fail(Status::Malformed, "slice replay has trailing fields");
		if (ValidateSliceStackReplay(candidate, maximumBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		state = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	}
}
