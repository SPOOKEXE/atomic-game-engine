// What content this world actually has, from a script.
//
// **A scene could name an asset and had no way to ask what the names were.**
// `part.MeshId = "props/fox.amesh"` is rule 4 working correctly - a name crosses
// and an id does not - but it left every demo in `examples/` holding a string
// literal for a file that only exists if somebody baked and published that exact
// tree. `MeshGrid.luau` had six of them, and on any store but the one it was
// written against every single part fell back to a cube. Nothing warned: an
// unregistered mesh draws as a cube, which is also what a mesh still streaming
// in looks like.
//
// So this is the other half of the pair: the catalogues the client already fills
// as content arrives, readable from a script.
//
// **Neutral since v0.16, and nothing in this file names a VM.** It was six
// `lua_CFunction`s and a `lua_State` per line, which is why JavaScript did not
// have this service at all - an installer can only build the VM it was written
// against. Every method is a `ScriptMethod` now and `ServiceCatalogue.cpp`
// builds both languages from the one surface at the foot of the file.
//
// **Most of it reports what this world has, not what a store holds.**
// `MeshCatalogue` and `TextureCatalogue` are written by whatever registered the
// content this run - the client's content pump - so an empty list means "nothing
// has arrived here", which on a headless server is the permanent and correct
// answer.
//
// **`GetPublishedMeshes` is the one exception, and v0.10 made it necessary.**
// While content was fetched by kind, everything published arrived whether a
// scene wanted it or not, so "what has arrived" and "what exists" were the same
// list and only the first needed asking. Nothing is fetched by kind any more -
// `client/ContentDemand.hpp` carries the 6.9 GB that ended it - so a scene
// reading only the first can never discover anything, and every demo was back to
// string literals. It reads the manifest a client already verified, which is a
// few hundred strings rather than a store, and naming one of them is what
// fetches it.
//
// **Names, in sorted order, and nothing else.** Not handles, not a table of
// metadata that would then be a second place facts about a mesh live -
// `MeshPart.TrianglesCount` is already the way to ask about one. Sorted because
// the catalogues are hash maps and a demo that laid parts out in iteration order
// would arrange itself differently on every run, which is exactly the kind of
// non-determinism `AGENTS.md` rule 5 is about even when nothing replicates it.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/script/MeshSource.hpp>
#include <engine/script/ScriptCall.hpp>
#include <engine/script/ServiceSurface.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace engine::script {
	namespace {
		// Answers a sorted list of names.
		//
		// **Views over interned text and never owned strings**, which is what
		// `ScriptCall::ReturnStrings` takes: a `core::Name` never releases, so a
		// view into one outlives the call by construction and a catalogue read
		// every frame costs no allocation per entry.
		void ReturnSorted(ScriptCall &call, std::vector<std::string_view> names) {
			std::sort(names.begin(), names.end());
			call.ReturnStrings(names);
		}

		// `ContentService:GetMeshes()`
		void GetMeshes(ScriptCall &call) {
			std::vector<std::string_view> names;
			if (const auto *catalogue = call.World().Resource<scene::MeshCatalogue>(); catalogue != nullptr) {
				names.reserve(catalogue->Triangles.size());
				for (const auto &[id, triangles] : catalogue->Triangles) {
					// **Skipped rather than reported as an empty string.** An id
					// that no longer resolves is a name registry that has been
					// reset under this world, which is not a thing a script can
					// act on.
					const core::Name name = core::Name::FromId(id);
					if (name.IsValid()) {
						names.emplace_back(name.Text());
					}
				}
			}
			ReturnSorted(call, std::move(names));
		}

		// `ContentService:GetPublishedMeshes()`
		//
		// **What there is to name, where `GetMeshes` says what has been named.**
		// The two used to be one question because content was fetched by kind, so
		// everything published arrived whether or not a scene wanted it. Since
		// v0.10 nothing is fetched by kind - `client/ContentDemand.hpp` has the
		// 6.9 GB that made that necessary - and the consequence landed here: a
		// scene reading `GetMeshes` sees only what it or another scene already
		// asked for, so it can never discover anything.
		//
		// Reading this and setting a `MeshId` *is* the ask. The name goes into
		// the world, `CollectWantedContent` finds it on the next pump, and that
		// one asset is fetched. A scene decides how many to take;
		// `MeshGrid.luau` takes twelve.
		void GetPublishedMeshes(ScriptCall &call) {
			std::vector<core::Name> published;
			(void)scene::PublishedMeshes(call.World(), published);

			std::vector<std::string_view> names;
			names.reserve(published.size());
			for (const core::Name &mesh : published) {
				names.emplace_back(mesh.Text());
			}
			ReturnSorted(call, std::move(names));
		}

		// `ContentService:GetMeshTextures(mesh)` -> `{ string }`
		//
		// **What a model is wearing, which nothing else could answer.** A
		// `MeshPart` naming no `TextureID` shows whatever each of its submeshes
		// recorded at bake time, and those names live *inside* the mesh file -
		// so a script that wants to swap a character's sheet had no way to learn
		// the current one and no name to put back. `MeshCatalogue::Textures`
		// records them at intake, where the file is open.
		//
		// **In submesh order with duplicates kept**, unlike every other list
		// here: a character with twenty submeshes sharing four sheets is four
		// names repeated, and which run wears which is a fact worth not
		// destroying. So this one is deliberately *not* sorted or deduplicated.
		//
		// An empty list for a mesh nothing has recorded and for one whose
		// submeshes name nothing - every built-in is the second. Those two are
		// the same answer for the same reason `TrianglesOf` answers zero to
		// both: this world cannot tell you, and a caller can act on neither.
		void GetMeshTextures(ScriptCall &call) {
			const core::Name mesh(call.AsString(0));

			std::vector<core::Name> sheets;
			(void)scene::SheetsOf(call.World(), mesh, sheets);

			std::vector<std::string_view> names;
			names.reserve(sheets.size());
			for (const core::Name &sheet : sheets) {
				// **An empty string, not a hole and not a null.** A submesh that
				// names no sheet is an ordinary thing - a model with one
				// untextured run - and the slot has to stay in the list or the
				// index stops meaning "submesh number", which is the whole
				// reason this is in submesh order.
				//
				// `Name("")` is invalid and `Name::Text()` on an invalid name is
				// a view over a **null pointer**, so the empty view here is over
				// a literal rather than default-constructed - `lua_pushlstring`
				// traps on a null even with a length of zero. Found twice: by a
				// crash on the first model with an untextured submesh, and again
				// by the same test the day this moved to `ReturnStrings`.
				names.emplace_back(sheet.IsValid() ? sheet.Text() : std::string_view(""));
			}

			call.ReturnStrings(names);
		}

		// `ContentService:GetTextures()`
		void GetTextures(ScriptCall &call) {
			std::vector<std::string_view> names;
			if (const auto *catalogue = call.World().Resource<scene::TextureCatalogue>();
				catalogue != nullptr) {
				names.reserve(catalogue->Flipbooks.size());
				for (const auto &[id, facts] : catalogue->Flipbooks) {
					const core::Name name = core::Name::FromId(id);
					if (name.IsValid()) {
						names.emplace_back(name.Text());
					}
				}
			}
			ReturnSorted(call, std::move(names));
		}

		// `ContentService:GetFlipbook(texture)` -> `{Side, Frames, FrameRate}` or nil
		//
		// **The one piece of metadata that is not derivable and not already
		// exposed.** A flipbook's grid and rate come from the source file - a
		// GIF states a delay per frame - and a scene that wanted to drive an
		// emitter at the authored rate would otherwise have to hardcode a number
		// the bake already knows. Nil for a still image, which is the same
		// answer as "this world has not been told", and for the same reason as
		// `TextureCatalogue::Find`: neither is something to play.
		//
		// **A `ScriptValue` map rather than a return of its own**, which is the
		// decision this method forced. Three numbers under three names is
		// exactly a `ValueTag::Map`; both VMs already build one, and a
		// `ReturnRecord` invented here would be a return type per service shape
		// on an interface that is supposed to carry what its callers ask for.
		// The limit is real and is the useful half of the answer: a record
		// holding an `Instance` or an `EnumItem` could not go through this door,
		// which is why `GetBoundActionInfo` is still written twice.
		void GetFlipbook(ScriptCall &call) {
			const scene::FlipbookFacts facts = scene::FlipbookOf(call.World(), core::Name(call.AsString(0)));
			if (!facts.IsFlipbook()) {
				call.ReturnNil();
				return;
			}

			const auto number = [](double value) {
				ScriptValue held{ValueTag::Number};
				held.Number = value;
				return held;
			};

			ScriptValue record{ValueTag::Map};
			record.Entries.emplace_back("Side", number(facts.Side));
			record.Entries.emplace_back("Frames", number(facts.Frames));
			record.Entries.emplace_back("FrameRate", number(facts.FrameRate));
			call.ReturnValue(record);
		}

		void GetMeshSize(ScriptCall &call) {
			call.ReturnVector3(scene::MeshSizeOf(call.World(), core::Name(call.AsString(0))));
		}

		// `ContentService:GetTriangleCount(mesh)`
		//
		// The same number `MeshPart.TrianglesCount` gives, asked about a mesh
		// rather than about a part - so a script can size a layout before it has
		// built anything to measure.
		void GetTriangleCount(ScriptCall &call) {
			call.ReturnNumber(
				static_cast<double>(scene::TrianglesOf(call.World(), core::Name(call.AsString(0))))
			);
		}

		ScriptValue NumberValue(double value) {
			ScriptValue held{ValueTag::Number};
			held.Number = value;
			return held;
		}

		ScriptValue FieldValue(uint32_t offset, uint32_t stride) {
			ScriptValue field{ValueTag::Map};
			field.Entries.emplace_back("Offset", NumberValue(offset));
			field.Entries.emplace_back("Stride", NumberValue(stride));
			return field;
		}

		template <class T> std::span<const std::byte> BytesOf(const std::vector<T> &values) {
			return std::as_bytes(std::span(values));
		}

		// `ContentService:ReadMesh(mesh)` -> record or nil
		//
		// **What this world has received, like every reader here**, so nil on a
		// headless server and before the content pump has delivered the mesh.
		// Naming the mesh on a `MeshPart` is what fetches it.
		//
		// The vertex attributes come back as one `Vertices` buffer of
		// consecutive blocks and a `Layout` describing them, which is exactly
		// what `EditableMesh:SetGeometryFromBuffer` takes, so a read mesh can be
		// edited and rebuilt without repacking.
		void ReadMesh(ScriptCall &call) {
			const std::shared_ptr<const scene::MeshSource> source =
				scene::MeshSourceOf(call.World(), core::Name(call.AsString(0)));
			if (source == nullptr) {
				call.ReturnNil();
				return;
			}
			const scene::MeshSource &mesh = *source;
			const bool skinned = !mesh.Joints.empty();

			std::vector<std::byte> vertices;
			ScriptValue layout{ValueTag::Map};
			layout.Entries.emplace_back("VertexCount", NumberValue(mesh.VertexCount));
			ScriptValue indexFormat{ValueTag::String};
			indexFormat.Text = "u32";
			layout.Entries.emplace_back("IndexFormat", std::move(indexFormat));
			const auto block = [&](const char *name, std::span<const std::byte> bytes, uint32_t stride) {
				layout.Entries.emplace_back(name, FieldValue(static_cast<uint32_t>(vertices.size()), stride));
				vertices.insert(vertices.end(), bytes.begin(), bytes.end());
			};
			vertices.reserve(size_t(mesh.VertexCount) * (skinned ? 56 : 32));
			block("Position", BytesOf(mesh.Positions), 12);
			block("Normal", BytesOf(mesh.Normals), 12);
			block("UV", BytesOf(mesh.UVs), 8);
			if (skinned) {
				block("Joints", BytesOf(mesh.Joints), 8);
				block("Weights", BytesOf(mesh.Weights), 16);
			}

			ScriptValue submeshes{ValueTag::Array};
			for (const auto &[first, count] : mesh.Submeshes) {
				ScriptValue run{ValueTag::Map};
				run.Entries.emplace_back("FirstIndex", NumberValue(first));
				run.Entries.emplace_back("IndexCount", NumberValue(count));
				submeshes.Items.push_back(std::move(run));
			}

			// Parents are palette slots like the joint buffer's, so zero-based,
			// with -1 for a root.
			ScriptValue skeleton{ValueTag::Array};
			for (const scene::MeshSourceJoint &joint : mesh.Skeleton) {
				ScriptValue entry{ValueTag::Map};
				ScriptValue name{ValueTag::String};
				name.Text = joint.Name;
				entry.Entries.emplace_back("Name", std::move(name));
				entry.Entries.emplace_back(
					"Parent",
					NumberValue(joint.Parent == scene::MeshSourceJoint::NO_PARENT ? -1.0 : joint.Parent)
				);
				ScriptValue rest{ValueTag::CFrame};
				rest.Frame = joint.Rest;
				entry.Entries.emplace_back("Rest", std::move(rest));
				ScriptValue inverseBind{ValueTag::CFrame};
				inverseBind.Frame = joint.InverseBind;
				entry.Entries.emplace_back("InverseBind", std::move(inverseBind));
				skeleton.Items.push_back(std::move(entry));
			}

			ScriptValue record{ValueTag::Map};
			record.Entries.emplace_back("VertexCount", NumberValue(mesh.VertexCount));
			record.Entries.emplace_back("IndexCount", NumberValue(static_cast<double>(mesh.Indices.size())));
			record.Entries.emplace_back("JointCount", NumberValue(mesh.JointCount));
			record.Entries.emplace_back("Layout", std::move(layout));
			record.Entries.emplace_back("Submeshes", std::move(submeshes));
			record.Entries.emplace_back("Skeleton", std::move(skeleton));
			const std::array<ScriptCall::NamedBytes, 2> buffers{{
				{"Vertices", vertices},
				{"Indices", BytesOf(mesh.Indices)},
			}};
			call.ReturnRecord(record, buffers);
		}

		constexpr std::array<ServiceMethod, 8> CONTENT_METHODS{{
			{"ReadMesh", ReadMesh},
			{"GetMeshes", GetMeshes},
			{"GetPublishedMeshes", GetPublishedMeshes},
			{"GetMeshTextures", GetMeshTextures},
			{"GetMeshSize", GetMeshSize},
			{"GetTextures", GetTextures},
			{"GetFlipbook", GetFlipbook},
			{"GetTriangleCount", GetTriangleCount},
		}};
	}

	std::shared_ptr<const scene::MeshSource> BuildMeshSource(const assets::MeshData &mesh) {
		auto source = std::make_shared<scene::MeshSource>();
		const size_t count = mesh.Vertices.size();
		source->VertexCount = static_cast<uint32_t>(count);
		source->JointCount = mesh.JointCount;
		source->Positions.resize(count * 3);
		source->Normals.resize(count * 3);
		source->UVs.resize(count * 2);
		if (mesh.JointCount != 0) {
			source->Joints.resize(count * 4);
			source->Weights.resize(count * 4);
		}
		for (size_t index = 0; index < count; index++) {
			const assets::MeshVertex &vertex = mesh.Vertices[index];
			std::memcpy(&source->Positions[index * 3], vertex.Position, sizeof(vertex.Position));
			std::memcpy(&source->Normals[index * 3], vertex.Normal, sizeof(vertex.Normal));
			std::memcpy(&source->UVs[index * 2], vertex.TexCoord, sizeof(vertex.TexCoord));
			if (mesh.JointCount == 0) continue;
			for (size_t slot = 0; slot < 4; slot++) {
				source->Joints[index * 4 + slot] = vertex.Joints[slot];
				source->Weights[index * 4 + slot] = static_cast<float>(vertex.Weights[slot]) / 65535.0f;
			}
		}
		source->Indices = mesh.Indices;
		source->Submeshes.reserve(mesh.Submeshes.size());
		for (const assets::Submesh &submesh : mesh.Submeshes)
			source->Submeshes.push_back({submesh.FirstIndex, submesh.IndexCount});
		source->Skeleton.reserve(mesh.Skeleton.size());
		for (const assets::MeshJoint &joint : mesh.Skeleton)
			source->Skeleton.push_back({joint.Name, joint.Parent, joint.Rest, joint.InverseBind});
		return source;
	}

	const ServiceSurface &ContentServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "ContentService";
			surface.Methods = CONTENT_METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
