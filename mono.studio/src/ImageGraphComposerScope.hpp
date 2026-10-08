#pragma once

#include <engine/imagegraph/ComposerScope.hpp>

namespace studio::detail {
	inline constexpr auto IMAGE_COMPOSER_SCOPE = engine::imagegraph::ComposerScope::ImageOnly;
	inline bool ImageGraphComposerTypeVisible(std::string_view type) {
		return engine::imagegraph::ComposerNodeEnabled(type, IMAGE_COMPOSER_SCOPE);
	}
	inline bool ImageGraphComposerExportEnabled(std::string_view extension) {
		return engine::imagegraph::ComposerExportEnabled(extension, IMAGE_COMPOSER_SCOPE);
	}
}
