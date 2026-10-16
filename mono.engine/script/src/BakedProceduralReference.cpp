#include "ProceduralSceneState.hpp"
#include "ProceduralState.hpp"

#include <engine/assets/ContentForm.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace engine::script {
	namespace baked_reference_detail {
		constexpr uint32_t ENCODING_VERSION = 2;
		constexpr uint32_t DOCUMENT_VERSION = 1;
		constexpr size_t MAXIMUM_NAME_BYTES = 512;

		bool ValidName(std::string_view name) {
			if (name.empty() || name.size() > MAXIMUM_NAME_BYTES ||
				assets::FormOfName(name) != assets::ContentForm::AProcedural || name.front() == '/' ||
				name.find('\\') != name.npos || name.find(':') != name.npos || name.find('\0') != name.npos)
				return false;
			size_t begin = 0;
			while (begin < name.size()) {
				const size_t end = name.find('/', begin);
				const auto part = name.substr(begin, end == name.npos ? name.npos : end - begin);
				if (part.empty() || part == "." || part == "..") return false;
				if (end == name.npos) break;
				begin = end + 1;
			}
			return name.back() != '/';
		}

		bool Valid(const BakedProceduralReference &reference) {
			return reference.Part != ecs::NULL_ENTITY && reference.Revision != 0 &&
				   reference.Scope.CellsPerSide > 0 && reference.Scope.CellsPerSide <= 64 &&
				   ValidName(reference.Description.Text());
		}

		bool Unconfigured(const BakedProceduralReference &reference) {
			return !reference.Description.IsValid() && reference.Scope.X == 0 && reference.Scope.Y == 0 &&
				   reference.Scope.CellsPerSide == procedural::RegionScope{}.CellsPerSide &&
				   reference.Part == ecs::NULL_ENTITY && reference.Revision == 0;
		}

		void Write(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *rows = static_cast<const BakedProceduralReference *>(source);
			for (size_t i = 0; i < count; ++i) {
				writer.WriteUInt32(ENCODING_VERSION);
				writer.WriteName(rows[i].Description);
				writer.WriteInt32(rows[i].Scope.X);
				writer.WriteInt32(rows[i].Scope.Y);
				writer.WriteUInt16(rows[i].Scope.CellsPerSide);
				writer.WriteUInt64(rows[i].Part.Id);
				writer.WriteUInt64(rows[i].Revision);
			}
		}

		void Read(core::ByteReader &reader, void *destination, size_t count) {
			auto *rows = static_cast<BakedProceduralReference *>(destination);
			for (size_t i = 0; i < count; ++i) {
				if (reader.ReadUInt32() != ENCODING_VERSION) {
					reader.Fail();
					return;
				}
				BakedProceduralReference candidate;
				const auto name = reader.ReadString();
				candidate.Scope.X = reader.ReadInt32();
				candidate.Scope.Y = reader.ReadInt32();
				candidate.Scope.CellsPerSide = reader.ReadUInt16();
				candidate.Part = ecs::Entity(reader.ReadUInt64());
				candidate.Revision = reader.ReadUInt64();
				if (reader.Failed()) return;
				if (name.empty()) {
					// Only the exact default is an inert snapshot value. A partial
					// demand must not become a loadable row.
					if (!Unconfigured(candidate)) {
						reader.Fail();
						return;
					}
					rows[i] = candidate;
					continue;
				}
				if (!ValidName(name)) {
					reader.Fail();
					return;
				}
				try {
					candidate.Description = core::Name(name);
				} catch (const std::bad_alloc &) {
					reader.Fail();
					return;
				}
				if (!Valid(candidate)) {
					reader.Fail();
					return;
				}
				rows[i] = candidate;
			}
		}
	}

	void RegisterBakedProceduralReferenceComponents() {
		ecs::Components::Register<BakedProceduralReference>(
			"script.BakedProceduralReference", baked_reference_detail::Write, baked_reference_detail::Read
		);
	}

	bool EncodeBakedProceduralReferences(
		const ecs::Store &store,
		const std::function<uint32_t(ecs::Entity)> &documentId,
		std::vector<uint8_t> &out,
		procedural::Diagnostic &diagnostic
	) {
		using namespace baked_reference_detail;
		using procedural_detail::Refuse;
		if (!store.IsOnOwningThread() || !documentId)
			return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_owner");
		try {
			struct Record {
				uint32_t Mesh = 0;
				BakedProceduralReference Reference{};
			};
			std::array<Record, MAXIMUM_BAKED_PROCEDURAL_REFERENCES> records{};
			size_t count = 0;
			bool valid = true;
			const auto capture = [&](ecs::Entity mesh) {
				if (!valid) return;
				const auto *reference = store.Get<BakedProceduralReference>(mesh);
				if (!reference || Unconfigured(*reference)) return;
				const uint32_t meshId = documentId(mesh);
				if (meshId == 0) return;
				if (!Valid(*reference)) {
					valid = Refuse(diagnostic, procedural::Status::InvalidInput, "baked.reference");
					return;
				}
				if (count == records.size()) {
					valid = Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.references");
					return;
				}
				const uint32_t partId = documentId(reference->Part);
				ProceduralSceneTarget target;
				if (partId == 0) {
					valid = Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_part");
					return;
				}
				if (!CaptureProceduralSceneTarget(
						store, reference->Scope, mesh, reference->Part, target, diagnostic
					)) {
					valid = false;
					return;
				}
				for (size_t prior = 0; prior < count; ++prior)
					if (records[prior].Mesh == meshId || records[prior].Reference.Part.Id == partId) {
						valid =
							Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_duplicate");
						return;
					}
				auto saved = *reference;
				saved.Part = ecs::Entity{partId};
				records[count++] = {meshId, saved};
			};
			store.EachRoot([&](ecs::Entity root) {
				capture(root);
				store.EachDescendant(root, capture);
			});
			if (!valid) return false;
			if (count == 0) {
				out.clear();
				diagnostic = {};
				return true;
			}
			std::sort(records.begin(), records.begin() + count, [](const Record &left, const Record &right) {
				return left.Mesh < right.Mesh;
			});
			core::ByteWriter writer(0, MAXIMUM_BAKED_PROCEDURAL_DOCUMENT_BYTES);
			writer.WriteUInt32(DOCUMENT_VERSION);
			writer.WriteUInt32(static_cast<uint32_t>(count));
			for (size_t index = 0; index < count; ++index) {
				writer.WriteUInt32(records[index].Mesh);
				Write(writer, &records[index].Reference, 1);
			}
			const auto bytes = writer.Bytes();
			std::vector<uint8_t> encoded(bytes.size());
			std::transform(bytes.begin(), bytes.end(), encoded.begin(), [](std::byte byte) {
				return std::to_integer<uint8_t>(byte);
			});
			out.swap(encoded);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "baked.document_storage");
		} catch (const std::length_error &) {
			return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.document_bytes");
		}
	}

	bool DecodeBakedProceduralReferences(
		ecs::Store &store,
		std::span<const uint8_t> bytes,
		const std::function<ecs::Entity(uint32_t)> &instance,
		procedural::Diagnostic &diagnostic
	) {
		using namespace baked_reference_detail;
		using procedural_detail::Refuse;
		if (!store.IsOnOwningThread() || store.AdoptOnly() || !instance ||
			!ecs::Components::Assigned<BakedProceduralReference>().IsValid())
			return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_owner");
		if (bytes.size() > MAXIMUM_BAKED_PROCEDURAL_DOCUMENT_BYTES)
			return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.document_bytes");
		try {
			core::ByteReader reader(std::as_bytes(bytes));
			if (reader.ReadUInt32() != DOCUMENT_VERSION)
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_version");
			const uint32_t count = reader.ReadUInt32();
			if (count > MAXIMUM_BAKED_PROCEDURAL_REFERENCES)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.references");
			struct Record {
				BakedProceduralReference Reference{};
				ProceduralSceneTarget Target{};
				BakedProceduralReference *Destination = nullptr;
				scene::Visual *Visual = nullptr;
				scene::Collider *Collider = nullptr;
			};
			std::array<Record, MAXIMUM_BAKED_PROCEDURAL_REFERENCES> records{};
			for (uint32_t index = 0; index < count; ++index) {
				const uint32_t meshId = reader.ReadUInt32();
				auto &record = records[index];
				Read(reader, &record.Reference, 1);
				if (reader.Failed() || meshId == 0 || !Valid(record.Reference) ||
					record.Reference.Part.Id > std::numeric_limits<uint32_t>::max())
					return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_reference");
				const auto mesh = instance(meshId);
				const auto part = instance(static_cast<uint32_t>(record.Reference.Part.Id));
				if (mesh == ecs::NULL_ENTITY || part == ecs::NULL_ENTITY)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_instance");
				for (uint32_t prior = 0; prior < index; ++prior)
					if (records[prior].Target.Mesh == mesh || records[prior].Target.Part == part)
						return Refuse(
							diagnostic, procedural::Status::InvalidInput, "baked.document_duplicate"
						);
				const auto *visual = store.Get<scene::Visual>(part);
				const auto *collider = store.Get<scene::Collider>(part);
				if (!visual || !collider)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_binding");
				// Reflected CanCollide stores Trigger. Empty Mask belongs to the
				// derived procedural binding, restored here as in ProvisionTarget.
				auto candidateCollider = *collider;
				candidateCollider.Mask = spatial::LayerMask::None();
				if (!procedural_scene_detail::CaptureTarget(
						store,
						record.Reference.Scope,
						mesh,
						part,
						visual->Mesh,
						candidateCollider,
						record.Target,
						diagnostic
					))
					return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_binding");
				record.Reference.Part = part;
				record.Reference.Revision = 1;
			}
			if (!reader.AtEnd())
				return Refuse(diagnostic, procedural::Status::InvalidInput, "baked.document_packet");
			if (count == 0) {
				diagnostic = {};
				return true;
			}
			size_t retainedCount = count;
			store.Each<const BakedProceduralReference>([&](ecs::Entity mesh,
														   const BakedProceduralReference &reference) {
				if (!Valid(reference)) return;
				for (uint32_t index = 0; index < count; ++index)
					if (records[index].Target.Mesh == mesh) return;
				++retainedCount;
			});
			if (retainedCount > MAXIMUM_BAKED_PROCEDURAL_REFERENCES)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "baked.references");
			// Structural additions can move columns. Retain pointers only after
			// every allocation has finished; failed staging leaves inert rows only.
			for (uint32_t index = 0; index < count; ++index)
				if (!store.Has<BakedProceduralReference>(records[index].Target.Mesh))
					store.Set(records[index].Target.Mesh, BakedProceduralReference{});
			for (uint32_t index = 0; index < count; ++index) {
				auto &record = records[index];
				record.Destination = store.GetMutable<BakedProceduralReference>(record.Target.Mesh);
				record.Visual = store.GetMutable<scene::Visual>(record.Target.Part);
				record.Collider = store.GetMutable<scene::Collider>(record.Target.Part);
			}
			static_assert(std::is_nothrow_copy_assignable_v<BakedProceduralReference>);
			static_assert(std::is_nothrow_copy_assignable_v<core::Name>);
			static_assert(std::is_nothrow_copy_assignable_v<spatial::LayerMask>);
			for (uint32_t index = 0; index < count; ++index) {
				auto &record = records[index];
				*record.Destination = record.Reference;
				record.Visual->Mesh = record.Target.Content;
				record.Visual->Fitted = record.Target.Content;
				record.Collider->Mask = record.Target.BeforeMask;
			}
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "baked.document_storage");
		}
	}

	bool SetBakedProceduralReference(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		std::string_view descriptionName,
		procedural::Diagnostic &diagnostic
	) {
		if (!baked_reference_detail::ValidName(descriptionName) || scope.CellsPerSide == 0 ||
			scope.CellsPerSide > 64 || !ecs::Components::Assigned<BakedProceduralReference>().IsValid()) {
			diagnostic.Code = procedural::Status::InvalidInput;
			diagnostic.Field = "baked.reference";
			return false;
		}
		if (!store.Has<BakedProceduralReference>(mesh)) {
			size_t count = 0;
			store.Each<const BakedProceduralReference>([&](ecs::Entity, const BakedProceduralReference &) {
				++count;
			});
			if (count >= MAXIMUM_BAKED_PROCEDURAL_REFERENCES) {
				diagnostic.Code = procedural::Status::LimitExceeded;
				diagnostic.Field = "baked.references";
				return false;
			}
		}
		const auto *prior = store.Get<BakedProceduralReference>(mesh);
		if (prior && prior->Revision == std::numeric_limits<uint64_t>::max()) {
			diagnostic.Code = procedural::Status::LimitExceeded;
			diagnostic.Field = "baked.revision";
			return false;
		}
		try {
			const core::Name name(descriptionName);
			const BakedProceduralReference candidate{
				name, scope, part, prior ? prior->Revision + 1 : uint64_t{1}
			};
			ProceduralSceneTarget target;
			if (!CaptureProceduralSceneTarget(store, scope, mesh, part, target, diagnostic)) return false;
			store.Set(mesh, candidate);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic.Code = procedural::Status::AllocationFailed;
			diagnostic.Field = "baked.reference_storage";
			return false;
		}
	}
}
