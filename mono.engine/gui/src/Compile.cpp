#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
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
		// crosses a process — the comparison is this frame against the last —
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
		// order-independent fold would be the more accurate answer — and would
		// also collide far more readily, because commutative folds do. A
		// reshuffle costs one rebuild nobody sees; a collision is a UI showing
		// what is no longer there.
		constexpr uint64_t Fold(uint64_t running, uint64_t term) {
			return (running ^ Scramble(term)) * GOLDEN;
		}

		// **Floats fold by their bits, not by their value.** `std::bit_cast`
		// rather than a cast to an integer, which would round — so a position
		// moving by half a pixel would hash the same and the panel would keep
		// the old rectangle. It also means `-0.0` and `0.0` fold differently
		// and cost one rebuild, which is the safe direction.
		uint64_t Fold(uint64_t running, float value) {
			return Fold(running, static_cast<uint64_t>(std::bit_cast<uint32_t>(value)));
		}

		uint64_t Fold(uint64_t running, bool value) {
			// **1 and 2, not 1 and 0.** A zero term folds to a value that
			// depends only on the running total, so a `false` next to a
			// missing field would be indistinguishable — and the whole job here
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
		// — which is what makes this a check rather than a comment.
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
			running = Fold(running, value.Constraint);
			return Fold(running, value.Automatic);
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
			return Fold(running, value.LineHeight);
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
			return Fold(running, value.RectSize);
		}

		uint64_t Fold(uint64_t running, const Button &value) {
			running = Fold(running, value.AutoButtonColor);
			running = Fold(running, value.Modal);
			return Fold(running, value.Selected);
		}

		uint64_t Fold(uint64_t running, const Scrolling &value) {
			running = Fold(running, value.CanvasSize);
			running = Fold(running, value.CanvasPosition);
			running = Fold(running, value.BarThickness);
			running = Fold(running, value.BarColor);
			running = Fold(running, value.BarTransparency);
			running = Fold(running, value.Enabled);
			running = Fold(running, value.Direction);
			return Fold(running, value.AutomaticCanvas);
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
			return Fold(running, value.Brightness);
		}

		uint64_t Fold(uint64_t running, const Billboard &value) {
			running = Fold(running, value.Adornee);
			running = Fold(running, value.Size);
			running = Fold(running, value.StudsOffset);
			running = Fold(running, value.StudsOffsetWorldSpace);
			running = Fold(running, value.ExtentsOffset);
			running = Fold(running, value.AlwaysOnTop);
			running = Fold(running, value.LightInfluence);
			return Fold(running, value.MaxDistance);
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
			return Fold(running, value.Order);
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
			return Fold(running, value.Transparency);
		}

		uint64_t Fold(uint64_t running, const Scale &value) {
			return Fold(running, value.Factor);
		}

		// One pass over every row carrying `T`, folding the row's identity, its
		// place in the tree and the component itself.
		//
		// **The tree links come along with every component**, so a node with an
		// `Element` and a `Background` folds them twice. That is one extra
		// multiply per row against having to remember a separate hierarchy pass
		// restricted to exactly the right set of rows — and a pass that covered
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
		// perceived brightness — a saturated green reads as light and a
		// saturated blue as dark at the same numeric value.
		float ShiftDirection(const Color3 &colour) {
			const float luminance = 0.2126f * colour.R + 0.7152f * colour.G + 0.0722f * colour.B;
			return luminance > 0.5f ? -1.0f : 1.0f;
		}

		// The border rectangle for a `BorderMode`.
		//
		// Three answers rather than one because the choice decides whether two
		// elements sized to touch overlap by a pixel — which is the difference
		// between a table with hairlines and a table with double rules.
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

		// Everything one element contributes to the list.
		void Emit(
			const Store &store,
			Entity instance,
			const Resolved &resolved,
			const CompileRequest &request,
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

			out.Elements++;

			float radius = 0.0f;
			const Stroke *stroke = nullptr;
			store.EachChild(instance, [&](Entity child) {
				if (const Corner *corner = store.Get<Corner>(child)) {
					// Against the *smaller* axis, which is what stops a scale
					// of 0.5 turning a wide element into an ellipse.
					radius =
						corner->Radius.Resolve(std::min(resolved.AbsoluteSize.X, resolved.AbsoluteSize.Y));
				}
				if (stroke == nullptr) {
					stroke = store.Get<Stroke>(child);
				}
			});

			DrawCommand base;
			base.Source = instance;
			base.Bounds = bounds;
			base.Clip = resolved.Clip;
			base.Rotation = resolved.AbsoluteRotation;
			base.CornerRadius = radius;

			if (const Background *background = store.Get<Background>(instance);
				background != nullptr && background->Transparency < 1.0f) {
				Color3 fill = background->Color;

				// **Applied here and never stored back**, so `BackgroundColor3`
				// reads what the author wrote however the pointer is behaving.
				// `Button::AutoButtonColor` says so at the field.
				if (const Button *button = store.Get<Button>(instance);
					button != nullptr && button->AutoButtonColor) {
					const float direction = ShiftDirection(fill);
					if (instance == request.Pressed) {
						fill = Shift(fill, direction * PRESS_SHIFT);
					} else if (instance == request.Hovered) {
						fill = Shift(fill, direction * HOVER_SHIFT);
					}
				}

				DrawCommand rectangle = base;
				rectangle.Kind = DrawKind::Rectangle;
				rectangle.Tint = fill;
				rectangle.Transparency = background->Transparency;
				out.Commands.push_back(rectangle);

				if (background->BorderSizePixel > 0) {
					DrawCommand border = base;
					border.Kind = DrawKind::Outline;
					border.Bounds = BorderRect(
						bounds, background->Border, static_cast<float>(background->BorderSizePixel)
					);
					border.Thickness = static_cast<float>(background->BorderSizePixel);
					border.Tint = background->BorderColor;

					// Roblox's border fades with the *background*'s
					// transparency and has none of its own. Kept, because a
					// script fading a panel expects the outline to go with it.
					border.Transparency = background->Transparency;
					out.Commands.push_back(border);
				}
			}

			if (const Picture *picture = store.Get<Picture>(instance);
				picture != nullptr && picture->Image.IsValid() && picture->Transparency < 1.0f) {
				DrawCommand image = base;
				image.Kind = DrawKind::Image;
				image.Image = picture->Image;
				image.Tint = picture->Color;
				image.Transparency = picture->Transparency;
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
				out.Commands.push_back(image);
			}

			if (const Label *label = store.Get<Label>(instance); label != nullptr) {
				// A text box with nothing typed shows its placeholder, in the
				// placeholder's colour. One command either way, because a
				// backend drawing "the text" should not have to know which of
				// the two strings it is.
				const std::string *text = &label->Text;
				Color3 colour = label->Color;

				if (const Entry *entry = store.Get<Entry>(instance);
					entry != nullptr && text->empty()) {
					text = &entry->PlaceholderText;
					colour = entry->PlaceholderColor;
				}

				if (!text->empty() && label->Transparency < 1.0f) {
					DrawCommand run = base;
					run.Kind = DrawKind::Text;
					run.Text = *text;
					run.Tint = colour;
					run.Transparency = label->Transparency;
					run.TextSize = resolved.TextSize;
					run.Font = label->Font;
					run.XAlignment = label->XAlignment;
					run.YAlignment = label->YAlignment;
					run.Wrapped = label->Wrapped;
					run.LineHeight = label->LineHeight;

					// **Text clips to the element as well as to its
					// ancestors**, whatever `ClipsDescendants` says. Roblox does
					// the same, and the reason is that a string is the one thing
					// routinely bigger than the box it was put in — an
					// unclipped overflow reads as a corrupt layout rather than
					// as text that did not fit.
					run.Clip = resolved.Clip.Intersection(bounds);
					out.Commands.push_back(run);
				}
			}

			// Last of this element's commands, so it sits over its own fill and
			// its own text — which is what an outline is for.
			if (stroke != nullptr && stroke->Transparency < 1.0f && stroke->Thickness > 0.0f) {
				DrawCommand outline = base;
				outline.Kind = DrawKind::Outline;
				outline.Bounds = BorderRect(bounds, BorderMode::Outline, stroke->Thickness);
				outline.Thickness = stroke->Thickness;
				outline.Tint = stroke->Color;
				outline.Transparency = stroke->Transparency;
				out.Commands.push_back(outline);
			}
		}

		// Walks one subtree in paint order.
		//
		// Parent first, then children sorted by `ZIndex`. **A child is drawn
		// over its parent whatever its `ZIndex`**, which is
		// `ZIndexBehavior::Sibling` and Roblox's modern default; `Global` is
		// applied afterwards, as a stable sort of the whole collector's list.
		void
		Walk(const Store &store, Entity instance, const CompileRequest &request, int depth, DrawList &out) {
			if (depth > 256) {
				return;
			}

			const Resolved *resolved = store.Get<Resolved>(instance);
			if (resolved == nullptr || !resolved->Rendered) {
				return;
			}

			Emit(store, instance, *resolved, request, out);

			std::vector<Entity> children;
			store.EachChild(instance, [&](Entity child) {
				if (const Resolved *below = store.Get<Resolved>(child); below != nullptr && below->Rendered) {
					children.push_back(child);
				}
			});

			// Stable, so siblings sharing a `ZIndex` keep insertion order —
			// which is Roblox's tiebreak and the only one that does not make
			// two overlapping panels swap between frames.
			std::stable_sort(children.begin(), children.end(), [&](Entity left, Entity right) {
				const Element *a = store.Get<Element>(left);
				const Element *b = store.Get<Element>(right);
				return (a != nullptr ? a->ZIndex : 0) < (b != nullptr ? b->ZIndex : 0);
			});

			for (const Entity child : children) {
				Walk(store, child, request, depth + 1, out);
			}
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
		// contents hash identically — correct arithmetic and the wrong answer
		// for a list that has been pointed at the other one.
		uint64_t stamp = Fold(0, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&store)));

		stamp = Fold(stamp, request.Display.Width);
		stamp = Fold(stamp, request.Display.Height);
		stamp = Fold(stamp, request.Display.TopInset);
		stamp = Fold(stamp, request.Hovered);
		stamp = Fold(stamp, request.Pressed);

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
		stamp = FoldRows<AspectRatio>(store, stamp);
		stamp = FoldRows<SizeLimits>(store, stamp);
		stamp = FoldRows<TextSizeLimits>(store, stamp);
		stamp = FoldRows<Corner>(store, stamp);
		stamp = FoldRows<Stroke>(store, stamp);
		stamp = FoldRows<Scale>(store, stamp);

		if (Fresh && stamp == Stamp) {
			return false;
		}

		// --- the compile, which is what the scan exists to skip ---------------

		Stamp = stamp;
		Fresh = true;
		Built++;

		Layout(store, request.Display);

		List.Commands.clear();
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
				Walk(store, root, request, 1, List);
			}

			// **`Global` is a re-sort of what `Sibling` produced**, rather than
			// a second walk. The two behaviours differ only in whether depth
			// beats `ZIndex`, so sorting the already-flat range by `ZIndex`
			// alone — stably, so the tree order survives as the tiebreak — is
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

		// The paint position, written back so a panel or a test can ask why one
		// element covered another. Read by nothing on the drawing path.
		for (size_t index = 0; index < List.Commands.size(); index++) {
			if (Resolved *value = store.GetMutable<Resolved>(List.Commands[index].Source)) {
				value->Order = static_cast<int32_t>(index);
			}
		}

		return true;
	}
}
