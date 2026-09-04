#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Shaders.hpp>

#include <algorithm>
#include <array>
#include <string>

namespace engine::scene {

	namespace {
		// The GLSL, as a property, with the revision moved by the write.
		//
		// **`Computed` rather than `Property<&ShaderSource::Code>`, and the
		// difference is the counter.** A generated field setter writes the
		// string and nothing else, which would leave `Revision` saying the
		// shader had not changed - so a library holding the last revision it
		// compiled would go on drawing the previous one, for ever, and the
		// symptom is an edit that does nothing.
		ecs::PropertyDescriptor SourceProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Source");
			property.Type = ecs::PropertyType::String;
			property.Size = sizeof(std::string);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ShaderSource>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const ShaderSource *held = store.Get<ShaderSource>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<std::string *>(out) = held->Code;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				return SetShaderSource(store, instance, *static_cast<const std::string *>(value));
			};
			return property;
		}

		// How many times the source has been written, read-only.
		//
		// **Read-only rather than absent**, because it is the number a consumer
		// acts on and a consumer that cannot see it cannot be debugged: a shader
		// that will not recompile is either a revision that is not moving or a
		// compile that is failing, and those have different fixes. Writable it
		// would be a way to make the library skip a real edit.
		ecs::PropertyDescriptor RevisionProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Revision");
			property.Type = ecs::PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ShaderSource>()});
			property.Writable = false;

			// **Empty, and deliberately not `Reads`.** `Writes` reaches the
			// bindings manifest, so a read-only property naming a component
			// there would tell every script author that setting it moves
			// storage it cannot even be given a value for -
			// `TrianglesCountProperty`'s own comment carries the argument.
			// Caught by `mono.tools/bindings` only once something finally
			// called `ShaderScriptClass()` from that tool's own bootstrap -
			// see `main.cpp`'s own comment on why nothing had, until now.
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const ShaderSource *held = store.Get<ShaderSource>(instance);
				if (held == nullptr) {
					return false;
				}
				*static_cast<int32_t *>(out) = static_cast<int32_t>(held->Revision);
				return true;
			};
			return property;
		}

		// The class, built once for the process.
		//
		// A function-local static in `ShaderScriptClass`, so the class exists
		// before the first caller reads an id from it - `RegisterScriptClasses`
		// is the pattern and carries the reason.
		ecs::ClassId RegisterShaderScriptClass() {
			// Through `EnsureClassTree` rather than registering `Instance`
			// again, and it registers this module's components on the way - a
			// class is a set of component ids and cannot be declared before
			// they exist.
			EnsureClassTree();

			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));

			// **An `Instance` and not a `PVInstance`**, for `Attachment`'s
			// reason: it has no place in the world of its own, and a
			// `Transform` on this row would be a second opinion about where the
			// thing that uses it is.
			const std::array source{ecs::Components::Of<ShaderSource>()};
			const ecs::ClassId script = ecs::Classes::Register("ShaderScript", instance, source);

			ecs::Classes::Computed(script, SourceProperty());
			ecs::Classes::Computed(script, RevisionProperty());
			return script;
		}

		ecs::ClassId RegisterLensShaderClass() {
			EnsureClassTree();

			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));
			const std::array source{ecs::Components::Of<ShaderSource>()};
			const ecs::ClassId lens = ecs::Classes::Register("LensShader", instance, source);

			// The source surface is intentionally shared. The class determines the
			// shader contract, while the revision-on-write rule remains one rule.
			ecs::Classes::Computed(lens, SourceProperty());
			ecs::Classes::Computed(lens, RevisionProperty());
			return lens;
		}
	}

	ecs::ClassId ShaderScriptClass() {
		static const ecs::ClassId script = RegisterShaderScriptClass();
		return script;
	}

	ecs::ClassId LensShaderClass() {
		static const ecs::ClassId lens = RegisterLensShaderClass();
		return lens;
	}

	ecs::Entity ShaderScriptNamed(ecs::Store &store, const core::Name &name) {
		if (!name.IsValid()) {
			return ecs::NULL_ENTITY;
		}

		// **The lowest entity id wins**, so a world holding two scripts of one
		// name resolves the same way every run. `ScriptsIn` sorts its result for
		// the same reason: an archetype walk reorders itself the first time an
		// unrelated component is added to one of the rows.
		ecs::Entity found = ecs::NULL_ENTITY;
		const ecs::ClassId scriptClass = ShaderScriptClass();
		store.Each<const ShaderSource>([&](ecs::Entity entity, const ShaderSource &) {
			if (!store.IsA(entity, scriptClass) || store.InstanceNameOf(entity) != name) {
				return;
			}
			if (found == ecs::NULL_ENTITY || entity.Id < found.Id) {
				found = entity;
			}
		});
		return found;
	}

	ecs::Entity LensShaderNamed(ecs::Store &store, const core::Name &name) {
		if (!name.IsValid()) {
			return ecs::NULL_ENTITY;
		}

		const ecs::ClassId lensClass = LensShaderClass();
		ecs::Entity found = ecs::NULL_ENTITY;
		store.Each<const ShaderSource>([&](ecs::Entity entity, const ShaderSource &) {
			if (!store.IsA(entity, lensClass) || store.InstanceNameOf(entity) != name) {
				return;
			}
			if (found == ecs::NULL_ENTITY || entity.Id < found.Id) {
				found = entity;
			}
		});
		return found;
	}

	ShaderText ShaderTextOf(ecs::Store &store, const core::Name &name) {
		const ecs::Entity script = ShaderScriptNamed(store, name);
		if (script == ecs::NULL_ENTITY) {
			return {};
		}

		const ShaderSource *held = store.Get<ShaderSource>(script);
		if (held == nullptr) {
			return {};
		}
		return ShaderText{.Code = held->Code, .Revision = held->Revision, .Found = true};
	}

	ShaderText LensShaderTextOf(ecs::Store &store, const core::Name &name) {
		const ecs::Entity shader = LensShaderNamed(store, name);
		if (shader == ecs::NULL_ENTITY) {
			return {};
		}

		const ShaderSource *held = store.Get<ShaderSource>(shader);
		if (held == nullptr) {
			return {};
		}
		return ShaderText{.Code = held->Code, .Revision = held->Revision, .Found = true};
	}

	bool SetShaderSource(ecs::Store &store, ecs::Entity script, std::string_view code) {
		ShaderSource *held = store.GetMutable<ShaderSource>(script);
		if (held == nullptr) {
			return false;
		}

		// **The counter moves even when the text is identical**, and that is
		// deliberate: comparing first would make the revision mean "the source
		// differs from last time", which is a second answer to a question the
		// library already asks by holding the revision it compiled. One writer,
		// one meaning - this counts writes.
		held->Code.assign(code);
		held->Revision++;
		return true;
	}

	size_t DemandedShaders(ecs::Store &store, std::vector<core::Name> &out) {
		out.clear();

		// **Walked over the materials**, which is what a world asks for rather
		// than what it holds - the header carries the argument. A `const` walk,
		// so this may be called from a read-only consumer without acquiring
		// anything.
		store.Each<const MaterialRef>([&out](ecs::Entity, const MaterialRef &material) {
			if (material.Shader.IsValid()) {
				out.push_back(material.Shader);
			}
		});

		// The screen's own shader, asked for the identical way - a resource
		// rather than a row, so it is a plain read rather than a walk.
		const core::Name postProcess = PostProcessShaderOf(store);
		if (postProcess.IsValid()) {
			out.push_back(postProcess);
		}

		// By id rather than by text: a `core::Name` is already an integer in
		// this process, and the order only has to be stable rather than
		// alphabetical.
		std::sort(out.begin(), out.end(), [](const core::Name &left, const core::Name &right) {
			return left.Id() < right.Id();
		});
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out.size();
	}

	core::Name PostProcessShaderOf(const ecs::Store &store) {
		const PostProcessing *held = store.Resource<PostProcessing>();
		return held == nullptr ? core::Name{} : held->Shader;
	}

	void SetPostProcessShader(ecs::Store &store, const core::Name &name) {
		PostProcessing *held = store.ResourceMutable<PostProcessing>();
		if (held == nullptr) {
			store.SetResource(PostProcessing{name});
			return;
		}
		held->Shader = name;
	}
}
