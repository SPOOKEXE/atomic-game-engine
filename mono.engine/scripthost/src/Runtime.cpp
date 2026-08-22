// The one switch on `Language`.
//
// **Here rather than in `script` because `script` may not name a VM, and not in
// either adapter because neither may name the other.** `MakeRuntime` lived in
// `script/src/Runtime.cpp` until v0.19, which is what made the module 33,000
// lines: the file that picks an implementation has to see both implementations,
// so both had to be in the same library, so the VM boundary could only ever be a
// naming convention checked by a bespoke sixty-line CMake closure walk.
//
// Moving this one function up a layer is what deleted that check. The boundary
// is a module boundary now, and the tier and layer checks enforce it for free.
//
// @tier L11 · shared

#include <engine/scripthost/Runtime.hpp>
#include <engine/scriptjs/Runtime.hpp>
#include <engine/scriptluau/Runtime.hpp>

namespace engine::script {

	std::unique_ptr<Runtime> MakeRuntime(ecs::Store &store, Language language, const RuntimeLimits &limits) {
		if (language == Language::JavaScript) {
			return MakeJavaScriptRuntime(store, limits);
		}
		return MakeLuauRuntime(store, limits);
	}
}
