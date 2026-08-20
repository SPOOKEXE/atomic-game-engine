#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/RichText.hpp>
#include <engine/gui/Services.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::gui {

	namespace {
		using core::Color3;
		using core::Rect;
		using core::UDim;
		using core::UDim2;
		using core::Vector2;
		using core::Vector3;
		using ecs::ClassId;
		using ecs::Entity;
		using ecs::Hierarchy;
		using ecs::InstanceName;
		using ecs::Store;

		// --- the fold --------------------------------------------------------
		//
		// `studio/Hierarchy.cpp`'s, kept identical on purpose: the same
		// constants, the same finaliser, the same order dependence. Two
		// signatures in one repository that mix differently are two things a
		// reviewer has to hold separately, and neither is more correct.

		constexpr uint64_t GOLDEN = 0x9E3779B97F4A7C15ull;

		// splitmix64's finaliser: the cheapest mix that avalanches every input
		// bit across all sixty-four output ones.
		//
		// **Not `std::hash`.** The standard says nothing about what it
		// produces, so two builds of this program may disagree. Nothing here
		// crosses a process - the comparison is this frame against the last -
		// but a hash whose value is a property of the compiler is one nobody
		// can write a test for, and the test is what keeps the field list
		// honest.
		constexpr uint64_t Scramble(uint64_t value) {
			value ^= value >> 30;
			value *= 0xBF58476D1CE4E5B9ull;
			value ^= value >> 27;
			value *= 0x94D049BB133111EBull;
			value ^= value >> 31;
			return value;
		}

		// Folds one term in, order included.
		//
		// **Order-dependent deliberately.** Two archetypes that swap their rows
		// hold the same elements and would compile identically, so an
		// order-independent fold would be the more accurate answer - and would
		// also collide far more readily, because commutative folds do. A
		// reshuffle costs one rebuild nobody sees; a collision is a UI showing
		// what is no longer there.
		constexpr uint64_t Fold(uint64_t running, uint64_t term) {
			return (running ^ Scramble(term)) * GOLDEN;
		}

		// **Floats fold by their bits, not by their value.** `std::bit_cast`
		// rather than a cast to an integer, which would round - so a position
		// moving by half a pixel would hash the same and the panel would keep
		// the old rectangle. It also means `-0.0` and `0.0` fold differently
		// and cost one rebuild, which is the safe direction.
		uint64_t Fold(uint64_t running, float value) {
			return Fold(running, static_cast<uint64_t>(std::bit_cast<uint32_t>(value)));
		}

		uint64_t Fold(uint64_t running, bool value) {
			// **1 and 2, not 1 and 0.** A zero term folds to a value that
			// depends only on the running total, so a `false` next to a
			// missing field would be indistinguishable - and the whole job here
			// is telling those apart.
			return Fold(running, static_cast<uint64_t>(value ? 1 : 2));
		}

		uint64_t Fold(uint64_t running, int32_t value) {
			return Fold(running, static_cast<uint64_t>(static_cast<uint32_t>(value)));
		}

		uint64_t Fold(uint64_t running, const core::Name &value) {
			// The interned id, not the text. Two different strings never share
			// an id within a process, and the comparison never leaves one.
			return Fold(running, static_cast<uint64_t>(value.Id()));
		}

		// **Every byte, because an owned string has no id to stand in for it.**
		// That is the cost of `Label::Text` no longer interning, and it is the
		// right one to pay here: this hash is what stops the list being rebuilt,
		// and a label whose text changed by one character has to be seen to have
		// changed. Folding the length alone would keep a stale rectangle on
		// screen for a score that went from 19 to 91.
		//
		// The length is folded first so that "ab" and "a" + "b" in adjacent
		// fields cannot collide.
		uint64_t Fold(uint64_t running, const std::string &value) {
			running = Fold(running, static_cast<uint64_t>(value.size()));
			for (const char letter : value) {
				running = Fold(running, static_cast<uint64_t>(static_cast<unsigned char>(letter)));
			}
			return running;
		}

		uint64_t Fold(uint64_t running, Entity value) {
			return Fold(running, static_cast<uint64_t>(value.Id));
		}

		uint64_t Fold(uint64_t running, const Vector2 &value) {
			return Fold(Fold(running, value.X), value.Y);
		}

		uint64_t Fold(uint64_t running, const Vector3 &value) {
			return Fold(Fold(Fold(running, value.X), value.Y), value.Z);
		}

		uint64_t Fold(uint64_t running, const Color3 &value) {
			return Fold(Fold(Fold(running, value.R), value.G), value.B);
		}

		uint64_t Fold(uint64_t running, const UDim &value) {
			return Fold(Fold(running, value.Scale), value.Offset);
		}

		uint64_t Fold(uint64_t running, const UDim2 &value) {
			return Fold(Fold(running, value.X), value.Y);
		}

		uint64_t Fold(uint64_t running, const Rect &value) {
			return Fold(Fold(running, value.Min), value.Max);
		}

		// Every enum in this module, by its stored ordinal.
		template <class E>
			requires std::is_enum_v<E>
		uint64_t Fold(uint64_t running, E value) {
			return Fold(running, static_cast<uint64_t>(static_cast<uint8_t>(value)));
		}

		// --- one fold per component ------------------------------------------
		//
		// **Every field of every one, in declaration order.** The table in
		// `Compile.hpp` states the rule and `gui/tests/Compile.cpp` enforces it
		// by writing every declared property and asserting the signature moved
		// - which is what makes this a check rather than a comment.
		//
		// `Resolved` is deliberately absent: it is what the compile *produces*,
		// so folding it in would make the signature depend on its own output.

		uint64_t Fold(uint64_t running, const Element &value) {
			running = Fold(running, value.Position);
			running = Fold(running, value.Size);
			running = Fold(running, value.AnchorPoint);
			running = Fold(running, value.Rotation);
			running = Fold(running, value.ZIndex);
			running = Fold(running, value.LayoutOrder);
			running = Fold(running, value.Visible);
			running = Fold(running, value.ClipsDescendants);
			running = Fold(running, value.Active);
			running = Fold(running, value.Selectable);
			running = Fold(running, value.Interactable);
			running = Fold(running, value.Constraint);
			return Fold(running, value.Automatic);
		}

		// **The service's row, because the selection decides what is drawn.**
		// A highlight that appeared only when something *else* changed would be
		// the stale-list failure `Compile.hpp`'s table exists to prevent, one
		// component further out than the usual case.
		uint64_t Fold(uint64_t running, const GuiServiceState &value) {
			running = Fold(running, value.SelectedObject);
			running = Fold(running, value.FocusedTextBox);
			running = Fold(running, value.MenuIsOpen);
			return Fold(running, value.AutoSelectGuiEnabled);
		}

		uint64_t Fold(uint64_t running, const Selection &value) {
			running = Fold(running, value.NextUp);
			running = Fold(running, value.NextDown);
			running = Fold(running, value.NextLeft);
			running = Fold(running, value.NextRight);
			running = Fold(running, value.ImageObject);
			return Fold(running, value.Order);
		}

		uint64_t Fold(uint64_t running, const Background &value) {
			running = Fold(running, value.Color);
			running = Fold(running, value.Transparency);
			running = Fold(running, value.BorderColor);
			running = Fold(running, value.BorderSizePixel);
			return Fold(running, value.Border);
		}

		uint64_t Fold(uint64_t running, const Label &value) {
			running = Fold(running, value.Text);
			running = Fold(running, value.Color);
			running = Fold(running, value.Transparency);
			running = Fold(running, value.Size);
			running = Fold(running, value.Font);
			running = Fold(running, value.XAlignment);
			running = Fold(running, value.YAlignment);
			running = Fold(running, value.Wrapped);
			running = Fold(running, value.Scaled);
			running = Fold(running, value.Truncate);
			running = Fold(running, value.StrokeColor);
			running = Fold(running, value.StrokeTransparency);
			running = Fold(running, value.LineHeight);
			running = Fold(running, value.MaxVisible);
			return Fold(running, value.Rich);
		}

		uint64_t Fold(uint64_t running, const Picture &value) {
			running = Fold(running, value.Image);
			running = Fold(running, value.Color);
			running = Fold(running, value.Transparency);
			running = Fold(running, value.Scale);
			running = Fold(running, value.SliceCenter);
			running = Fold(running, value.SliceScale);
			running = Fold(running, value.TileSize);
			running = Fold(running, value.RectOffset);
			running = Fold(running, value.RectSize);
			running = Fold(running, value.Shader);
			running = Fold(running, value.HoverImage);
			running = Fold(running, value.PressedImage);
			return Fold(running, value.Resample);
		}

		uint64_t Fold(uint64_t running, const Button &value) {
			return Fold(running, value.AutoButtonColor);
		}

		uint64_t Fold(uint64_t running, const Scrolling &value) {
			running = Fold(running, value.CanvasSize);
			running = Fold(running, value.CanvasPosition);
			running = Fold(running, value.BarColor);
			running = Fold(running, value.TopImage);
			running = Fold(running, value.MidImage);
			running = Fold(running, value.BottomImage);
			running = Fold(running, value.BarThickness);
			running = Fold(running, value.BarTransparency);
			running = Fold(running, value.Enabled);
			running = Fold(running, value.Direction);
			running = Fold(running, value.AutomaticCanvas);
			running = Fold(running, value.Elastic);
			running = Fold(running, value.HorizontalInset);
			running = Fold(running, value.VerticalInset);
			return Fold(running, value.VerticalBar);
		}

		uint64_t Fold(uint64_t running, const Entry &value) {
			running = Fold(running, value.PlaceholderText);
			running = Fold(running, value.PlaceholderColor);
			running = Fold(running, value.ClearTextOnFocus);
			running = Fold(running, value.MultiLine);
			running = Fold(running, value.TextEditable);
			running = Fold(running, value.CursorPosition);
			return Fold(running, value.SelectionStart);
		}

		uint64_t Fold(uint64_t running, const Layer &value) {
			running = Fold(running, value.Enabled);
			running = Fold(running, value.DisplayOrder);
			running = Fold(running, value.Behavior);
			running = Fold(running, value.ResetOnSpawn);
			return Fold(running, value.IgnoreGuiInset);
		}

		uint64_t Fold(uint64_t running, const Surface &value) {
			running = Fold(running, value.Adornee);
			running = Fold(running, value.On);
			running = Fold(running, value.Sizing);
			running = Fold(running, value.PixelsPerStud);
			running = Fold(running, value.CanvasSize);
			running = Fold(running, value.AlwaysOnTop);
			running = Fold(running, value.LightInfluence);
			running = Fold(running, value.Brightness);
			running = Fold(running, value.ZOffset);
			running = Fold(running, value.MaxDistance);
			running = Fold(running, value.ClipsDescendants);
			return Fold(running, value.Active);
		}

		uint64_t Fold(uint64_t running, const Billboard &value) {
			running = Fold(running, value.Adornee);
			running = Fold(running, value.PlayerToHideFrom);
			running = Fold(running, value.Size);
			running = Fold(running, value.StudsOffset);
			running = Fold(running, value.StudsOffsetWorldSpace);
			running = Fold(running, value.ExtentsOffset);
			running = Fold(running, value.ExtentsOffsetWorldSpace);
			running = Fold(running, value.SizeOffset);
			running = Fold(running, value.AlwaysOnTop);
			running = Fold(running, value.LightInfluence);
			running = Fold(running, value.Brightness);
			running = Fold(running, value.MaxDistance);
			running = Fold(running, value.DistanceStep);
			running = Fold(running, value.ClipsDescendants);
			return Fold(running, value.Active);
		}

		uint64_t Fold(uint64_t running, const Group &value) {
			running = Fold(running, value.Color);
			return Fold(running, value.Transparency);
		}

		uint64_t Fold(uint64_t running, const Viewport &value) {
			running = Fold(running, value.CurrentCamera);
			running = Fold(running, value.Ambient);
			running = Fold(running, value.LightColor);
			running = Fold(running, value.LightDirection);
			running = Fold(running, value.Color);
			return Fold(running, value.Transparency);
		}

		uint64_t Fold(uint64_t running, const Padding &value) {
			running = Fold(running, value.Top);
			running = Fold(running, value.Bottom);
			running = Fold(running, value.Left);
			return Fold(running, value.Right);
		}

		uint64_t Fold(uint64_t running, const ListLayout &value) {
			running = Fold(running, value.Direction);
			running = Fold(running, value.Padding);
			running = Fold(running, value.Horizontal);
			running = Fold(running, value.Vertical);
			running = Fold(running, value.Order);
			running = Fold(running, value.HorizontalFlex);
			running = Fold(running, value.VerticalFlex);
			running = Fold(running, value.ItemLine);
			return Fold(running, value.Wraps);
		}

		uint64_t Fold(uint64_t running, const GridLayout &value) {
			running = Fold(running, value.CellSize);
			running = Fold(running, value.CellPadding);
			running = Fold(running, value.Direction);
			running = Fold(running, value.MaxCells);
			running = Fold(running, value.Corner);
			running = Fold(running, value.Horizontal);
			running = Fold(running, value.Vertical);
			return Fold(running, value.Order);
		}

		uint64_t Fold(uint64_t running, const DragDetector &value) {
			running = Fold(running, value.BoundingUI);
			running = Fold(running, value.Axis);
			running = Fold(running, value.MinTranslation);
			running = Fold(running, value.MaxTranslation);
			running = Fold(running, value.Style);
			running = Fold(running, value.Response);
			return Fold(running, value.Enabled);
		}

		uint64_t Fold(uint64_t running, const TableLayout &value) {
			running = Fold(running, value.Padding);
			running = Fold(running, value.Direction);
			running = Fold(running, value.Horizontal);
			running = Fold(running, value.Vertical);
			running = Fold(running, value.Order);
			running = Fold(running, value.FillEmptySpaceColumns);
			return Fold(running, value.FillEmptySpaceRows);
		}

		uint64_t Fold(uint64_t running, const PageLayout &value) {
			running = Fold(running, value.CurrentPage);
			running = Fold(running, value.Padding);
			running = Fold(running, value.Direction);
			running = Fold(running, value.Order);
			running = Fold(running, value.Circular);
			running = Fold(running, value.Animated);
			running = Fold(running, value.TweenTime);
			running = Fold(running, value.Easing);
			return Fold(running, value.EasingWay);
		}

		// **The two motion rows are folded, and that is what makes an animation
		// draw at all.** The signature decides whether a frame recompiles; a
		// carousel that slid without moving the hash would draw its first frame
		// and then sit there until something else in the tree changed.
		//
		// Folding the *resolved* number rather than the clock is what keeps a
		// still interface still: `Alpha` and `Overshoot` stop changing the
		// moment a slide lands or a spring settles, so a rebuild-per-frame
		// lasts exactly as long as the motion does. Folding
		// `CompileRequest::Seconds` instead would rebuild every frame forever.
		uint64_t Fold(uint64_t running, const PageMotion &value) {
			running = Fold(running, value.From);
			running = Fold(running, value.To);
			return Fold(running, value.Alpha);
		}

		uint64_t Fold(uint64_t running, const ScrollMotion &value) {
			return Fold(running, value.Overshoot);
		}

		uint64_t Fold(uint64_t running, const AspectRatio &value) {
			running = Fold(running, value.Ratio);
			running = Fold(running, value.Type);
			return Fold(running, value.Dominant);
		}

		uint64_t Fold(uint64_t running, const SizeLimits &value) {
			running = Fold(running, value.Min);
			return Fold(running, value.Max);
		}

		uint64_t Fold(uint64_t running, const TextSizeLimits &value) {
			running = Fold(running, value.Min);
			return Fold(running, value.Max);
		}

		uint64_t Fold(uint64_t running, const Corner &value) {
			return Fold(running, value.Radius);
		}

		uint64_t Fold(uint64_t running, const Stroke &value) {
			running = Fold(running, value.Color);
			running = Fold(running, value.Thickness);
			running = Fold(running, value.Transparency);
			running = Fold(running, value.Enabled);
			running = Fold(running, value.Apply);
			running = Fold(running, value.Join);
			return Fold(running, value.Sizing);
		}

		uint64_t Fold(uint64_t running, const Scale &value) {
			return Fold(running, value.Factor);
		}

		// **The whole sequence and not just its count.** A ramp edited keypoint
		// by keypoint is the ordinary way one is authored, and folding the count
		// alone would leave the panel showing the previous colours until
		// something unrelated moved. The unused tail is deliberately skipped:
		// `Count` says how many keypoints matter and two ramps that agree on
		// those are the same ramp however the array behind them was reached.
		uint64_t Fold(uint64_t running, const core::ColorSequence &value) {
			running = Fold(running, static_cast<uint64_t>(value.Count));
			for (uint32_t index = 0; index < value.Count; index++) {
				running = Fold(running, value.Keypoints[index].Time);
				running = Fold(running, value.Keypoints[index].Value);
			}
			return running;
		}

		uint64_t Fold(uint64_t running, const core::NumberSequence &value) {
			running = Fold(running, static_cast<uint64_t>(value.Count));
			for (uint32_t index = 0; index < value.Count; index++) {
				running = Fold(running, value.Keypoints[index].Time);
				running = Fold(running, value.Keypoints[index].Value);
				running = Fold(running, value.Keypoints[index].Envelope);
			}
			return running;
		}

		uint64_t Fold(uint64_t running, const Gradient &value) {
			running = Fold(running, value.Color);
			running = Fold(running, value.Transparency);
			running = Fold(running, value.Offset);
			running = Fold(running, value.Rotation);
			return Fold(running, value.Enabled);
		}

		uint64_t Fold(uint64_t running, const FlexItem &value) {
			running = Fold(running, value.GrowRatio);
			running = Fold(running, value.ShrinkRatio);
			running = Fold(running, value.Mode);
			return Fold(running, value.ItemLine);
		}

		// One pass over every row carrying `T`, folding the row's identity, its
		// place in the tree and the component itself.
		//
		// **The tree links come along with every component**, so a node with an
		// `Element` and a `Background` folds them twice. That is one extra
		// multiply per row against having to remember a separate hierarchy pass
		// restricted to exactly the right set of rows - and a pass that covered
		// *every* instance would rebuild the UI whenever a part moved.
		template <class T> uint64_t FoldRows(Store &store, uint64_t running) {
			store.Each<const T, const Hierarchy>(
				[&](Entity entity, const T &component, const Hierarchy &node) {
					running = Fold(running, entity);
					running = Fold(running, node.Parent);
					running = Fold(running, node.FirstChild);
					running = Fold(running, node.NextSibling);
					running = Fold(running, component);
				}
			);
			return running;
		}

		// --- the flatten -----------------------------------------------------

		struct Ids {
			ClassId Object;
			ClassId Collector;

			Ids() {
				RegisterGuiClasses();
				Object = GuiClass("GuiObject");
				Collector = GuiClass("LayerCollector");
			}
		};

		const Ids &Classes() {
			static const Ids ids;
			return ids;
		}

		// How far a hovered or pressed button's fill shifts.
		//
		// Press moves further than hover and in the same direction, so holding
		// a button reads as more of what hovering it started rather than as a
		// reversal. Named here rather than written at the point of use, which
		// is `ui/Theme.cpp`'s rule: a literal at a point of use is one that
		// cannot be changed without finding every copy.
		constexpr float HOVER_SHIFT = 0.12f;
		constexpr float PRESS_SHIFT = 0.20f;

		Color3 Shift(const Color3 &colour, float amount) {
			return Color3{
				std::clamp(colour.R + amount, 0.0f, 1.0f),
				std::clamp(colour.G + amount, 0.0f, 1.0f),
				std::clamp(colour.B + amount, 0.0f, 1.0f),
			};
		}

		// Which way an `AutoButtonColor` shift goes for a given fill.
		//
		// **Away from the end the fill is already at, rather than always
		// lighter.** Lightening on hover is the obvious rule and it is
		// invisible on the one button that matters most: `Background::Color`
		// defaults to white, so `1.0 + 0.12` clamps back to `1.0` and hovering
		// a freshly created `TextButton` did nothing at all. Always darkening
		// has the same hole at the other end, against a black fill. Choosing by
		// luminance is what gives every fill somewhere to go, and it is one
		// branch rather than a special case per palette.
		//
		// Rec. 709 weights, because a shift judged by eye is judged against
		// perceived brightness - a saturated green reads as light and a
		// saturated blue as dark at the same numeric value.
		float ShiftDirection(const Color3 &colour) {
			const float luminance = 0.2126f * colour.R + 0.7152f * colour.G + 0.0722f * colour.B;
			return luminance > 0.5f ? -1.0f : 1.0f;
		}

		// The border rectangle for a `BorderMode`.
		//
		// Three answers rather than one because the choice decides whether two
		// elements sized to touch overlap by a pixel - which is the difference
		// between a table with hairlines and a table with double rules.
		// A stroke's thickness in pixels, whatever it was authored in.
		//
		// **`ScaledSize` measures against the smaller side rather than the
		// area or the diagonal.** That is Roblox's rule and it is the one that
		// behaves: an outline scaled by a long thin element's width would be
		// thicker than the element is tall, and the ring would swallow it.
		//
		// @param stroke    What was authored.
		// @param reference The length a fraction is of - the smaller of the
		//        element's sides, or its text size when the glyphs took it.
		// @return Pixels, never negative.
		float StrokeThickness(const Stroke &stroke, float reference) {
			if (stroke.Sizing != StrokeSizing::ScaledSize) {
				return std::max(stroke.Thickness, 0.0f);
			}
			return std::max(stroke.Thickness, 0.0f) * std::max(reference, 0.0f);
		}

		Rect BorderRect(const Rect &bounds, BorderMode mode, float thickness) {
			const float half = thickness * 0.5f;
			switch (mode) {
			case BorderMode::Outline:
				return Rect{
					Vector2{bounds.Min.X - half, bounds.Min.Y - half},
					Vector2{bounds.Max.X + half, bounds.Max.Y + half}
				};
			case BorderMode::Inset:
				return Rect{
					Vector2{bounds.Min.X + half, bounds.Min.Y + half},
					Vector2{bounds.Max.X - half, bounds.Max.Y - half}
				};
			case BorderMode::Middle:
				return bounds;
			}
			return bounds;
		}

		// Turns an authored ramp into the two ends of the line it runs along.
		//
		// **The ends snap to the element's edges along the rotated axis**, which
		// is Roblox's rule and the one that makes a rotation read as a rotation:
		// a gradient turned ninety degrees ramps over the full *height* rather
		// than over a diagonal of the width. The extent of an axis-aligned
		// rectangle projected onto a direction is `|w·dx| + |h·dy|`, which is the
		// whole of the arithmetic below.
		//
		// **Resolved here rather than in a backend** for `DrawList`'s stated
		// reason: everything a backend receives is in canvas pixels, so an angle
		// and a size-relative offset would be two things it had to resolve
		// against a rectangle - and two backends resolving them would be two
		// answers to where the ramp starts.
		//
		// @return The index into `out.Gradients`, or -1 when there is no ramp.
		int32_t ResolveGradient(const Gradient *gradient, const Rect &bounds, DrawList &out) {
			if (gradient == nullptr || !gradient->Enabled) {
				return -1;
			}

			constexpr float TO_RADIANS = 3.14159265f / 180.0f;
			const float angle = gradient->Rotation * TO_RADIANS;
			const Vector2 direction{std::cos(angle), std::sin(angle)};

			const float width = bounds.Width();
			const float height = bounds.Height();
			const float extent = std::abs(width * direction.X) + std::abs(height * direction.Y);

			const Vector2 centre{
				(bounds.Min.X + bounds.Max.X) * 0.5f + gradient->Offset.X * width,
				(bounds.Min.Y + bounds.Max.Y) * 0.5f + gradient->Offset.Y * height,
			};

			DrawGradient resolved;
			resolved.Color = gradient->Color;
			resolved.Transparency = gradient->Transparency;
			resolved.Origin = Vector2{
				centre.X - direction.X * extent * 0.5f,
				centre.Y - direction.Y * extent * 0.5f,
			};
			resolved.Axis = Vector2{direction.X * extent, direction.Y * extent};

			out.Gradients.push_back(resolved);
			return static_cast<int32_t>(out.Gradients.size() - 1);
		}

		// Everything one element contributes to the list.
		void Emit(
			const Store &store,
			Entity instance,
			Entity collector,
			const Resolved &resolved,
			const CompileRequest &request,
			const Color3 &groupTint,
			float groupOpacity,
			DrawList &out
		) {
			const Element *element = store.Get<Element>(instance);
			if (element == nullptr) {
				return;
			}

			const Rect bounds{
				resolved.AbsolutePosition,
				Vector2{
					resolved.AbsolutePosition.X + resolved.AbsoluteSize.X,
					resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y,
				}
			};

			// An element clipped to nothing emits nothing. Cheaper than four
			// commands a backend will scissor away, and it makes the command
			// count mean "what is on screen".
			if (resolved.Clip.Intersection(bounds).Empty()) {
				return;
			}

			// **And an element wholly off the canvas emits nothing either, even
			// when nothing clips it.** That is Roblox's rule for a `SurfaceGui`
			// or a `BillboardGui` with `ClipsDescendants` off: hanging over the
			// edge is allowed and being somewhere else entirely is not. The clip
			// above cannot say so, because on such a collector it is deliberately
			// a rectangle that cuts nothing - so the canvas is asked directly,
			// which is one lookup on a path that already makes several.
			if (const Canvas *area = store.Get<Canvas>(collector);
				area != nullptr && area->Area.Intersection(bounds).Empty()) {
				return;
			}

			out.Elements++;

			float radius = 0.0f;
			const Stroke *stroke = nullptr;
			Entity strokeInstance;
			const Gradient *gradient = nullptr;
			store.EachChild(instance, [&](Entity child) {
				if (const Corner *corner = store.Get<Corner>(child)) {
					// Against the *smaller* axis, which is what stops a scale
					// of 0.5 turning a wide element into an ellipse.
					radius =
						corner->Radius.Resolve(std::min(resolved.AbsoluteSize.X, resolved.AbsoluteSize.Y));
				}
				if (stroke == nullptr) {
					if (const Stroke *found = store.Get<Stroke>(child)) {
						stroke = found;
						strokeInstance = child;
					}
				}
				if (gradient == nullptr) {
					gradient = store.Get<Gradient>(child);
				}
			});

			// **One resolve for the element and one for its outline, and the
			// second is a gradient one level deeper.** Roblox's arrangement: a
			// `UIGradient` under a `GuiObject` ramps what that object draws, and
			// one under a `UIStroke` ramps the stroke. The same component either
			// way, so an outline that fades along its length costs no second set
			// of properties.
			const int32_t ramp = ResolveGradient(gradient, bounds, out);

			// **On `base`, so every command this element emits inherits it.** The
			// exception is the `UIStroke` outline at the bottom, which takes its
			// own or none - and it is written as an override there rather than as
			// an omission here, because a command that quietly did not inherit
			// would be the one nobody notices is unramped.
			DrawCommand base;
			base.Gradient = ramp;
			base.Source = instance;
			base.Collector = collector;
			base.Spatial = store.Get<SpatialCanvas>(collector) != nullptr;
			base.Bounds = bounds;
			base.Clip = resolved.Clip;
			base.Rotation = resolved.AbsoluteRotation;
			base.CornerRadius = radius;

			const auto tintByGroup = [&](const Color3 &colour) {
				return Color3{
					colour.R * groupTint.R,
					colour.G * groupTint.G,
					colour.B * groupTint.B,
				};
			};
			const auto fadeByGroup = [&](float transparency) {
				return 1.0f - (1.0f - std::clamp(transparency, 0.0f, 1.0f)) * groupOpacity;
			};

			if (const Background *background = store.Get<Background>(instance);
				background != nullptr && background->Transparency < 1.0f) {
				Color3 fill = background->Color;

				// **Applied here and never stored back**, so `BackgroundColor3`
				// reads what the author wrote however the pointer is behaving.
				// `Button::AutoButtonColor` says so at the field.
				// **`Interactable` stops the shift as well as the event.** A
				// greyed-out button that still lit up under the pointer would be
				// telling a person it is pressable while refusing to be pressed,
				// which is the one thing the property exists to prevent.
				if (const Button *button = store.Get<Button>(instance);
					button != nullptr && button->AutoButtonColor && element->Interactable) {
					const float direction = ShiftDirection(fill);
					if (instance == request.Pressed) {
						fill = Shift(fill, direction * PRESS_SHIFT);
					} else if (instance == request.Hovered) {
						fill = Shift(fill, direction * HOVER_SHIFT);
					}
				}

				DrawCommand rectangle = base;
				rectangle.Kind = DrawKind::Rectangle;
				rectangle.Tint = tintByGroup(fill);
				rectangle.Transparency = fadeByGroup(background->Transparency);
				out.Commands.push_back(rectangle);

				if (background->BorderSizePixel > 0) {
					DrawCommand border = base;
					border.Kind = DrawKind::Outline;
					border.Bounds = BorderRect(
						bounds, background->Border, static_cast<float>(background->BorderSizePixel)
					);
					border.Thickness = static_cast<float>(background->BorderSizePixel);
					border.Tint = tintByGroup(background->BorderColor);

					// Roblox's border fades with the *background*'s
					// transparency and has none of its own. Kept, because a
					// script fading a panel expects the outline to go with it.
					border.Transparency = fadeByGroup(background->Transparency);
					out.Commands.push_back(border);
				}
			}

			if (const Picture *picture = store.Get<Picture>(instance);
				picture != nullptr && picture->Image.IsValid() && picture->Transparency < 1.0f) {
				// **The state image wins and neither is stored back**, which is
				// `AutoButtonColor`'s rule for the same reason: `Image` has to
				// read what the author wrote whatever the pointer is doing.
				// `Interactable` switches the swap off with the colour shift,
				// because a greyed-out button showing its hover art would be
				// telling a person it is pressable.
				core::Name shown = picture->Image;
				if (element->Interactable) {
					if (instance == request.Pressed && picture->PressedImage.IsValid()) {
						shown = picture->PressedImage;
					} else if (instance == request.Hovered && picture->HoverImage.IsValid()) {
						shown = picture->HoverImage;
					}
				}

				DrawCommand image = base;
				image.Kind = DrawKind::Image;
				image.Image = shown;
				image.Resample = picture->Resample;
				image.Tint = tintByGroup(picture->Color);
				image.Transparency = fadeByGroup(picture->Transparency);
				image.Scale = picture->Scale;
				image.SliceCenter = picture->SliceCenter;
				image.SliceScale = picture->SliceScale;
				image.Sample = Rect{
					picture->RectOffset,
					Vector2{
						picture->RectOffset.X + picture->RectSize.X,
						picture->RectOffset.Y + picture->RectSize.Y,
					}
				};
				image.Tile = picture->TileSize.Resolve(resolved.AbsoluteSize);
				image.Shader = picture->Shader;
				out.Commands.push_back(image);
			}

			if (const Viewport *viewport = store.Get<Viewport>(instance);
				viewport != nullptr && viewport->CurrentCamera != ecs::NULL_ENTITY &&
				viewport->Transparency < 1.0f) {
				DrawCommand scene = base;
				scene.Kind = DrawKind::Viewport;
				scene.Tint = tintByGroup(viewport->Color);
				scene.Transparency = fadeByGroup(viewport->Transparency);
				out.Commands.push_back(scene);
			}

			if (const Label *label = store.Get<Label>(instance); label != nullptr) {
				// A text box with nothing typed shows its placeholder, in the
				// placeholder's colour. One command either way, because a
				// backend drawing "the text" should not have to know which of
				// the two strings it is.
				const std::string *text = &label->Text;
				Color3 colour = label->Color;

				if (const Entry *entry = store.Get<Entry>(instance); entry != nullptr && text->empty()) {
					text = &entry->PlaceholderText;
					colour = entry->PlaceholderColor;
				}

				// **Parsed, then cut, then the spans are cut to match.** Roblox
				// applies `MaxVisibleGraphemes` to what a reader sees rather than
				// to what an author typed, so a typewriter effect on a marked-up
				// string reveals letters and not tags.
				std::string plain;
				std::vector<DrawSpan> spans;
				if (label->Rich && !text->empty()) {
					ParseRichText(*text, *label, plain, spans);
					text = &plain;
				}

				const std::string_view shown = FirstCharacters(*text, label->MaxVisible);
				if (shown.size() < text->size()) {
					const auto cut = static_cast<uint32_t>(shown.size());
					spans.erase(
						std::remove_if(
							spans.begin(),
							spans.end(),
							[cut](const DrawSpan &span) { return span.Begin >= cut; }
						),
						spans.end()
					);
					for (DrawSpan &span : spans) {
						span.End = std::min(span.End, cut);
					}
				}

				if (!shown.empty() && label->Transparency < 1.0f) {
					DrawCommand run = base;
					run.Kind = DrawKind::Text;
					run.Text = std::string(shown);
					run.Spans = std::move(spans);
					run.Tint = tintByGroup(colour);
					run.Transparency = fadeByGroup(label->Transparency);
					run.TextSize = resolved.TextSize;
					run.Font = label->Font;
					run.XAlignment = label->XAlignment;
					run.YAlignment = label->YAlignment;
					run.Wrapped = label->Wrapped;
					run.Truncate = label->Truncate;
					run.LineHeight = label->LineHeight;
					run.StrokeTint = tintByGroup(label->StrokeColor);
					run.StrokeTransparency = fadeByGroup(label->StrokeTransparency);

					// **A contextual `UIStroke` on a text element outlines the
					// glyphs, and that is what "contextual" means.** It wins over
					// `TextStrokeColor3` rather than adding to it, because the
					// two are one outline and drawing both would double its
					// weight - and a modifier an author put there deliberately is
					// the more specific of the two.
					// **Measured against the *drawn* text size, not the
					// authored one.** `resolved.TextSize` is what `TextScaled`
					// settled on, so a scaled stroke on a shrinking label stays
					// in proportion to the glyphs it is actually outlining.
					const float glyphStroke =
						stroke != nullptr
							? StrokeThickness(*stroke, static_cast<float>(std::max(resolved.TextSize, 0)))
							: 0.0f;

					if (stroke != nullptr && stroke->Enabled && glyphStroke > 0.0f &&
						stroke->Transparency < 1.0f && stroke->Apply == StrokeMode::Contextual) {
						run.StrokeTint = tintByGroup(stroke->Color);
						run.StrokeTransparency = fadeByGroup(stroke->Transparency);
					}

					// **Text clips to the element as well as to its
					// ancestors**, whatever `ClipsDescendants` says. Roblox does
					// the same, and the reason is that a string is the one thing
					// routinely bigger than the box it was put in - an
					// unclipped overflow reads as a corrupt layout rather than
					// as text that did not fit.
					run.Clip = resolved.Clip.Intersection(bounds);
					out.Commands.push_back(run);
				}
			}

			// Last of this element's commands, so it sits over its own fill and
			// its own text - which is what an outline is for.
			// **Not drawn as a rectangle when a text element took it**, because
			// the run above already outlined the glyphs with it - drawing both
			// would put a box round a label whose author asked for an outlined
			// word. An element with no `Label` has no content to be contextual
			// about, so `Contextual` falls back to the rectangle there, which is
			// Roblox's rule and the reason the default member is the one it is.
			const bool tookByText = stroke != nullptr && stroke->Apply == StrokeMode::Contextual &&
									store.Get<Label>(instance) != nullptr &&
									!store.Get<Label>(instance)->Text.empty();

			const float ringStroke = stroke != nullptr
										 ? StrokeThickness(*stroke, std::min(bounds.Width(), bounds.Height()))
										 : 0.0f;

			if (stroke != nullptr && stroke->Enabled && !tookByText && stroke->Transparency < 1.0f &&
				ringStroke > 0.0f) {
				DrawCommand outline = base;
				outline.Kind = DrawKind::Outline;
				outline.Bounds = BorderRect(bounds, BorderMode::Outline, ringStroke);
				outline.Thickness = ringStroke;
				outline.Join = stroke->Join;
				outline.Tint = tintByGroup(stroke->Color);
				outline.Transparency = fadeByGroup(stroke->Transparency);

				// The stroke's own ramp, and *only* its own: an outline is a
				// separate thing an author gave a separate colour, so inheriting
				// the fill's gradient would tint it with a ramp nobody asked for.
				const Gradient *own = nullptr;
				store.EachChild(strokeInstance, [&](Entity child) {
					if (own == nullptr) {
						own = store.Get<Gradient>(child);
					}
				});
				outline.Gradient = ResolveGradient(own, outline.Bounds, out);
				out.Commands.push_back(outline);
			}
		}

		// The thumbs a scrolling frame draws.
		//
		// **Read from `ScrollState` rather than worked out here**, which is the
		// change that made a bar draggable. The layout already decides where each
		// thumb is - it has to, because the canvas and the window are what it
		// places children against - and a second copy of that arithmetic here
		// would be a bar drawn in one place and grabbed in another.
		void EmitScrollbars(
			const Store &store,
			Entity instance,
			Entity collector,
			const Resolved &resolved,
			const Color3 &groupTint,
			float groupOpacity,
			DrawList &out
		) {
			const Scrolling *scrolling = store.Get<Scrolling>(instance);
			const ScrollState *state = store.Get<ScrollState>(instance);
			if (scrolling == nullptr || state == nullptr || scrolling->BarThickness <= 0 ||
				scrolling->BarTransparency >= 1.0f) {
				return;
			}

			const Rect bounds{
				resolved.AbsolutePosition,
				Vector2{
					resolved.AbsolutePosition.X + resolved.AbsoluteSize.X,
					resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y,
				},
			};

			DrawCommand bar;
			bar.Source = instance;
			bar.Collector = collector;
			bar.Spatial = store.Get<SpatialCanvas>(collector) != nullptr;
			bar.Clip = resolved.Clip.Intersection(bounds);
			bar.CornerRadius = static_cast<float>(scrolling->BarThickness) * 0.5f;
			bar.Tint = Color3{
				scrolling->BarColor.R * groupTint.R,
				scrolling->BarColor.G * groupTint.G,
				scrolling->BarColor.B * groupTint.B,
			};
			bar.Transparency =
				1.0f - (1.0f - std::clamp(scrolling->BarTransparency, 0.0f, 1.0f)) * groupOpacity;

			// **Three images or none, and `MidImage` is what decides.** Roblox
			// draws a bar as two caps and a stretched middle; a bar with only a
			// cap set would be one that drew a corner and no length, so the
			// middle is the one that turns the path on and the caps default to it
			// when unset. Unset entirely leaves the rounded rectangle, which is a
			// perfectly good scroll bar and is what the default theme is.
			const bool pictured = scrolling->MidImage.IsValid();
			const core::Name cap = scrolling->TopImage.IsValid() ? scrolling->TopImage : scrolling->MidImage;
			const core::Name tail =
				scrolling->BottomImage.IsValid() ? scrolling->BottomImage : scrolling->MidImage;

			// The caps take one thickness each and the middle stretches between
			// them, which is a nine-slice in one dimension done with three quads
			// - a `ScaleType::Slice` cannot express it, because the slice centre
			// is in image pixels and these are three separate images.
			const auto strip = [&](const Rect &whole, bool downwards) {
				if (!pictured) {
					bar.Kind = DrawKind::Rectangle;
					bar.Image = core::Name{};
					bar.Bounds = whole;
					out.Commands.push_back(bar);
					return;
				}

				const float size = static_cast<float>(scrolling->BarThickness);
				const float length = downwards ? whole.Height() : whole.Width();
				const float ends = std::min(size, length * 0.5f);

				bar.Kind = DrawKind::Image;
				bar.Scale = ScaleType::Stretch;

				const auto piece =
					[&](float from, float to, const core::Name &image) {
						if (!(to > from)) {
							return;
						}
						bar.Image = image;
						bar.Bounds = downwards
									 ? Rect{Vector2{whole.Min.X, whole.Min.Y + from},
											Vector2{whole.Max.X, whole.Min.Y + to}}
									 : Rect{Vector2{whole.Min.X + from, whole.Min.Y},
											Vector2{whole.Min.X + to, whole.Max.Y}};
						out.Commands.push_back(bar);
					};

				piece(0.0f, ends, cap);
				piece(ends, length - ends, scrolling->MidImage);
				piece(length - ends, length, tail);
			};

			if (!state->VerticalThumb.Empty()) {
				strip(state->VerticalThumb, true);
			}
			if (!state->HorizontalThumb.Empty()) {
				strip(state->HorizontalThumb, false);
			}
		}

		// Walks one subtree in paint order.
		//
		// Parent first, then children sorted by `ZIndex`. **A child is drawn
		// over its parent whatever its `ZIndex`**, which is
		// `ZIndexBehavior::Sibling` and Roblox's modern default; `Global` is
		// applied afterwards, as a stable sort of the whole collector's list.
		void Walk(
			const Store &store,
			Entity instance,
			Entity collector,
			const CompileRequest &request,
			int depth,
			const Color3 &inheritedTint,
			float inheritedOpacity,
			DrawList &out
		) {
			if (depth > 256) {
				return;
			}

			const Resolved *resolved = store.Get<Resolved>(instance);
			if (resolved == nullptr || !resolved->Rendered) {
				return;
			}

			Color3 tint = inheritedTint;
			float opacity = inheritedOpacity;
			if (const Group *group = store.Get<Group>(instance)) {
				tint = Color3{
					tint.R * group->Color.R,
					tint.G * group->Color.G,
					tint.B * group->Color.B,
				};
				opacity *= 1.0f - std::clamp(group->Transparency, 0.0f, 1.0f);
			}

			Emit(store, instance, collector, *resolved, request, tint, opacity, out);

			std::vector<Entity> children;
			store.EachChild(instance, [&](Entity child) {
				if (const Resolved *below = store.Get<Resolved>(child); below != nullptr && below->Rendered) {
					children.push_back(child);
				}
			});

			// Stable, so siblings sharing a `ZIndex` keep insertion order -
			// which is Roblox's tiebreak and the only one that does not make
			// two overlapping panels swap between frames.
			std::stable_sort(children.begin(), children.end(), [&](Entity left, Entity right) {
				const Element *a = store.Get<Element>(left);
				const Element *b = store.Get<Element>(right);
				return (a != nullptr ? a->ZIndex : 0) < (b != nullptr ? b->ZIndex : 0);
			});

			for (const Entity child : children) {
				Walk(store, child, collector, request, depth + 1, tint, opacity, out);
			}

			// Scrollbars belong over the scrolled children. Emitting them after
			// the subtree keeps that paint order without a renderer special case.
			EmitScrollbars(store, instance, collector, *resolved, tint, opacity, out);
		}
	}

	bool Compiled::Rebuild(Store &store, const CompileRequest &request) {
		ENGINE_PROFILE_CAT("gui compile", engine::core::ProfileCategory::ECS);

		Asked++;

		// --- the scan, which on almost every frame is all that happens -------
		//
		// A pure read: no writes, no allocation, one linear pass per component
		// over packed columns. What it produces is a number to compare against
		// the last one.

		// **Which world, before what is in it.** Two worlds built the same way
		// allocate the same entity ids and hold the same components, so their
		// contents hash identically - correct arithmetic and the wrong answer
		// for a list that has been pointed at the other one.
		uint64_t stamp = Fold(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&store)));

		stamp = Fold(stamp, request.Display.Width);
		stamp = Fold(stamp, request.Display.Height);
		stamp = Fold(stamp, request.Display.TopInset);
		stamp = Fold(stamp, request.Hovered);
		stamp = Fold(stamp, request.Pressed);
		stamp = Fold(stamp, request.Viewer);

		// Names, for `SortOrder::Name`. Restricted to rows that have an
		// `Element`, so renaming a part does not rebuild the UI.
		store.Each<const Element, const InstanceName>(
			[&](Entity entity, const Element &, const InstanceName &label) {
				stamp = Fold(stamp, entity);
				stamp = Fold(stamp, label.Value);
			}
		);

		stamp = FoldRows<Element>(store, stamp);
		stamp = FoldRows<Background>(store, stamp);
		stamp = FoldRows<Label>(store, stamp);
		stamp = FoldRows<Picture>(store, stamp);
		stamp = FoldRows<Button>(store, stamp);
		stamp = FoldRows<Scrolling>(store, stamp);
		stamp = FoldRows<Entry>(store, stamp);
		stamp = FoldRows<Layer>(store, stamp);
		stamp = FoldRows<Surface>(store, stamp);
		stamp = FoldRows<Billboard>(store, stamp);
		stamp = FoldRows<Group>(store, stamp);
		stamp = FoldRows<Viewport>(store, stamp);
		stamp = FoldRows<Padding>(store, stamp);
		stamp = FoldRows<ListLayout>(store, stamp);
		stamp = FoldRows<GridLayout>(store, stamp);
		stamp = FoldRows<TableLayout>(store, stamp);
		stamp = FoldRows<PageLayout>(store, stamp);
		stamp = FoldRows<PageMotion>(store, stamp);
		stamp = FoldRows<ScrollMotion>(store, stamp);

		// **The clock is folded while something is moving and only then, and
		// without this an animation runs for exactly one frame.** The signature
		// is what decides whether `Rebuild` lays out at all, and the layout is
		// what advances a slide or a spring - so folding only the *resolved*
		// numbers is circular: they cannot change until the layout runs, and
		// the layout does not run until they change.
		//
		// Folding `Seconds` unconditionally would be the other failure, and a
		// worse one: every still interface in the engine would rebuild its
		// whole draw list every frame forever.
		//
		// So the question asked is whether anything is *in flight*, which is a
		// property of the rows rather than of the clock. It stops being true
		// the moment a page lands or a spring settles, and the signature goes
		// back to standing still with it.
		bool moving = false;
		store.Each<const PageMotion>([&](Entity, const PageMotion &motion) {
			moving = moving || motion.From != motion.To;
		});
		store.Each<const ScrollMotion>([&](Entity, const ScrollMotion &motion) {
			moving = moving || motion.Held || motion.ReleasedAt >= 0.0;
		});
		if (moving) {
			// As a `float`, which is what every other real number here folds
			// as. A `double` is ambiguous against the integer overload, and
			// picking it would be folding more precision than an animation
			// running at a frame rate can express anyway.
			stamp = Fold(stamp, static_cast<float>(request.Seconds));
		}
		stamp = FoldRows<DragDetector>(store, stamp);
		stamp = FoldRows<AspectRatio>(store, stamp);
		stamp = FoldRows<SizeLimits>(store, stamp);
		stamp = FoldRows<TextSizeLimits>(store, stamp);
		stamp = FoldRows<Corner>(store, stamp);
		stamp = FoldRows<Stroke>(store, stamp);
		stamp = FoldRows<Scale>(store, stamp);
		stamp = FoldRows<FlexItem>(store, stamp);
		stamp = FoldRows<Gradient>(store, stamp);
		stamp = FoldRows<Selection>(store, stamp);
		stamp = FoldRows<GuiServiceState>(store, stamp);

		if (Fresh && stamp == Stamp) {
			return false;
		}

		// --- the compile, which is what the scan exists to skip ---------------

		Stamp = stamp;
		Fresh = true;
		Built++;

		Layout(store, request.Display, request.Seconds);

		List.Commands.clear();
		List.Gradients.clear();
		List.Elements = 0;
		List.CanvasSize = Vector2{request.Display.Width, request.Display.Height};

		const Ids &ids = Classes();

		std::vector<Entity> collectors;
		store.Each<const Layer>([&](Entity entity, const Layer &) { collectors.push_back(entity); });

		std::stable_sort(collectors.begin(), collectors.end(), [&](Entity left, Entity right) {
			const Layer *a = store.Get<Layer>(left);
			const Layer *b = store.Get<Layer>(right);
			return (a != nullptr ? a->DisplayOrder : 0) < (b != nullptr ? b->DisplayOrder : 0);
		});

		for (const Entity collector : collectors) {
			const Resolved *resolved = store.Get<Resolved>(collector);
			if (resolved == nullptr || !resolved->Rendered) {
				continue;
			}

			// **Hidden from one viewer and drawn for every other**, which is what
			// makes this a compile-time decision rather than a layout one: the
			// tree is laid out identically for everybody and only what reaches
			// the list differs. A null `Viewer` matches no billboard, because
			// `PlayerToHideFrom` unset is also null and "nobody" must not hide it
			// from everybody.
			if (const Billboard *billboard = store.Get<Billboard>(collector);
				billboard != nullptr && request.Viewer != ecs::NULL_ENTITY &&
				billboard->PlayerToHideFrom == request.Viewer) {
				continue;
			}

			const size_t first = List.Commands.size();

			std::vector<Entity> roots;
			store.EachChild(collector, [&](Entity child) {
				if (store.IsA(child, ids.Object)) {
					roots.push_back(child);
				}
			});

			std::stable_sort(roots.begin(), roots.end(), [&](Entity left, Entity right) {
				const Element *a = store.Get<Element>(left);
				const Element *b = store.Get<Element>(right);
				return (a != nullptr ? a->ZIndex : 0) < (b != nullptr ? b->ZIndex : 0);
			});

			for (const Entity root : roots) {
				Walk(store, root, collector, request, 1, Color3{1.0f, 1.0f, 1.0f}, 1.0f, List);
			}

			// **`Global` is a re-sort of what `Sibling` produced**, rather than
			// a second walk. The two behaviours differ only in whether depth
			// beats `ZIndex`, so sorting the already-flat range by `ZIndex`
			// alone - stably, so the tree order survives as the tiebreak - is
			// exactly the legacy rule and costs one sort on the collectors that
			// ask for it.
			const Layer *layer = store.Get<Layer>(collector);
			if (layer != nullptr && layer->Behavior == ZIndexBehavior::Global) {
				std::stable_sort(
					List.Commands.begin() + static_cast<std::ptrdiff_t>(first),
					List.Commands.end(),
					[&](const DrawCommand &left, const DrawCommand &right) {
						const Element *a = store.Get<Element>(left.Source);
						const Element *b = store.Get<Element>(right.Source);
						return (a != nullptr ? a->ZIndex : 0) < (b != nullptr ? b->ZIndex : 0);
					}
				);
			}
		}

		// **The selection highlight, last, so it sits over everything.** A
		// gamepad's selection is a viewer's fact rather than the tree's, which is
		// why it is drawn here instead of being an instance somewhere: cloning
		// the `SelectionImageObject` into the tree - Roblox's implementation -
		// would put a row in the explorer, in `GetChildren` and in a save file
		// that nobody authored. What is emitted instead is that object's own fill
		// and picture at the selected element's rectangle, which is what a
		// highlight is and needs no second tree.
		//
		// Nothing is drawn when no `SelectionImageObject` is set, which is
		// deliberate and is the one place this stops short of Roblox: the default
		// highlight there is a built-in asset this engine does not ship, and a
		// rectangle invented here would be a look nobody chose.
		if (const Entity service = GuiServiceOf(store); service != ecs::NULL_ENTITY) {
			const GuiServiceState *selection = store.Get<GuiServiceState>(service);
			const Entity selected = selection != nullptr ? selection->SelectedObject : ecs::NULL_ENTITY;
			const Selection *marks = selected != ecs::NULL_ENTITY && store.Alive(selected)
										 ? store.Get<Selection>(selected)
										 : nullptr;
			const Resolved *where = marks != nullptr ? store.Get<Resolved>(selected) : nullptr;

			if (marks != nullptr && where != nullptr && where->Rendered &&
				marks->ImageObject != ecs::NULL_ENTITY && store.Alive(marks->ImageObject)) {
				const Rect over{
					where->AbsolutePosition,
					Vector2{
						where->AbsolutePosition.X + where->AbsoluteSize.X,
						where->AbsolutePosition.Y + where->AbsoluteSize.Y,
					},
				};

				DrawCommand mark;
				mark.Source = marks->ImageObject;
				mark.Bounds = over;
				mark.Clip = where->Clip;
				mark.Rotation = where->AbsoluteRotation;

				// **The selected element's own collector, read back off the list
				// rather than looked up in the tree.** A backend splits screen
				// pixels from a world-space canvas by this field, so a highlight
				// carrying the wrong one would be drawn on the screen while the
				// button it marks is on a wall.
				for (const DrawCommand &command : List.Commands) {
					if (command.Source == selected) {
						mark.Collector = command.Collector;
						mark.Spatial = command.Spatial;
						break;
					}
				}

				if (const Background *fill = store.Get<Background>(marks->ImageObject);
					fill != nullptr && fill->Transparency < 1.0f) {
					mark.Kind = DrawKind::Rectangle;
					mark.Tint = fill->Color;
					mark.Transparency = fill->Transparency;
					List.Commands.push_back(mark);
				}

				if (const Picture *picture = store.Get<Picture>(marks->ImageObject);
					picture != nullptr && picture->Image.IsValid() && picture->Transparency < 1.0f) {
					mark.Kind = DrawKind::Image;
					mark.Image = picture->Image;
					mark.Tint = picture->Color;
					mark.Transparency = picture->Transparency;
					mark.Scale = picture->Scale;
					mark.SliceCenter = picture->SliceCenter;
					mark.SliceScale = picture->SliceScale;
					mark.Sample = Rect{
						picture->RectOffset,
						Vector2{
							picture->RectOffset.X + picture->RectSize.X,
							picture->RectOffset.Y + picture->RectSize.Y,
						},
					};
					mark.Tile = picture->TileSize.Resolve(where->AbsoluteSize);
					mark.Shader = picture->Shader;
					List.Commands.push_back(mark);
				}
			}
		}

		// The paint position, written back so a panel or a test can ask why one
		// element covered another. Read by nothing on the drawing path.
		for (size_t index = 0; index < List.Commands.size(); index++) {
			if (Resolved *value = store.GetMutable<Resolved>(List.Commands[index].Source)) {
				value->Order = static_cast<int32_t>(index);
			}
		}

		return true;
	}

	size_t DemandedShaders(ecs::Store &store, std::vector<core::Name> &out) {
		out.clear();

		store.Each<const Picture>([&out](ecs::Entity, const Picture &picture) {
			if (picture.Shader.IsValid()) {
				out.push_back(picture.Shader);
			}
		});

		std::sort(out.begin(), out.end(), [](const core::Name &left, const core::Name &right) {
			return left.Id() < right.Id();
		});
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out.size();
	}
}
