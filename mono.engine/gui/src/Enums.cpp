#include <engine/gui/Enums.hpp>

namespace engine::gui {

	// One `Describe` per set, each a plain switch with no `default`.
	//
	// **No `default` on purpose.** A member added to an enum and not to its
	// switch is a compiler warning here, and the build treats it as an error;
	// with a `default` it would be a member that registers under whatever the
	// fallback string is, and two members sharing a name is an `EnumTable`
	// registration that quietly loses one of them.

	const char *Describe(SizeConstraint value) {
		switch (value) {
		case SizeConstraint::RelativeXY:
			return "RelativeXY";
		case SizeConstraint::RelativeXX:
			return "RelativeXX";
		case SizeConstraint::RelativeYY:
			return "RelativeYY";
		}
		return "RelativeXY";
	}

	const char *Describe(AutomaticSize value) {
		switch (value) {
		case AutomaticSize::None:
			return "None";
		case AutomaticSize::X:
			return "X";
		case AutomaticSize::Y:
			return "Y";
		case AutomaticSize::XY:
			return "XY";
		}
		return "None";
	}

	const char *Describe(BorderMode value) {
		switch (value) {
		case BorderMode::Outline:
			return "Outline";
		case BorderMode::Middle:
			return "Middle";
		case BorderMode::Inset:
			return "Inset";
		}
		return "Outline";
	}

	const char *Describe(ScaleType value) {
		switch (value) {
		case ScaleType::Stretch:
			return "Stretch";
		case ScaleType::Slice:
			return "Slice";
		case ScaleType::Tile:
			return "Tile";
		case ScaleType::Fit:
			return "Fit";
		case ScaleType::Crop:
			return "Crop";
		}
		return "Stretch";
	}

	const char *Describe(ResampleMode value) {
		switch (value) {
		case ResampleMode::Default:
			return "Default";
		case ResampleMode::Pixelated:
			return "Pixelated";
		}
		return "Default";
	}

	const char *Describe(TextXAlignment value) {
		switch (value) {
		case TextXAlignment::Left:
			return "Left";
		case TextXAlignment::Center:
			return "Center";
		case TextXAlignment::Right:
			return "Right";
		}
		return "Center";
	}

	const char *Describe(TextYAlignment value) {
		switch (value) {
		case TextYAlignment::Top:
			return "Top";
		case TextYAlignment::Center:
			return "Center";
		case TextYAlignment::Bottom:
			return "Bottom";
		}
		return "Center";
	}

	const char *Describe(TextTruncate value) {
		switch (value) {
		case TextTruncate::None:
			return "None";
		case TextTruncate::AtEnd:
			return "AtEnd";
		}
		return "None";
	}

	const char *Describe(FontFace value) {
		switch (value) {
		case FontFace::Regular:
			return "Regular";
		case FontFace::Bold:
			return "Bold";
		case FontFace::Italic:
			return "Italic";
		case FontFace::Code:
			return "Code";
		}
		return "Regular";
	}

	const char *Describe(FillDirection value) {
		switch (value) {
		case FillDirection::Horizontal:
			return "Horizontal";
		case FillDirection::Vertical:
			return "Vertical";
		}
		return "Vertical";
	}

	const char *Describe(HorizontalAlignment value) {
		switch (value) {
		case HorizontalAlignment::Left:
			return "Left";
		case HorizontalAlignment::Center:
			return "Center";
		case HorizontalAlignment::Right:
			return "Right";
		}
		return "Left";
	}

	const char *Describe(VerticalAlignment value) {
		switch (value) {
		case VerticalAlignment::Top:
			return "Top";
		case VerticalAlignment::Center:
			return "Center";
		case VerticalAlignment::Bottom:
			return "Bottom";
		}
		return "Top";
	}

	const char *Describe(SortOrder value) {
		switch (value) {
		case SortOrder::Name:
			return "Name";
		case SortOrder::Custom:
			return "Custom";
		case SortOrder::LayoutOrder:
			return "LayoutOrder";
		}
		return "LayoutOrder";
	}

	const char *Describe(StartCorner value) {
		switch (value) {
		case StartCorner::TopLeft:
			return "TopLeft";
		case StartCorner::TopRight:
			return "TopRight";
		case StartCorner::BottomLeft:
			return "BottomLeft";
		case StartCorner::BottomRight:
			return "BottomRight";
		}
		return "TopLeft";
	}

	const char *Describe(AspectType value) {
		switch (value) {
		case AspectType::FitWithinMaxSize:
			return "FitWithinMaxSize";
		case AspectType::ScaleWithParentSize:
			return "ScaleWithParentSize";
		}
		return "FitWithinMaxSize";
	}

	const char *Describe(DominantAxis value) {
		switch (value) {
		case DominantAxis::Width:
			return "Width";
		case DominantAxis::Height:
			return "Height";
		}
		return "Width";
	}

	const char *Describe(ScrollingDirection value) {
		switch (value) {
		case ScrollingDirection::X:
			return "X";
		case ScrollingDirection::Y:
			return "Y";
		case ScrollingDirection::XY:
			return "XY";
		}
		return "Y";
	}

	const char *Describe(NodePortDirection value) {
		switch (value) {
		case NodePortDirection::Input:
			return "Input";
		case NodePortDirection::Output:
			return "Output";
		}
		return "Input";
	}

	const char *Describe(NodeBypassMode value) {
		switch (value) {
		case NodeBypassMode::None:
			return "None";
		case NodeBypassMode::Bypass:
			return "Bypass";
		}
		return "None";
	}

	const char *Describe(NodePortEdge value) {
		switch (value) {
		case NodePortEdge::Top:
			return "Top";
		case NodePortEdge::Bottom:
			return "Bottom";
		case NodePortEdge::Corner:
			return "Corner";
		}
		return "Top";
	}

	const char *Describe(InputPortLayout value) {
		switch (value) {
		case InputPortLayout::Manual:
			return "Manual";
		case InputPortLayout::Separate:
			return "Separate";
		case InputPortLayout::Squash:
			return "Squash";
		}
		return "Manual";
	}

	const char *Describe(NodeGroupLayout value) {
		switch (value) {
		case NodeGroupLayout::Manual:
			return "Manual";
		case NodeGroupLayout::AroundEdge:
			return "AroundEdge";
		case NodeGroupLayout::SmallestSpace:
			return "SmallestSpace";
		}
		return "Manual";
	}

	const char *Describe(StrokeMode value) {
		switch (value) {
		case StrokeMode::Contextual:
			return "Contextual";
		case StrokeMode::Border:
			return "Border";
		}
		return "Contextual";
	}

	const char *Describe(LineJoin value) {
		switch (value) {
		case LineJoin::Round:
			return "Round";
		case LineJoin::Bevel:
			return "Bevel";
		case LineJoin::Miter:
			return "Miter";
		}
		return "Round";
	}

	const char *Describe(StrokeSizing value) {
		switch (value) {
		case StrokeSizing::FixedSize:
			return "FixedSize";
		case StrokeSizing::ScaledSize:
			return "ScaledSize";
		}
		return "FixedSize";
	}

	const char *Describe(DragStyle value) {
		switch (value) {
		case DragStyle::TranslatePlane:
			return "TranslatePlane";
		case DragStyle::TranslateLine:
			return "TranslateLine";
		case DragStyle::TranslateLineOrPlane:
			return "TranslateLineOrPlane";
		case DragStyle::Rotate:
			return "Rotate";
		case DragStyle::Scriptable:
			return "Scriptable";
		}
		return "TranslatePlane";
	}

	const char *Describe(DragResponse value) {
		switch (value) {
		case DragResponse::Offset:
			return "Offset";
		case DragResponse::Scale:
			return "Scale";
		case DragResponse::CustomOffset:
			return "CustomOffset";
		case DragResponse::CustomScale:
			return "CustomScale";
		}
		return "Offset";
	}

	const char *Describe(ElasticBehavior value) {
		switch (value) {
		case ElasticBehavior::WhenScrollable:
			return "WhenScrollable";
		case ElasticBehavior::Always:
			return "Always";
		case ElasticBehavior::Never:
			return "Never";
		}
		return "WhenScrollable";
	}

	const char *Describe(ScrollBarInset value) {
		switch (value) {
		case ScrollBarInset::None:
			return "None";
		case ScrollBarInset::ScrollBar:
			return "ScrollBar";
		case ScrollBarInset::Always:
			return "Always";
		}
		return "None";
	}

	const char *Describe(BarPosition value) {
		switch (value) {
		case BarPosition::Right:
			return "Right";
		case BarPosition::Left:
			return "Left";
		}
		return "Right";
	}

	const char *Describe(FlexAlignment value) {
		switch (value) {
		case FlexAlignment::None:
			return "None";
		case FlexAlignment::Fill:
			return "Fill";
		case FlexAlignment::SpaceAround:
			return "SpaceAround";
		case FlexAlignment::SpaceBetween:
			return "SpaceBetween";
		case FlexAlignment::SpaceEvenly:
			return "SpaceEvenly";
		}
		return "None";
	}

	const char *Describe(ItemLineAlignment value) {
		switch (value) {
		case ItemLineAlignment::Automatic:
			return "Automatic";
		case ItemLineAlignment::Start:
			return "Start";
		case ItemLineAlignment::Center:
			return "Center";
		case ItemLineAlignment::End:
			return "End";
		case ItemLineAlignment::Stretch:
			return "Stretch";
		}
		return "Automatic";
	}

	const char *Describe(FlexMode value) {
		switch (value) {
		case FlexMode::None:
			return "None";
		case FlexMode::Grow:
			return "Grow";
		case FlexMode::Shrink:
			return "Shrink";
		case FlexMode::Fill:
			return "Fill";
		case FlexMode::Custom:
			return "Custom";
		}
		return "None";
	}

	const char *Describe(ZIndexBehavior value) {
		switch (value) {
		case ZIndexBehavior::Global:
			return "Global";
		case ZIndexBehavior::Sibling:
			return "Sibling";
		}
		return "Sibling";
	}

	const char *Describe(SurfaceSizingMode value) {
		switch (value) {
		case SurfaceSizingMode::FixedSize:
			return "FixedSize";
		case SurfaceSizingMode::PixelsPerStud:
			return "PixelsPerStud";
		}
		return "FixedSize";
	}

	const char *Describe(Face value) {
		switch (value) {
		case Face::Right:
			return "Right";
		case Face::Top:
			return "Top";
		case Face::Back:
			return "Back";
		case Face::Left:
			return "Left";
		case Face::Bottom:
			return "Bottom";
		case Face::Front:
			return "Front";
		}
		return "Front";
	}
}
