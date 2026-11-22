#include "editable/EditablePackingProperties.hpp"

#include <engine/collision/ConvexHull.hpp>
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Base64.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/characters/Skinning.hpp>
#include <engine/scene/editable/EditableMesh.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::scene {

	namespace {
		uint64_t MixMeshSignature(uint64_t hash, uint64_t word) {
			hash ^= word + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}

		uint64_t PairFloats(float first, float second) {
			return static_cast<uint64_t>(std::bit_cast<uint32_t>(first)) |
				   (static_cast<uint64_t>(std::bit_cast<uint32_t>(second)) << 32);
		}

		template <class Geometry> uint64_t SignGeometry(const Geometry &geometry) {
			// Four independent lanes keep the walk throughput-bound instead of
			// making every word wait on one dependency chain. The final fold and
			// tagged lengths keep array order and field identity in the contract.
			uint64_t lanes[4]{
				0xcbf29ce484222325ull,
				0x84222325cbf29ce4ull,
				0x9e3779b97f4a7c15ull,
				0x6a09e667f3bcc909ull,
			};
			size_t lane = 0;
			const auto word = [&](uint64_t value) {
				lanes[lane] = MixMeshSignature(lanes[lane], value);
				lane = (lane + 1) & 3u;
			};
			const auto field = [&](uint64_t tag, size_t count) {
				word(tag);
				word(static_cast<uint64_t>(count));
			};

			field(1, geometry.Positions.size());
			for (const core::Vector3 &value : geometry.Positions) {
				word(PairFloats(value.X, value.Y));
				word(PairFloats(value.Z, 0.0f));
			}
			field(2, geometry.Normals.size());
			for (const core::Vector3 &value : geometry.Normals) {
				word(PairFloats(value.X, value.Y));
				word(PairFloats(value.Z, 0.0f));
			}
			field(3, geometry.UVs.size());
			for (const core::Vector2 &value : geometry.UVs) {
				word(PairFloats(value.X, value.Y));
			}
			field(4, geometry.Colours.size());
			for (const core::Color3 &value : geometry.Colours) {
				word(PairFloats(value.R, value.G));
				word(PairFloats(value.B, 0.0f));
			}
			field(5, geometry.Alphas.size());
			for (const float value : geometry.Alphas) {
				word(PairFloats(value, 0.0f));
			}
			field(6, geometry.Indices.size());
			for (size_t at = 0; at < geometry.Indices.size(); at += 2) {
				const uint64_t first = geometry.Indices[at];
				const uint64_t second = at + 1 < geometry.Indices.size() ? geometry.Indices[at + 1] : 0;
				word(first | (second << 32));
			}
			// Absent arrays contribute only their zero length, so an unskinned
			// mesh signs the same work as before these fields existed.
			field(7, geometry.Joints.size());
			for (const EditableJoints &value : geometry.Joints) {
				word(
					static_cast<uint64_t>(value[0]) | (static_cast<uint64_t>(value[1]) << 16) |
					(static_cast<uint64_t>(value[2]) << 32) | (static_cast<uint64_t>(value[3]) << 48)
				);
			}
			field(8, geometry.Weights.size());
			for (const EditableWeights &value : geometry.Weights) {
				word(PairFloats(value[0], value[1]));
				word(PairFloats(value[2], value[3]));
			}
			for (size_t channel = 0; channel < EDITABLE_ATTRIBUTE_CHANNELS; channel++) {
				field(9 + channel, geometry.Attributes[channel].size());
				for (const EditableAttribute &value : geometry.Attributes[channel]) {
					word(PairFloats(value[0], value[1]));
					word(PairFloats(value[2], value[3]));
				}
			}

			uint64_t signature = MixMeshSignature(lanes[0], lanes[1]);
			signature = MixMeshSignature(signature, lanes[2]);
			signature = MixMeshSignature(signature, lanes[3]);
			return signature == 0 ? 1 : signature;
		}

		bool SameGeometry(const EditableMesh &mesh, const EditableMeshGeometry &geometry) {
			return mesh.Positions == geometry.Positions && mesh.Normals == geometry.Normals &&
				   mesh.UVs == geometry.UVs && mesh.Colours == geometry.Colours &&
				   mesh.Alphas == geometry.Alphas && mesh.Indices == geometry.Indices &&
				   mesh.Joints == geometry.Joints && mesh.Weights == geometry.Weights &&
				   mesh.Attributes == geometry.Attributes;
		}

		// Records that `streams` of `[first, end)` changed in the revision just
		// bumped, extending the current run or starting a new one.
		void NoteStreams(EditableMesh &mesh, uint16_t streams, uint32_t first, uint32_t end) {
			EditableStreamChanges &changes = mesh.StreamChanges;
			if (changes.Streams != 0 && changes.Through + 1 == mesh.Revision) {
				changes.Through = mesh.Revision;
				changes.Streams |= streams;
				changes.First = std::min(changes.First, first);
				changes.End = std::max(changes.End, end);
				return;
			}
			changes = {mesh.Revision, mesh.Revision, streams, first, end};
		}

		void ApplyPreparedEditableMesh(EditableMesh &mesh, PreparedEditableMesh &prepared) noexcept {
			static_assert(std::is_nothrow_swappable_v<std::vector<core::Vector3>>);
			static_assert(std::is_nothrow_swappable_v<std::vector<core::Vector2>>);
			static_assert(std::is_nothrow_swappable_v<std::vector<core::Color3>>);
			static_assert(std::is_nothrow_swappable_v<std::vector<float>>);
			static_assert(std::is_nothrow_swappable_v<std::vector<uint32_t>>);
			mesh.Positions.swap(prepared.Geometry.Positions);
			mesh.Normals.swap(prepared.Geometry.Normals);
			mesh.UVs.swap(prepared.Geometry.UVs);
			mesh.Colours.swap(prepared.Geometry.Colours);
			mesh.Alphas.swap(prepared.Geometry.Alphas);
			mesh.Indices.swap(prepared.Geometry.Indices);
			mesh.Joints.swap(prepared.Geometry.Joints);
			mesh.Weights.swap(prepared.Geometry.Weights);
			for (size_t channel = 0; channel < EDITABLE_ATTRIBUTE_CHANNELS; channel++)
				mesh.Attributes[channel].swap(prepared.Geometry.Attributes[channel]);
			mesh.Welds.clear();
			mesh.Signature = prepared.Signature;
			++mesh.Revision;
			prepared.Valid = false;
		}

		// How many vertices are in the mesh, read-only.
		ecs::PropertyDescriptor VertexCountProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("VertexCount");
			property.Type = ecs::PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const EditableMesh *held = store.Get<EditableMesh>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = static_cast<int32_t>(held->Positions.size());
				return true;
			};
			return property;
		}

		// How many triangles are in the mesh, read-only.
		ecs::PropertyDescriptor TriangleCountProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("TriangleCount");
			property.Type = ecs::PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const EditableMesh *held = store.Get<EditableMesh>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = static_cast<int32_t>(held->Indices.size() / 3);
				return true;
			};
			return property;
		}

		// --- the saved geometry -----------------------------------------------
		//
		// A mesh built by a script is rebuilt by the script, but one written by
		// a tool - an imported terrain - has no script to rebuild it, so its
		// arrays are saved as one property. The text is `atomic-mesh-1:` and the
		// base64 of: vertex count, index count, a flags byte (bit 0 normals,
		// 1 UVs, 2 colours, 3 alphas, 4 skin, 5 to 7 attribute channels 0 to 2),
		// then each present array as little-endian floats and the indices as
		// u32. Skin is four u16 slots then four f32 weights per vertex. Sections
		// the flags byte has no bit left for follow the indices, each behind a
		// tag byte: 1 is channel 3, 2 is one u32 weld per vertex.

		constexpr std::string_view GEOMETRY_PREFIX = "atomic-mesh-1:";
		std::string EncodeGeometry(const EditableMesh &mesh) {
			const size_t vertices = mesh.Positions.size();
			const bool normals = mesh.Normals.size() == vertices;
			const bool uvs = mesh.UVs.size() == vertices;
			// Default colours and alphas are left out; the reader restores them.
			const bool colours =
				mesh.Colours.size() == vertices &&
				std::any_of(mesh.Colours.begin(), mesh.Colours.end(), [](const core::Color3 &colour) {
					return colour.R != 1.0f || colour.G != 1.0f || colour.B != 1.0f;
				});
			const bool alphas = mesh.Alphas.size() == vertices &&
								std::any_of(mesh.Alphas.begin(), mesh.Alphas.end(), [](float alpha) {
									return alpha != 1.0f;
								});
			const bool skin =
				mesh.Joints.size() == vertices && mesh.Weights.size() == vertices && vertices != 0;
			std::array<bool, EDITABLE_ATTRIBUTE_CHANNELS> channels{};
			for (size_t channel = 0; channel < EDITABLE_ATTRIBUTE_CHANNELS; channel++)
				channels[channel] = mesh.Attributes[channel].size() == vertices && vertices != 0;
			core::ByteWriter writer;
			writer.WriteUInt32(static_cast<uint32_t>(vertices));
			writer.WriteUInt32(static_cast<uint32_t>(mesh.Indices.size()));
			writer.WriteUInt8(
				static_cast<uint8_t>(
					(normals ? 1u : 0u) | (uvs ? 2u : 0u) | (colours ? 4u : 0u) | (alphas ? 8u : 0u) |
					(skin ? 16u : 0u) | (channels[0] ? 32u : 0u) | (channels[1] ? 64u : 0u) |
					(channels[2] ? 128u : 0u)
				)
			);
			const auto attribute = [&writer](const EditableAttribute &value) {
				for (const float component : value)
					writer.WriteFloat(component);
			};
			const auto vector3 = [&writer](const core::Vector3 &value) {
				writer.WriteFloat(value.X);
				writer.WriteFloat(value.Y);
				writer.WriteFloat(value.Z);
			};
			for (const auto &position : mesh.Positions)
				vector3(position);
			if (normals)
				for (const auto &normal : mesh.Normals)
					vector3(normal);
			if (uvs)
				for (const auto &uv : mesh.UVs) {
					writer.WriteFloat(uv.X);
					writer.WriteFloat(uv.Y);
				}
			if (colours)
				for (const auto &colour : mesh.Colours) {
					writer.WriteFloat(colour.R);
					writer.WriteFloat(colour.G);
					writer.WriteFloat(colour.B);
				}
			if (alphas)
				for (const float alpha : mesh.Alphas)
					writer.WriteFloat(alpha);
			if (skin) {
				for (const EditableJoints &joints : mesh.Joints)
					for (const uint16_t joint : joints)
						writer.WriteUInt16(joint);
				for (const EditableWeights &weights : mesh.Weights)
					attribute(weights);
			}
			for (size_t channel = 0; channel < 3; channel++)
				if (channels[channel])
					for (const EditableAttribute &value : mesh.Attributes[channel])
						attribute(value);
			for (const uint32_t index : mesh.Indices)
				writer.WriteUInt32(index);
			if (channels[3]) {
				writer.WriteUInt8(1);
				for (const EditableAttribute &value : mesh.Attributes[3])
					attribute(value);
			}
			if (mesh.Welds.size() == vertices && vertices != 0) {
				writer.WriteUInt8(2);
				for (const uint32_t weld : mesh.Welds)
					writer.WriteUInt32(weld);
			}
			return std::string(GEOMETRY_PREFIX) + core::EncodeBase64(writer.Bytes());
		}

		bool DecodeGeometry(std::string_view text, EditableMeshGeometry &out, std::vector<uint32_t> &welds) {
			if (!text.starts_with(GEOMETRY_PREFIX)) return false;
			std::vector<std::byte> bytes;
			if (!core::DecodeBase64(text.substr(GEOMETRY_PREFIX.size()), bytes)) return false;
			core::ByteReader reader(bytes);
			const uint32_t vertices = reader.ReadUInt32();
			const uint32_t indices = reader.ReadUInt32();
			const uint8_t flags = reader.ReadUInt8();
			if (reader.Failed()) return false;
			if (vertices > MAXIMUM_EDITABLE_MESH_VERTICES || indices > MAXIMUM_EDITABLE_MESH_INDICES)
				return false;
			const size_t floatsPerVertex = 3 + ((flags & 1u) ? 3 : 0) + ((flags & 2u) ? 2 : 0) +
										   ((flags & 4u) ? 3 : 0) + ((flags & 8u) ? 1 : 0) +
										   ((flags & 16u) ? 6 : 0) + ((flags & 32u) ? 4 : 0) +
										   ((flags & 64u) ? 4 : 0) + ((flags & 128u) ? 4 : 0);
			const size_t body = size_t(vertices) * floatsPerVertex * 4 + size_t(indices) * 4;
			if (reader.Remaining() < body) return false;
			const auto attribute = [&reader] {
				EditableAttribute value{};
				for (float &component : value)
					component = reader.ReadFloat();
				return value;
			};
			const auto vector3 = [&reader] {
				const float x = reader.ReadFloat();
				const float y = reader.ReadFloat();
				const float z = reader.ReadFloat();
				return core::Vector3{x, y, z};
			};
			out = {};
			out.Positions.resize(vertices);
			for (auto &position : out.Positions)
				position = vector3();
			if (flags & 1u) {
				out.Normals.resize(vertices);
				for (auto &normal : out.Normals)
					normal = vector3();
			}
			if (flags & 2u) {
				out.UVs.resize(vertices);
				for (auto &uv : out.UVs) {
					uv.X = reader.ReadFloat();
					uv.Y = reader.ReadFloat();
				}
			}
			if (flags & 4u) {
				out.Colours.resize(vertices);
				for (auto &colour : out.Colours) {
					colour.R = reader.ReadFloat();
					colour.G = reader.ReadFloat();
					colour.B = reader.ReadFloat();
				}
			}
			if (flags & 8u) {
				out.Alphas.resize(vertices);
				for (float &alpha : out.Alphas)
					alpha = reader.ReadFloat();
			}
			if (flags & 16u) {
				out.Joints.resize(vertices);
				for (EditableJoints &joints : out.Joints)
					for (uint16_t &joint : joints)
						joint = reader.ReadUInt16();
				out.Weights.resize(vertices);
				for (EditableWeights &weights : out.Weights)
					weights = attribute();
			}
			for (size_t channel = 0; channel < 3; channel++) {
				if ((flags & (32u << channel)) == 0) continue;
				out.Attributes[channel].resize(vertices);
				for (EditableAttribute &value : out.Attributes[channel])
					value = attribute();
			}
			out.Indices.resize(indices);
			for (uint32_t &index : out.Indices)
				index = reader.ReadUInt32();
			welds.clear();
			uint8_t previous = 0;
			while (!reader.Failed() && !reader.AtEnd()) {
				// Tags appear at most once and in order.
				const uint8_t tag = reader.ReadUInt8();
				if (tag <= previous) return false;
				previous = tag;
				if (tag == 1) {
					out.Attributes[3].resize(vertices);
					for (EditableAttribute &value : out.Attributes[3])
						value = attribute();
				} else if (tag == 2) {
					if (reader.Remaining() < size_t(vertices) * 4) return false;
					welds.resize(vertices);
					for (uint32_t &weld : welds) {
						weld = reader.ReadUInt32();
						if (weld >= vertices) return false;
					}
				} else {
					return false;
				}
			}
			// Absent arrays take the values a new vertex would have.
			if (out.Normals.empty()) out.Normals.assign(vertices, core::Vector3{0.0f, 1.0f, 0.0f});
			if (out.UVs.empty()) out.UVs.assign(vertices, core::Vector2{});
			if (out.Colours.empty()) out.Colours.assign(vertices, core::Color3{1.0f, 1.0f, 1.0f});
			if (out.Alphas.empty()) out.Alphas.assign(vertices, 1.0f);
			return !reader.Failed() && reader.AtEnd();
		}

		// The whole geometry as saved text. Hidden from scripts, which have
		// `SetGeometry` and the per-vertex methods; written by documents only.
		ecs::PropertyDescriptor GeometryProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Geometry");
			property.Type = ecs::PropertyType::String;
			property.Size = sizeof(std::string);
			property.Scriptable = false;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const EditableMesh *held = store.Get<EditableMesh>(instance);
				if (held == nullptr) return false;
				*static_cast<std::string *>(out) =
					held->Positions.empty() ? std::string() : EncodeGeometry(*held);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				const auto &text = *static_cast<const std::string *>(value);
				EditableMeshGeometry geometry;
				std::vector<uint32_t> welds;
				if (!text.empty() && !DecodeGeometry(text, geometry, welds)) return false;
				const EditableMeshCommit result = ReplaceEditableMesh(store, instance, std::move(geometry));
				if (result != EditableMeshCommit::Applied && result != EditableMeshCommit::Unchanged)
					return false;
				return welds.empty() ||
					   SetEditableVertexStream(
						   store, instance, EditableVertexStream::Weld, std::as_bytes(std::span(welds))
					   );
			};
			return property;
		}

		// The content name a `MeshId` names this mesh by, read-only - see
		// `EditableMeshContentName`.
		ecs::PropertyDescriptor MeshContentIdProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("ContentId");
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<EditableMesh>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				if (store.Get<EditableMesh>(instance) == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) = EditableMeshContentName(store, instance);
				return true;
			};
			return property;
		}

		ecs::ClassId RegisterEditableMeshClass() {
			// Through `EnsureClassTree`, for `ShaderScriptClass`'s exact reason:
			// this registers the module's components on the way, and a class is
			// a set of component ids that must exist first.
			EnsureClassTree();

			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));

			// **An `Instance` and not a `PVInstance`**, `ShaderScript`'s reason
			// applied here: an `EditableMesh` has no place of its own in the
			// world. What draws it is a `MeshPart` naming its content id.
			const std::array components{ecs::Components::Of<EditableMesh>()};
			const ecs::ClassId editableMesh = ecs::Classes::Register("EditableMesh", instance, components);

			ecs::Classes::Computed(editableMesh, VertexCountProperty());
			ecs::Classes::Computed(editableMesh, TriangleCountProperty());
			ecs::Classes::Computed(editableMesh, MeshContentIdProperty());
			ecs::Classes::Computed(editableMesh, GeometryProperty());
			for (auto &property : detail::PackingProperties<EditableMesh, SetEditableMeshPacking>())
				ecs::Classes::Computed(editableMesh, std::move(property));
			for (const ecs::PropertyDescriptor &property : ecs::Classes::Describe(editableMesh).Properties)
				(void)ecs::Classes::SetPropertiesTag(editableMesh, property.Spelling, core::Name("Geometry"));
			(void)ecs::Classes::SetPropertiesTag(editableMesh, "ContentId", core::Name("Identity"));
			return editableMesh;
		}
	}

	namespace {
		bool
		BakedBefore(const EditableMeshCollision::Baked &left, const EditableMeshCollision::Baked &right) {
			return left.Instance < right.Instance;
		}

		// The collision ledger can outlive content streaming, so terrain worlds
		// routinely hold hundreds of rows. Keep its derived representation sorted
		// before every lookup. Normal refreshes already publish this order, while
		// this repair also makes a restored or tool-authored ledger safe to search.
		void CanonicalizeBakedRows(EditableMeshCollision &ledger) {
			if (std::is_sorted(ledger.Rows.begin(), ledger.Rows.end(), BakedBefore) &&
				std::adjacent_find(
					ledger.Rows.begin(), ledger.Rows.end(), [](const auto &left, const auto &right) {
						return left.Instance == right.Instance;
					}
				) == ledger.Rows.end()) {
				return;
			}
			std::stable_sort(ledger.Rows.begin(), ledger.Rows.end(), BakedBefore);
			ledger.Rows.erase(
				std::unique(
					ledger.Rows.begin(),
					ledger.Rows.end(),
					[](const auto &left, const auto &right) { return left.Instance == right.Instance; }
				),
				ledger.Rows.end()
			);
		}

		const EditableMeshCollision::Baked *
		FindBaked(const EditableMeshCollision *ledger, uint64_t instance) {
			if (ledger == nullptr) {
				return nullptr;
			}
			const auto found = std::lower_bound(
				ledger->Rows.begin(),
				ledger->Rows.end(),
				instance,
				[](const EditableMeshCollision::Baked &row, uint64_t value) { return row.Instance < value; }
			);
			return found != ledger->Rows.end() && found->Instance == instance ? &*found : nullptr;
		}

		// The demand row for one mesh, or null. `demand` is sorted by instance.
		const EditableMeshCollisionDemand *
		DemandFor(const std::vector<EditableMeshCollisionDemand> &demand, uint64_t instance) {
			const auto found = std::lower_bound(
				demand.begin(),
				demand.end(),
				instance,
				[](const EditableMeshCollisionDemand &row, uint64_t id) { return row.Instance < id; }
			);
			return found != demand.end() && found->Instance == instance ? &*found : nullptr;
		}

		// The entity id inside an `EditableMeshContentName`, or false for any
		// other geometry name. Reading a name's text takes the name registry's
		// lock, and this runs for every mesh collider every tick, so the answer
		// is kept per name: names never change once interned. The memo is
		// dropped whole when it grows large, so a world that streams meshes for
		// hours cannot grow it without bound.
		bool EditableMeshInstanceOf(core::Name geometry, uint64_t &instance) {
			constexpr size_t MEMO_LIMIT = 65'536;
			constexpr uint64_t NOT_EDITABLE = 0;
			static thread_local std::unordered_map<uint32_t, uint64_t> memo;
			if (const auto known = memo.find(geometry.Id()); known != memo.end()) {
				instance = known->second;
				return instance != NOT_EDITABLE;
			}
			constexpr std::string_view PREFIX = "editable-mesh://";
			const std::string_view text = geometry.Text();
			uint64_t parsed = NOT_EDITABLE;
			if (text.starts_with(PREFIX)) {
				const char *first = text.data() + PREFIX.size();
				const char *last = text.data() + text.size();
				const auto result = std::from_chars(first, last, parsed);
				if (result.ec != std::errc{} || result.ptr != last || first == last) parsed = NOT_EDITABLE;
			}
			if (memo.size() >= MEMO_LIMIT) memo.clear();
			memo.emplace(geometry.Id(), parsed);
			instance = parsed;
			return parsed != NOT_EDITABLE;
		}

		core::AABB WorldBoundsOf(
			const ecs::Store &store, ecs::Entity entity, const Transform &transform, const Collider &collider
		) {
			const Bounds *bounds = store.Get<Bounds>(entity);
			const core::Vector3 half = bounds != nullptr ? bounds->HalfExtent : collider.Extent * 0.5f;
			return core::OrientedBoxBounds(transform.Frame, half);
		}
	}

	void CollectEditableMeshCollisionDemand(
		const ecs::Store &store, std::vector<EditableMeshCollisionDemand> &out
	) {
		out.clear();
		auto &mutableStore = const_cast<ecs::Store &>(store);

		// Contact-only colliders wait for something that moves. The margin
		// covers more than a tick of travel at walking and falling speeds, so
		// the shape exists before the first contact it could take part in.
		constexpr float MOVER_MARGIN = 4.0f;
		constexpr float MOVER_LOOKAHEAD_SECONDS = 0.25f;
		// Past this many movers the pairwise test would cost more than the
		// bakes it avoids, so contact-only colliders are baked up front.
		constexpr size_t MOVER_LIMIT = 256;
		std::vector<core::AABB> movers;
		bool moversKnown = false;
		bool tooManyMovers = false;
		const auto gatherMovers = [&] {
			moversKnown = true;
			mutableStore.Each<const Collider, const Transform, const Motion>([&](ecs::Entity entity,
																				 const Collider &collider,
																				 const Transform &transform,
																				 const Motion &motion) {
				if (tooManyMovers) return;
				if (movers.size() == MOVER_LIMIT) {
					tooManyMovers = true;
					return;
				}
				const core::AABB box = WorldBoundsOf(store, entity, transform, collider);
				const float reach = MOVER_MARGIN + motion.Linear.Magnitude() * MOVER_LOOKAHEAD_SECONDS;
				const core::Vector3 margin{reach, reach, reach};
				movers.push_back(core::AABB{box.Minimum - margin, box.Maximum + margin});
			});
		};

		mutableStore.Each<const Collider>([&](ecs::Entity entity, const Collider &collider) {
			if (!collider.Geometry.IsValid() ||
				(collider.Shape != ShapeKind::Mesh && collider.Shape != ShapeKind::Hull)) {
				return;
			}
			// Nothing can collide with, touch or query it.
			if (collider.Trigger && collider.TouchDisabled && !collider.CanQuery) return;
			uint64_t instance = 0;
			if (!EditableMeshInstanceOf(collider.Geometry, instance)) return;
			if (!collider.CanQuery && !store.Has<Motion>(entity)) {
				const Transform *transform = store.Get<Transform>(entity);
				if (transform == nullptr) return;
				if (!moversKnown) gatherMovers();
				if (!tooManyMovers) {
					const core::AABB box = WorldBoundsOf(store, entity, *transform, collider);
					const bool near = std::any_of(movers.begin(), movers.end(), [&](const core::AABB &mover) {
						return mover.Overlaps(box);
					});
					if (!near) return;
				}
			}
			out.push_back(EditableMeshCollisionDemand{instance, collider.Shape == ShapeKind::Hull});
		});
		std::sort(out.begin(), out.end(), [](const auto &left, const auto &right) {
			return left.Instance < right.Instance;
		});
		size_t unique = 0;
		for (const EditableMeshCollisionDemand current : out) {
			if (unique != 0 && out[unique - 1].Instance == current.Instance) {
				out[unique - 1].Hull = out[unique - 1].Hull || current.Hull;
			} else {
				out[unique++] = current;
			}
		}
		out.resize(unique);
	}

	size_t RefreshEditableMeshCollision(ecs::Store &store, std::span<const uint64_t> deferred) {
		// Refuse the expensive path before copying either resource. Both can hold
		// whole terrain chunks, and copying them merely to discover that every
		// revision is already resident made a stopped editable scene cost
		// milliseconds on every presentation.
		EditableMeshCollision *heldLedger = store.ResourceMutable<EditableMeshCollision>();
		if (heldLedger != nullptr) {
			CanonicalizeBakedRows(*heldLedger);
		}
		const EditableMeshCollision *heldBaked = heldLedger;
		const CollisionShapes *heldShapes = CollisionShapesOf(store);
		// Gathered on the first editable mesh only, so worlds with no
		// script-built geometry skip the collider walk entirely. Both passes
		// below read the same answer.
		std::optional<std::vector<EditableMeshCollisionDemand>> demanded;
		const auto demandOf = [&](ecs::Entity instance) {
			if (!demanded.has_value()) {
				demanded.emplace();
				CollectEditableMeshCollisionDemand(store, *demanded);
			}
			return DemandFor(*demanded, instance.Id);
		};
		size_t retained = 0;
		bool dirty = false;
		store.Each<const EditableMesh>([&](ecs::Entity instance, const EditableMesh &mesh) {
			const EditableMeshCollisionDemand *demand = demandOf(instance);
			const EditableMeshCollision::Baked *known = FindBaked(heldBaked, instance.Id);
			retained += known != nullptr ? 1u : 0u;
			if (demand == nullptr) {
				dirty = dirty || known != nullptr;
				return;
			}

			const bool hasTriangles = !mesh.Positions.empty() && mesh.Indices.size() >= 3;
			if (known == nullptr) {
				// A newly created mesh is commonly incomplete for several edits.
				// There is no resident shape to replace or remove until it has a
				// triangle, so copying the catalogue cannot change the answer.
				dirty = dirty || hasTriangles;
				return;
			}
			if (known->Revision != mesh.Revision) {
				dirty = true;
				return;
			}
			const core::Name name = EditableMeshContentName(store, instance);
			if (heldShapes == nullptr || heldShapes->FindMesh(name) == nullptr) {
				dirty = true;
				return;
			}
			if (demand->Hull && heldShapes->FindHull(name) == nullptr) {
				dirty = true;
			}
		});
		// Count matching identities instead of all live meshes. An incomplete
		// replacement has no shape yet, but it must not make a destroyed mesh's
		// ledger row look retained merely because the two totals happen to match.
		if (heldBaked != nullptr && heldBaked->Rows.size() != retained) {
			dirty = true;
		}
		if (!dirty) {
			return 0;
		}

		struct CollisionBuild {
			core::Name Name;
			const EditableMesh *Source = nullptr;
			bool WantsHull = false;
			bool Split = false;
			collision::TriangleMesh Mesh{};
			collision::ConvexHull Hull{};
		};
		std::vector<CollisionBuild> builds;

		// What is in the world this call, so the sweep below can tell a mesh
		// that was destroyed from one that simply did not change.
		std::vector<EditableMeshCollision::Baked> seen;
		if (heldBaked != nullptr) {
			seen.reserve(heldBaked->Rows.size());
		}

		store.Each<const EditableMesh>([&](ecs::Entity instance, const EditableMesh &mesh) {
			const EditableMeshCollisionDemand *demand = demandOf(instance);
			if (demand == nullptr) {
				return;
			}
			const core::Name name = EditableMeshContentName(store, instance);
			const EditableMeshCollision::Baked *known = FindBaked(heldBaked, instance.Id);

			// The steady state: one binary revision lookup after the bounded
			// ledger canonicalization pass, which is
			// `EditableMeshUploader::Refresh`'s decision and its reason.
			//
			// **Unless a hull is wanted and is not there**, which is a part
			// switched to `ShapeKind::Hull` after its mesh was baked. See the
			// bake below for why a hull is not built until it is asked for.
			if (known != nullptr && known->Revision == mesh.Revision && heldShapes != nullptr &&
				heldShapes->FindMesh(name) != nullptr &&
				(heldShapes->FindHull(name) != nullptr || !demand->Hull)) {
				seen.push_back(EditableMeshCollision::Baked{instance.Id, mesh.Revision});
				return;
			}
			// Over this tick's work budget: keep what it had, under its old
			// revision, so a later call still sees it as stale. One with no
			// shape yet gets an empty one, because a mesh collider with no
			// shape collides as its whole bound; it waits colliding with
			// nothing, under a revision no mesh reaches.
			if (std::binary_search(deferred.begin(), deferred.end(), instance.Id)) {
				if (known != nullptr) {
					seen.push_back(*known);
				} else {
					builds.push_back(CollisionBuild{.Name = name, .Source = &mesh, .Split = true});
					seen.push_back(
						EditableMeshCollision::Baked{instance.Id, std::numeric_limits<uint32_t>::max()}
					);
				}
				return;
			}

			// **A mesh mid-edit registers nothing rather than an empty shape.**
			// Vertices added and no triangle yet is the ordinary state right
			// after `Instance.new("EditableMesh")`, and a hull of three points
			// with no soup behind it is a collider that stops nothing. The
			// revision is left unrecorded so the next call tries again.
			if (mesh.Positions.empty() || mesh.Indices.size() < 3) {
				// Retried every tick until it becomes valid, which is right and
				// is also what a mesh that never becomes valid looks like: a
				// part with no collision and nothing saying why.
				ENGINE_TRACE_EVERY(
					5.0,
					"an editable mesh has {} position(s) and {} index/indices; no collider yet",
					mesh.Positions.size(),
					mesh.Indices.size()
				);
				return;
			}

			// **The soup for every demanded shape and the hull only when asked**,
			// which is the difference between this being affordable and not.
			// Measured with the bench preset on a terrain chunk of 4,225 points
			// beside 2,000 resident shapes: the full refresh costs 0.885 ms. A
			// quickhull alone cost 7.3 ms in the same terrain fixture, and a heightfield's
			// convex hull is a dome over its summit that no collider in that
			// scene will ever ask for. `game::AddCollisionShapes` bakes both
			// because a delivered mesh is baked once at load; this runs on the
			// tick a script builds geometry, and a streamed world builds one a
			// tick for as long as somebody keeps walking.
			//
			// A part that is switched to `Hull` later gets one on the next
			// refresh, because the test below is "wanted and missing" rather
			// than "changed".
			builds.push_back(
				CollisionBuild{
					.Name = name,
					.Source = &mesh,
					.WantsHull = demand->Hull,
					.Split = EditableMeshSplitsCollision(store, instance, mesh),
					.Mesh = {},
					.Hull = {},
				}
			);
			seen.push_back(EditableMeshCollision::Baked{instance.Id, mesh.Revision});
		});

		// The store is read-only until this fork-join finishes, so each source
		// pointer remains valid and no world storage reaches a later tick. With
		// several worlds, Universe already runs this same batch concurrently per
		// scene; with one world, several changed meshes use the process pool.
		if (!builds.empty()) {
			parallel::Jobs::For(
				builds.size(),
				1,
				[&builds](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						CollisionBuild &build = builds[index];
						// A split mesh collides through its cells: an empty shape.
						if (!build.Split)
							build.Mesh =
								collision::BuildTriangleMesh(build.Source->Positions, build.Source->Indices);
						if (build.WantsHull) {
							build.Hull = collision::BuildConvexHull(build.Source->Positions);
						}
					}
				},
				2
			);
			const parallel::BatchTiming collisionTiming = parallel::Jobs::LastBatch();
			core::FrameGraph::Report(
				"editable collision workers", core::ProfileCategory::Physics, collisionTiming.BusyMilliseconds
			);
		}
		// **The shapes of meshes that are gone, dropped here.** A streamed
		// world creates and destroys a mesh per chunk, and a table that only
		// grew would hold a hull and a triangle soup for every chunk anybody
		// ever walked past. Nothing can name them again: the content name
		// carries the entity's generation, so even a reused id mints a new one.
		// `Store::Each` has no ordering contract. Sort once at the owner-thread
		// barrier, then merge the two ledgers instead of checking every old row
		// against every live mesh. A terrain with N retained chunks used to make
		// this housekeeping O(N squared) even when no worker had any geometry to
		// build.
		std::sort(seen.begin(), seen.end(), BakedBefore);
		std::vector<core::Name> forgotten;
		if (heldBaked != nullptr) {
			size_t live = 0;
			for (const EditableMeshCollision::Baked &was : heldBaked->Rows) {
				while (live < seen.size() && seen[live].Instance < was.Instance) {
					live++;
				}
				if (live == seen.size() || seen[live].Instance != was.Instance) {
					forgotten.emplace_back("editable-mesh://" + std::to_string(was.Instance));
				}
			}
		}

		const size_t changed = builds.size() + forgotten.size();

		if (changed == 0) {
			return 0;
		}

		// Commit after every worker has joined. Keeping the resident shape table
		// in place avoids copying every unchanged terrain BVH just to replace one
		// chunk. The ECS still owns the only table and observes the whole edit at
		// this owner-thread barrier.
		if (store.Resource<CollisionShapes>() == nullptr) {
			store.SetResource(CollisionShapes{});
		}
		CollisionShapes *shapes = store.ResourceMutable<CollisionShapes>();
		for (CollisionBuild &build : builds) {
			shapes->SetMesh(build.Name, std::move(build.Mesh));
			if (build.WantsHull) {
				shapes->SetHull(build.Name, std::move(build.Hull));
			}
		}
		for (const core::Name name : forgotten) {
			shapes->Forget(name);
		}

		EditableMeshCollision baked;
		baked.Rows = std::move(seen);
		store.SetResource(std::move(baked));
		return changed;
	}

	namespace {
		bool FiniteFour(const std::array<float, 4> &value) {
			return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]) &&
				   std::isfinite(value[3]);
		}

		bool ValidInfluences(const EditableJoints &joints, const EditableWeights &weights) {
			for (size_t slot = 0; slot < 4; slot++) {
				if (!std::isfinite(weights[slot]) || weights[slot] < 0.0f) return false;
				if (weights[slot] != 0.0f && joints[slot] >= MAX_JOINTS) return false;
			}
			return true;
		}

		// The skin and attribute half of the geometry contract.
		bool ValidExtras(const EditableMeshGeometry &geometry) {
			const size_t vertices = geometry.Positions.size();
			if (geometry.Joints.size() != geometry.Weights.size()) return false;
			if (!geometry.Joints.empty() && geometry.Joints.size() != vertices) return false;
			for (size_t vertex = 0; vertex < geometry.Joints.size(); vertex++)
				if (!ValidInfluences(geometry.Joints[vertex], geometry.Weights[vertex])) return false;
			for (const auto &channel : geometry.Attributes) {
				if (!channel.empty() && channel.size() != vertices) return false;
				for (const EditableAttribute &value : channel)
					if (!FiniteFour(value)) return false;
			}
			return true;
		}

		// Reads `Count` values of `T` at one element of a field. The caller has
		// already bounded the whole field. Buffers are little-endian, which is
		// every platform this engine builds for.
		template <class T, size_t Count>
		std::array<T, Count>
		ReadElement(std::span<const std::byte> bytes, const EditableBufferField &field, size_t element) {
			std::array<T, Count> value{};
			const size_t stride = field.Stride != 0 ? field.Stride : sizeof(T) * Count;
			std::memcpy(value.data(), bytes.data() + field.Offset + element * stride, sizeof(T) * Count);
			return value;
		}

		// Whether a field's last element ends inside the buffer.
		bool FieldFits(const EditableBufferField &field, size_t elementBytes, uint32_t count, size_t bytes) {
			if (!field.Present || count == 0) return true;
			const uint64_t stride = field.Stride != 0 ? field.Stride : elementBytes;
			if (field.Stride != 0 && field.Stride < elementBytes) return false;
			const uint64_t end = uint64_t(field.Offset) + uint64_t(count - 1) * stride + elementBytes;
			return end <= bytes;
		}
	}

	static_assert(std::endian::native == std::endian::little);

	std::string DecodeEditableMeshBuffers(
		const EditableBufferLayout &layout,
		std::span<const std::byte> vertices,
		std::span<const std::byte> indices,
		EditableMeshGeometry &out
	) {
		ENGINE_PROFILE_CAT("DecodeEditableMeshBuffers", core::ProfileCategory::Engine);
		const uint32_t count = layout.VertexCount;
		if (count > MAXIMUM_EDITABLE_MESH_VERTICES) return "VertexCount is past the mesh ceiling";
		if (!layout.Position.Present) return "layout needs a Position field";
		if (layout.Joints.Present != layout.Weights.Present)
			return "Joints and Weights must be given together";
		const size_t indexBytes = layout.WideIndices ? 4 : 2;
		if (indices.size() % indexBytes != 0) return "index buffer is not whole indices";
		if (indices.size() / indexBytes > MAXIMUM_EDITABLE_MESH_INDICES)
			return "index count is past the mesh ceiling";

		struct Check {
			const EditableBufferField &Field;
			size_t Bytes;
			const char *Name;
		};
		const Check checks[] = {
			{layout.Position, 12, "Position"},
			{layout.Normal, 12, "Normal"},
			{layout.UV, 8, "UV"},
			{layout.Colour, 12, "Color"},
			{layout.Alpha, 4, "Alpha"},
			{layout.Joints, 8, "Joints"},
			{layout.Weights, 16, "Weights"},
			{layout.Attributes[0], 16, "Attribute0"},
			{layout.Attributes[1], 16, "Attribute1"},
			{layout.Attributes[2], 16, "Attribute2"},
			{layout.Attributes[3], 16, "Attribute3"},
		};
		for (const Check &check : checks)
			if (!FieldFits(check.Field, check.Bytes, count, vertices.size()))
				return std::string(check.Name) + " reads past the end of the vertex buffer";

		EditableMeshGeometry geometry;
		const auto vector3 = [&](const EditableBufferField &field, size_t element) {
			const auto value = ReadElement<float, 3>(vertices, field, element);
			return core::Vector3{value[0], value[1], value[2]};
		};
		geometry.Positions.resize(count);
		for (uint32_t vertex = 0; vertex < count; vertex++)
			geometry.Positions[vertex] = vector3(layout.Position, vertex);
		if (layout.Normal.Present) {
			geometry.Normals.resize(count);
			for (uint32_t vertex = 0; vertex < count; vertex++)
				geometry.Normals[vertex] = vector3(layout.Normal, vertex);
		} else {
			geometry.Normals.assign(count, core::Vector3{0.0f, 1.0f, 0.0f});
		}
		if (layout.UV.Present) {
			geometry.UVs.resize(count);
			for (uint32_t vertex = 0; vertex < count; vertex++) {
				const auto value = ReadElement<float, 2>(vertices, layout.UV, vertex);
				geometry.UVs[vertex] = core::Vector2{value[0], value[1]};
			}
		} else {
			geometry.UVs.assign(count, core::Vector2{});
		}
		if (layout.Colour.Present) {
			geometry.Colours.resize(count);
			for (uint32_t vertex = 0; vertex < count; vertex++) {
				const auto value = ReadElement<float, 3>(vertices, layout.Colour, vertex);
				geometry.Colours[vertex] = core::Color3{value[0], value[1], value[2]};
			}
		} else {
			geometry.Colours.assign(count, core::Color3{1.0f, 1.0f, 1.0f});
		}
		if (layout.Alpha.Present) {
			geometry.Alphas.resize(count);
			for (uint32_t vertex = 0; vertex < count; vertex++)
				geometry.Alphas[vertex] = ReadElement<float, 1>(vertices, layout.Alpha, vertex)[0];
		} else {
			geometry.Alphas.assign(count, 0.0f);
		}
		if (layout.Joints.Present) {
			geometry.Joints.resize(count);
			geometry.Weights.resize(count);
			for (uint32_t vertex = 0; vertex < count; vertex++) {
				geometry.Joints[vertex] = ReadElement<uint16_t, 4>(vertices, layout.Joints, vertex);
				geometry.Weights[vertex] = ReadElement<float, 4>(vertices, layout.Weights, vertex);
			}
		}
		for (size_t channel = 0; channel < EDITABLE_ATTRIBUTE_CHANNELS; channel++) {
			if (!layout.Attributes[channel].Present) continue;
			geometry.Attributes[channel].resize(count);
			for (uint32_t vertex = 0; vertex < count; vertex++)
				geometry.Attributes[channel][vertex] =
					ReadElement<float, 4>(vertices, layout.Attributes[channel], vertex);
		}

		const size_t indexCount = indices.size() / indexBytes;
		geometry.Indices.resize(indexCount);
		if (layout.WideIndices) {
			std::memcpy(geometry.Indices.data(), indices.data(), indices.size());
		} else {
			for (size_t index = 0; index < indexCount; index++) {
				uint16_t value = 0;
				std::memcpy(&value, indices.data() + index * 2, 2);
				geometry.Indices[index] = value;
			}
		}
		out = std::move(geometry);
		return {};
	}

	uint64_t EditableMeshSignature(const EditableMeshGeometry &geometry) {
		return SignGeometry(geometry);
	}

	PreparedEditableMesh PrepareEditableMesh(EditableMeshGeometry geometry) {
		PreparedEditableMesh prepared;
		prepared.Valid = geometry.Positions.size() == geometry.Normals.size() &&
						 geometry.Positions.size() == geometry.UVs.size() &&
						 geometry.Positions.size() == geometry.Colours.size() &&
						 geometry.Positions.size() == geometry.Alphas.size() &&
						 geometry.Indices.size() % 3 == 0;

		if (prepared.Valid) {
			for (const uint32_t index : geometry.Indices) {
				if (index >= geometry.Positions.size()) {
					prepared.Valid = false;
					break;
				}
			}
		}
		if (prepared.Valid) prepared.Valid = ValidExtras(geometry);

		prepared.Signature = SignGeometry(geometry);
		prepared.Geometry = std::move(geometry);
		return prepared;
	}

	EditableMeshCommit CommitEditableMesh(
		ecs::Store &store, ecs::Entity instance, PreparedEditableMesh prepared, uint32_t expectedRevision
	) {
		if (!prepared.Valid) {
			return EditableMeshCommit::Invalid;
		}

		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return EditableMeshCommit::Missing;
		}
		if (mesh->Revision != expectedRevision) {
			return EditableMeshCommit::Stale;
		}

		const uint64_t currentSignature = mesh->Signature != 0 ? mesh->Signature : SignGeometry(*mesh);
		if (currentSignature == prepared.Signature && SameGeometry(*mesh, prepared.Geometry)) {
			mesh->Signature = currentSignature;
			return EditableMeshCommit::Unchanged;
		}

		ApplyPreparedEditableMesh(*mesh, prepared);
		return EditableMeshCommit::Applied;
	}

	EditableMeshCommit CommitEditableMeshBatch(ecs::Store &store, std::span<EditableMeshUpdate> updates) {
		ENGINE_PROFILE_CAT("CommitEditableMeshBatch", core::ProfileCategory::Engine);
		if (updates.size() > MAXIMUM_EDITABLE_MESH_BATCH) return EditableMeshCommit::Invalid;
		struct Destination {
			EditableMesh *Mesh = nullptr;
			uint64_t Signature = 0;
			bool Changed = false;
		};
		std::array<Destination, MAXIMUM_EDITABLE_MESH_BATCH> destinations{};
		bool anyChanged = false;
		for (size_t index = 0; index < updates.size(); ++index) {
			const auto &update = updates[index];
			if (!update.Prepared.Valid) return EditableMeshCommit::Invalid;
			for (size_t previous = 0; previous < index; ++previous)
				if (updates[previous].Instance == update.Instance) return EditableMeshCommit::Invalid;
			const auto *mesh = store.Get<EditableMesh>(update.Instance);
			if (mesh == nullptr) return EditableMeshCommit::Missing;
			if (mesh->Revision != update.ExpectedRevision) return EditableMeshCommit::Stale;
			const uint64_t signature = mesh->Signature != 0 ? mesh->Signature : SignGeometry(*mesh);
			const bool changed =
				signature != update.Prepared.Signature || !SameGeometry(*mesh, update.Prepared.Geometry);
			if (changed && mesh->Revision == std::numeric_limits<uint32_t>::max())
				return EditableMeshCommit::Invalid;
			destinations[index] = {nullptr, signature, changed};
			anyChanged |= changed;
		}
		// Marking an observed row can grow the ECS change list. Finish every such
		// operation before touching a geometry array; these lookups never move rows.
		for (size_t index = 0; index < updates.size(); ++index)
			destinations[index].Mesh = store.GetMutable<EditableMesh>(updates[index].Instance);
		for (size_t index = 0; index < updates.size(); ++index) {
			auto &destination = destinations[index];
			auto &mesh = *destination.Mesh;
			if (!destination.Changed) {
				mesh.Signature = destination.Signature;
				continue;
			}
			ApplyPreparedEditableMesh(mesh, updates[index].Prepared);
		}
		return anyChanged ? EditableMeshCommit::Applied : EditableMeshCommit::Unchanged;
	}

	EditableMeshCommit
	ReplaceEditableMesh(ecs::Store &store, ecs::Entity instance, EditableMeshGeometry geometry) {
		const EditableMesh *mesh = store.Get<EditableMesh>(instance);
		if (mesh == nullptr) {
			return EditableMeshCommit::Missing;
		}
		return CommitEditableMesh(store, instance, PrepareEditableMesh(std::move(geometry)), mesh->Revision);
	}

	core::Name EditableMeshContentName(const ecs::Store &store, ecs::Entity instance) {
		if (store.Get<EditableMesh>(instance) == nullptr) {
			return {};
		}
		// **The entity's own id, generation included**, so a destroyed
		// `EditableMesh` and whatever the allocator later mints in its place
		// never resolve to the same content name - the exact hazard `rule 4`
		// exists to close everywhere else a handle would otherwise cross.
		//
		// The name is a pure function of the id, and the collision refresh asks
		// for it per mesh per presented frame, so each thread keeps the interned
		// answer instead of formatting and interning a string every call. The
		// memo is dropped whole past its limit, like `EditableMeshInstanceOf`.
		constexpr size_t MEMO_LIMIT = 65'536;
		static thread_local std::unordered_map<uint64_t, core::Name> memo;
		if (const auto known = memo.find(instance.Id); known != memo.end()) return known->second;
		if (memo.size() >= MEMO_LIMIT) memo.clear();
		const core::Name name("editable-mesh://" + std::to_string(instance.Id));
		memo.emplace(instance.Id, name);
		return name;
	}

	std::optional<uint32_t> AddVertex(
		ecs::Store &store,
		ecs::Entity instance,
		const core::Vector3 &position,
		const core::Vector3 &normal,
		const core::Vector2 &uv
	) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return std::nullopt;
		}

		const uint32_t id = static_cast<uint32_t>(mesh->Positions.size());
		mesh->Positions.push_back(position);
		mesh->Normals.push_back(normal);
		mesh->UVs.push_back(uv);
		mesh->Colours.push_back(core::Color3{1.0f, 1.0f, 1.0f});
		mesh->Alphas.push_back(0.0f);
		// Optional arrays stay parallel once they exist.
		if (!mesh->Joints.empty()) {
			mesh->Joints.push_back({});
			mesh->Weights.push_back({});
		}
		for (auto &channel : mesh->Attributes)
			if (!channel.empty()) channel.push_back({});
		if (!mesh->Welds.empty()) mesh->Welds.push_back(id);
		mesh->Signature = 0;
		mesh->Revision++;
		return id;
	}

	std::optional<uint32_t>
	AddTriangle(ecs::Store &store, ecs::Entity instance, uint32_t a, uint32_t b, uint32_t c) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return std::nullopt;
		}

		const size_t count = mesh->Positions.size();
		if (a >= count || b >= count || c >= count) {
			return std::nullopt;
		}

		const uint32_t id = static_cast<uint32_t>(mesh->Indices.size() / 3);
		mesh->Indices.push_back(a);
		mesh->Indices.push_back(b);
		mesh->Indices.push_back(c);
		mesh->Signature = 0;
		mesh->Revision++;
		return id;
	}

	bool RemoveTriangle(ecs::Store &store, ecs::Entity instance, uint32_t triangle) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return false;
		}

		const size_t triangles = mesh->Indices.size() / 3;
		if (triangle >= triangles) {
			return false;
		}

		// swap-and-pop, three indices at a time - the method's own header
		// carries what this does to the last triangle's id.
		const size_t last = triangles - 1;
		if (triangle != last) {
			mesh->Indices[triangle * 3 + 0] = mesh->Indices[last * 3 + 0];
			mesh->Indices[triangle * 3 + 1] = mesh->Indices[last * 3 + 1];
			mesh->Indices[triangle * 3 + 2] = mesh->Indices[last * 3 + 2];
		}
		mesh->Indices.resize(last * 3);
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool SetVertexPosition(
		ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &position
	) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Positions.size()) {
			return false;
		}
		if (mesh->Positions[vertex] == position) {
			return true;
		}
		mesh->Positions[vertex] = position;
		mesh->Signature = 0;
		mesh->Revision++;
		NoteStreams(*mesh, EditableStreamBit(EditableVertexStream::Position), vertex, vertex + 1);
		return true;
	}

	bool
	SetVertexNormal(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector3 &normal) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Normals.size()) {
			return false;
		}
		if (mesh->Normals[vertex] == normal) {
			return true;
		}
		mesh->Normals[vertex] = normal;
		mesh->Signature = 0;
		mesh->Revision++;
		NoteStreams(*mesh, EditableStreamBit(EditableVertexStream::Normal), vertex, vertex + 1);
		return true;
	}

	bool SetVertexUV(ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Vector2 &uv) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->UVs.size()) {
			return false;
		}
		if (mesh->UVs[vertex] == uv) {
			return true;
		}
		mesh->UVs[vertex] = uv;
		mesh->Signature = 0;
		mesh->Revision++;
		NoteStreams(*mesh, EditableStreamBit(EditableVertexStream::UV), vertex, vertex + 1);
		return true;
	}

	bool SetVertexColor(
		ecs::Store &store, ecs::Entity instance, uint32_t vertex, const core::Color3 &colour, float alpha
	) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr || vertex >= mesh->Colours.size()) {
			return false;
		}
		if (mesh->Colours[vertex] == colour && mesh->Alphas[vertex] == alpha) {
			return true;
		}
		mesh->Colours[vertex] = colour;
		mesh->Alphas[vertex] = alpha;
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool ClearEditableMesh(ecs::Store &store, ecs::Entity instance) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh == nullptr) {
			return false;
		}
		mesh->Positions.clear();
		mesh->Normals.clear();
		mesh->UVs.clear();
		mesh->Colours.clear();
		mesh->Alphas.clear();
		mesh->Indices.clear();
		mesh->Joints.clear();
		mesh->Weights.clear();
		for (auto &channel : mesh->Attributes)
			channel.clear();
		mesh->Welds.clear();
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool SetVertexSkin(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t vertex,
		std::span<const uint16_t> joints,
		std::span<const float> weights
	) {
		if (joints.size() != weights.size() || joints.size() > 4) return false;
		EditableJoints slots{};
		EditableWeights amounts{};
		std::copy(joints.begin(), joints.end(), slots.begin());
		std::copy(weights.begin(), weights.end(), amounts.begin());
		if (!ValidInfluences(slots, amounts)) return false;
		const EditableMesh *held = store.Get<EditableMesh>(instance);
		if (held == nullptr || vertex >= held->Positions.size()) return false;
		if (!held->Joints.empty() && held->Joints[vertex] == slots && held->Weights[vertex] == amounts)
			return true;
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		if (mesh->Joints.empty()) {
			mesh->Joints.assign(mesh->Positions.size(), EditableJoints{});
			mesh->Weights.assign(mesh->Positions.size(), EditableWeights{});
		}
		mesh->Joints[vertex] = slots;
		mesh->Weights[vertex] = amounts;
		mesh->Signature = 0;
		mesh->Revision++;
		return true;
	}

	bool SetVertexAttribute(
		ecs::Store &store,
		ecs::Entity instance,
		uint32_t vertex,
		size_t channel,
		const EditableAttribute &value
	) {
		if (channel >= EDITABLE_ATTRIBUTE_CHANNELS || !FiniteFour(value)) return false;
		const EditableMesh *held = store.Get<EditableMesh>(instance);
		if (held == nullptr || vertex >= held->Positions.size()) return false;
		if (!held->Attributes[channel].empty() && held->Attributes[channel][vertex] == value) return true;
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		auto &values = mesh->Attributes[channel];
		const bool carried = !values.empty();
		if (!carried) values.assign(mesh->Positions.size(), EditableAttribute{});
		values[vertex] = value;
		mesh->Signature = 0;
		mesh->Revision++;
		// A new channel changes what the mesh carries, which no patch describes.
		if (carried)
			NoteStreams(
				*mesh,
				EditableStreamBit(
					static_cast<EditableVertexStream>(
						static_cast<uint8_t>(EditableVertexStream::Attribute0) + channel
					)
				),
				vertex,
				vertex + 1
			);
		return true;
	}

	bool SetEditableMeshPacking(ecs::Store &store, ecs::Entity instance, const EditablePacking &packing) {
		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		constexpr uint8_t attributes = static_cast<uint8_t>(EditablePackingAttribute::Position) |
									   static_cast<uint8_t>(EditablePackingAttribute::Normal) |
									   static_cast<uint8_t>(EditablePackingAttribute::UV) |
									   static_cast<uint8_t>(EditablePackingAttribute::Colour) |
									   static_cast<uint8_t>(EditablePackingAttribute::Alpha);
		if (mesh == nullptr ||
			static_cast<uint8_t>(packing.Format) > static_cast<uint8_t>(EditablePackingFormat::Boolean) ||
			(packing.Attributes & ~attributes) != 0 || !std::isfinite(packing.Minimum) ||
			!std::isfinite(packing.Maximum) || packing.Maximum < packing.Minimum)
			return false;
		if (mesh->Packing.Attributes == packing.Attributes && mesh->Packing.Format == packing.Format &&
			mesh->Packing.Minimum == packing.Minimum && mesh->Packing.Maximum == packing.Maximum)
			return true;
		mesh->Packing.Attributes = packing.Attributes;
		mesh->Packing.Format = packing.Format;
		mesh->Packing.Minimum = packing.Minimum;
		mesh->Packing.Maximum = packing.Maximum;
		mesh->Packing.Revision++;
		mesh->Revision++;
		return true;
	}

	uint32_t EditableVertexStreamComponents(EditableVertexStream stream) {
		switch (stream) {
		case EditableVertexStream::Position:
		case EditableVertexStream::Normal:
			return 3;
		case EditableVertexStream::UV:
			return 2;
		case EditableVertexStream::Weld:
			return 1;
		default:
			return 4;
		}
	}

	bool SetEditableVertexStream(
		ecs::Store &store,
		ecs::Entity instance,
		EditableVertexStream stream,
		std::span<const std::byte> bytes,
		uint32_t first,
		std::optional<uint32_t> count
	) {
		if (static_cast<uint8_t>(stream) > static_cast<uint8_t>(EditableVertexStream::Weld)) return false;
		const EditableMesh *held = store.Get<EditableMesh>(instance);
		if (held == nullptr) return false;
		const size_t vertices = held->Positions.size();
		if (first > vertices) return false;
		const size_t written = count.value_or(static_cast<uint32_t>(vertices - first));
		if (written > vertices - first) return false;
		const size_t width = EditableVertexStreamComponents(stream);
		if (bytes.size() != written * width * 4) return false;

		// Decoded and checked before the row is touched, so a refusal changes
		// nothing and an identical write bumps no revision.
		std::vector<float> floats;
		std::vector<uint32_t> welds;
		if (stream == EditableVertexStream::Weld) {
			welds.resize(written);
			if (written != 0) std::memcpy(welds.data(), bytes.data(), bytes.size());
			for (const uint32_t weld : welds)
				if (weld >= vertices) return false;
		} else {
			floats.resize(written * width);
			if (written != 0) std::memcpy(floats.data(), bytes.data(), bytes.size());
			for (const float value : floats)
				if (!std::isfinite(value)) return false;
		}
		if (written == 0) return true;

		const auto float3 = [&](size_t record) {
			return core::Vector3{floats[record * 3], floats[record * 3 + 1], floats[record * 3 + 2]};
		};
		const auto float4 = [&](size_t record) {
			return EditableAttribute{
				floats[record * 4], floats[record * 4 + 1], floats[record * 4 + 2], floats[record * 4 + 3]
			};
		};
		const size_t channel =
			static_cast<size_t>(stream) - static_cast<size_t>(EditableVertexStream::Attribute0);
		bool same = true;
		for (size_t record = 0; record < written && same; record++) {
			const size_t vertex = first + record;
			switch (stream) {
			case EditableVertexStream::Position:
				same = held->Positions[vertex] == float3(record);
				break;
			case EditableVertexStream::Normal:
				same = held->Normals[vertex] == float3(record);
				break;
			case EditableVertexStream::UV:
				same = held->UVs[vertex] == core::Vector2{floats[record * 2], floats[record * 2 + 1]};
				break;
			case EditableVertexStream::Colour: {
				const EditableAttribute value = float4(record);
				same = held->Colours[vertex] == core::Color3{value[0], value[1], value[2]} &&
					   held->Alphas[vertex] == value[3];
				break;
			}
			case EditableVertexStream::Weld:
				same = held->Welds.empty() ? welds[record] == vertex : held->Welds[vertex] == welds[record];
				break;
			default:
				same =
					!held->Attributes[channel].empty() && held->Attributes[channel][vertex] == float4(record);
				break;
			}
		}
		if (same) return true;

		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		// Whether the write only moves values a resident copy already carries.
		bool patch = true;
		for (size_t record = 0; record < written; record++) {
			const size_t vertex = first + record;
			switch (stream) {
			case EditableVertexStream::Position:
				mesh->Positions[vertex] = float3(record);
				break;
			case EditableVertexStream::Normal:
				mesh->Normals[vertex] = float3(record);
				break;
			case EditableVertexStream::UV:
				mesh->UVs[vertex] = core::Vector2{floats[record * 2], floats[record * 2 + 1]};
				break;
			case EditableVertexStream::Colour: {
				const EditableAttribute value = float4(record);
				mesh->Colours[vertex] = core::Color3{value[0], value[1], value[2]};
				mesh->Alphas[vertex] = value[3];
				break;
			}
			case EditableVertexStream::Weld:
				if (mesh->Welds.empty()) {
					mesh->Welds.resize(vertices);
					for (size_t index = 0; index < vertices; index++)
						mesh->Welds[index] = static_cast<uint32_t>(index);
				}
				mesh->Welds[vertex] = welds[record];
				break;
			default:
				if (mesh->Attributes[channel].empty()) {
					mesh->Attributes[channel].assign(vertices, EditableAttribute{});
					patch = false;
				}
				mesh->Attributes[channel][vertex] = float4(record);
				break;
			}
		}
		// Colour regroups draw runs and a weld reshapes the LOD build, so
		// neither is a vertex patch.
		if (stream == EditableVertexStream::Colour || stream == EditableVertexStream::Weld) patch = false;
		mesh->Signature = 0;
		mesh->Revision++;
		if (patch)
			NoteStreams(*mesh, EditableStreamBit(stream), first, static_cast<uint32_t>(first + written));
		return true;
	}

	bool RecalculateEditableMeshNormals(
		ecs::Store &store, ecs::Entity instance, uint32_t first, std::optional<uint32_t> count
	) {
		const EditableMesh *held = store.Get<EditableMesh>(instance);
		if (held == nullptr) return false;
		const size_t vertices = held->Positions.size();
		if (first > vertices) return false;
		const size_t written = count.value_or(static_cast<uint32_t>(vertices - first));
		if (written > vertices - first) return false;
		if (written == 0) return true;

		const bool welded = held->Welds.size() == vertices;
		const auto point = [&](uint32_t vertex) { return welded ? held->Welds[vertex] : vertex; };
		std::vector<core::Vector3> sums(vertices, core::Vector3{});
		for (size_t index = 0; index + 2 < held->Indices.size(); index += 3) {
			const uint32_t a = held->Indices[index];
			const uint32_t b = held->Indices[index + 1];
			const uint32_t c = held->Indices[index + 2];
			const core::Vector3 &pa = held->Positions[a];
			const core::Vector3 ab = held->Positions[b] - pa;
			const core::Vector3 ac = held->Positions[c] - pa;
			// Counter-clockwise from outside, so this points out. Its length is
			// twice the face's area, which is the weight.
			const core::Vector3 face{
				ab.Y * ac.Z - ab.Z * ac.Y,
				ab.Z * ac.X - ab.X * ac.Z,
				ab.X * ac.Y - ab.Y * ac.X,
			};
			sums[point(a)] = sums[point(a)] + face;
			sums[point(b)] = sums[point(b)] + face;
			sums[point(c)] = sums[point(c)] + face;
		}

		std::vector<core::Vector3> normals(
			held->Normals.begin() + first, held->Normals.begin() + first + written
		);
		bool changed = false;
		for (size_t record = 0; record < written; record++) {
			const core::Vector3 &sum = sums[point(static_cast<uint32_t>(first + record))];
			const float length = std::sqrt(sum.X * sum.X + sum.Y * sum.Y + sum.Z * sum.Z);
			if (!(length > 0.0f) || !std::isfinite(length)) continue;
			const core::Vector3 normal{sum.X / length, sum.Y / length, sum.Z / length};
			if (normals[record] == normal) continue;
			normals[record] = normal;
			changed = true;
		}
		if (!changed) return true;

		EditableMesh *mesh = store.GetMutable<EditableMesh>(instance);
		std::copy(normals.begin(), normals.end(), mesh->Normals.begin() + first);
		mesh->Signature = 0;
		mesh->Revision++;
		NoteStreams(
			*mesh,
			EditableStreamBit(EditableVertexStream::Normal),
			first,
			static_cast<uint32_t>(first + written)
		);
		return true;
	}

	ecs::ClassId EditableMeshClass() {
		static const ecs::ClassId editableMesh = RegisterEditableMeshClass();
		return editableMesh;
	}
}

namespace engine::scene {
	namespace {
		// A cell's collider: the owner's settings, naming the cell's mesh and
		// bounding only it.
		Collider CellCollider(const Collider &owner, core::Name geometry, core::Vector3 extent) {
			Collider cell = owner;
			cell.Shape = ShapeKind::Mesh;
			cell.Geometry = geometry;
			cell.Extent = extent;
			return cell;
		}

		// Where a cell sits: the owner's frame moved by the cell's centre.
		Transform CellTransform(const Transform &owner, core::Vector3 centre) {
			return Transform{owner.Frame * core::CFrame(centre)};
		}

		// A cut cell: its vertices about its centre, and its half size.
		struct CutCell {
			EditableMesh Mesh;
			core::Vector3 Centre;
			core::Vector3 Extent;
		};

		bool SameCollider(const Collider &left, const Collider &right) {
			return left.Extent == right.Extent && left.Layer == right.Layer && left.Mask == right.Mask &&
				   left.Geometry == right.Geometry && left.Shape == right.Shape &&
				   left.Trigger == right.Trigger && left.CanQuery == right.CanQuery &&
				   left.TouchDisabled == right.TouchDisabled;
		}

		// Cuts `mesh` into cells by triangle centre. Each keeps only the
		// vertices it uses; a triangle wider than a cell stays whole in one.
		std::vector<CutCell> CutCells(const EditableMesh &mesh) {
			struct Key {
				int32_t X, Y, Z;
				bool operator==(const Key &) const = default;
			};
			struct Hash {
				size_t operator()(const Key &key) const {
					return (static_cast<size_t>(static_cast<uint32_t>(key.X)) * 73856093u) ^
						   (static_cast<size_t>(static_cast<uint32_t>(key.Y)) * 19349663u) ^
						   (static_cast<size_t>(static_cast<uint32_t>(key.Z)) * 83492791u);
				}
			};
			std::unordered_map<Key, size_t, Hash> cellOf;
			std::vector<EditableMesh> cells;
			std::vector<std::unordered_map<uint32_t, uint32_t>> remap;
			const auto at = [](float value) {
				return static_cast<int32_t>(std::floor(value / (3.0f * SPLIT_COLLISION_CELL)));
			};
			for (size_t first = 0; first + 2 < mesh.Indices.size(); first += 3) {
				const uint32_t a = mesh.Indices[first], b = mesh.Indices[first + 1],
							   c = mesh.Indices[first + 2];
				if (a >= mesh.Positions.size() || b >= mesh.Positions.size() || c >= mesh.Positions.size())
					continue;
				const core::Vector3 sum = mesh.Positions[a] + mesh.Positions[b] + mesh.Positions[c];
				const auto [found, added] =
					cellOf.try_emplace(Key{at(sum.X), at(sum.Y), at(sum.Z)}, cells.size());
				if (added) {
					cells.emplace_back();
					cells.back().Revision = 1;
					remap.emplace_back();
				}
				EditableMesh &cell = cells[found->second];
				auto &map = remap[found->second];
				for (const uint32_t source : {a, b, c}) {
					const auto [slot, fresh] =
						map.try_emplace(source, static_cast<uint32_t>(cell.Positions.size()));
					if (fresh) cell.Positions.push_back(mesh.Positions[source]);
					cell.Indices.push_back(slot->second);
				}
			}
			std::vector<CutCell> out;
			out.reserve(cells.size());
			for (EditableMesh &cell : cells) {
				core::Vector3 low = cell.Positions.front(), high = low;
				for (const core::Vector3 &position : cell.Positions) {
					low = {
						std::min(low.X, position.X), std::min(low.Y, position.Y), std::min(low.Z, position.Z)
					};
					high = {
						std::max(high.X, position.X),
						std::max(high.Y, position.Y),
						std::max(high.Z, position.Z)
					};
				}
				const core::Vector3 centre = (low + high) * 0.5f;
				for (core::Vector3 &position : cell.Positions)
					position = position - centre;
				// Flat cells have no thickness on one axis; a bound needs some.
				const core::Vector3 half = (high - low) * 0.5f;
				const core::Vector3 extent{
					std::max(half.X, 0.05f), std::max(half.Y, 0.05f), std::max(half.Z, 0.05f)
				};
				out.push_back(CutCell{std::move(cell), centre, extent});
			}
			return out;
		}
	}

	bool
	EditableMeshSplitsCollision(const ecs::Store &store, ecs::Entity instance, const EditableMesh &mesh) {
		return mesh.Indices.size() / 3 > SPLIT_COLLISION_TRIANGLES &&
			   !store.Has<SplitCollisionCell>(instance);
	}

	bool SyncSplitCollision(ecs::Store &store) {
		// Most worlds have no editable mesh and no cell; neither walk runs.
		if (store.Query<const EditableMesh>().Count() == 0 &&
			store.Query<const SplitCollisionCell>().Count() == 0)
			return false;

		// The cells there are now, by owner.
		std::unordered_map<uint64_t, std::vector<ecs::Entity>> cellsOf;
		store.Each<const SplitCollisionCell>([&](ecs::Entity cell, const SplitCollisionCell &row) {
			cellsOf[row.Owner.Id].push_back(cell);
		});

		bool changed = false;
		struct Cut {
			ecs::Entity Owner;
			ecs::Entity Source;
		};
		std::vector<Cut> cuts;
		store.Each<const Collider>([&](ecs::Entity owner, const Collider &collider) {
			if (collider.Shape != ShapeKind::Mesh || store.Has<SplitCollisionCell>(owner)) return;
			uint64_t instance = 0;
			if (!collider.Geometry.IsValid() || !EditableMeshInstanceOf(collider.Geometry, instance)) return;
			const ecs::Entity source{instance};
			const EditableMesh *mesh = store.Alive(source) ? store.Get<EditableMesh>(source) : nullptr;
			const Transform *transform = store.Get<Transform>(owner);
			const auto held = cellsOf.find(owner.Id);
			const bool wanted = mesh != nullptr && transform != nullptr && !store.Has<Motion>(owner) &&
								EditableMeshSplitsCollision(store, source, *mesh);
			if (!wanted) return;
			bool current = held != cellsOf.end() && !held->second.empty();
			if (current) {
				const SplitCollisionCell *row = store.Get<SplitCollisionCell>(held->second.front());
				current = row->Source == source && row->Revision == mesh->Revision;
			}
			if (!current) {
				cuts.push_back(Cut{owner, source});
				return;
			}
			// Follow the owner. Written only when different, so a still world
			// leaves the static index alone.
			for (const ecs::Entity cell : held->second) {
				const Transform place =
					CellTransform(*transform, store.Get<SplitCollisionCell>(cell)->Centre);
				const Transform *at = store.Get<Transform>(cell);
				if (at == nullptr || std::memcmp(at, &place, sizeof(Transform)) != 0) {
					store.Set(cell, place);
					changed = true;
				}
				const Collider *own = store.Get<Collider>(cell);
				if (own == nullptr) continue;
				const Collider want = CellCollider(collider, own->Geometry, own->Extent);
				if (!SameCollider(*own, want)) {
					store.Set(cell, want);
					changed = true;
				}
			}
			held->second.clear();
		});
		// Whatever is left belongs to an owner that is gone, no longer splits,
		// or is being cut again.
		for (auto &[owner, cells] : cellsOf)
			for (const ecs::Entity cell : cells) {
				store.Destroy(cell);
				changed = true;
			}
		for (const Cut &cut : cuts) {
			const EditableMesh &mesh = *store.Get<EditableMesh>(cut.Source);
			const uint32_t revision = mesh.Revision;
			std::vector<CutCell> cells = CutCells(mesh);
			for (CutCell &piece : cells) {
				// Reads before `Create`, which may move the columns they live in.
				const Transform transform = *store.Get<Transform>(cut.Owner);
				const Collider collider = *store.Get<Collider>(cut.Owner);
				const ecs::Entity cell = store.Create();
				store.Set(cell, std::move(piece.Mesh));
				store.Set(cell, SplitCollisionCell{cut.Owner, cut.Source, revision, piece.Centre});
				store.Set(cell, CellTransform(transform, piece.Centre));
				store.Set(cell, CellCollider(collider, EditableMeshContentName(store, cell), piece.Extent));
			}
			changed = true;
		}
		return changed;
	}
}
