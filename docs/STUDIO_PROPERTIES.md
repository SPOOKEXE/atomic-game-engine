# Studio properties

The Properties panel groups reflected properties into collapsible sections.
Mesh parts expose separate LOD, PBR, Geometry, Transform, Appearance and Physics
sections. Other built-in classes use sections suited to their properties.
Filtering matches property names and hides sections with no matching rows.

## Property metadata

`ecs::PropertyDescriptor::PropertiesTag` names the inspector section. An invalid
tag places a property under `Unassigned`. The tag changes presentation only;
it does not move component storage or change property names, saves or replication.

Assign a tag after declaring a property:

```cpp
ecs::Classes::SetPropertiesTag(
    scene::EditableMeshClass(), "TriangleCount", core::Name("Geometry")
);
```

`SetPropertiesTag` accepts the class that declares the property and returns
false for an unknown property or an inherited-only declaration. Descendants
inherit the tag. A redeclaration inherits the base tag until it receives its
own valid tag; clearing that local tag restores the inherited tag.

Section names are ordinary names, so extensions can supply their own groups.
Property editors retain the declaring class and property identity when grouped.
Mixed selections continue to expose their shared editable property surface.

## Collection tags

Expand `Collection Tags` in Properties. Press `+` to open a new text row.
Enter a name and press Enter or leave the row to apply it. A blank or
whitespace-only name cancels the row; Escape cancels an edit.

Click an existing name to rename its selected memberships. Use `x` to remove
them. Mixed membership is marked, and its checkbox can add the tag to the
remaining selected instances. Authored tag edits support undo and redo;
renaming to an existing tag preserves each selected instance's prior membership
when undone. A full tag table refuses a rename before removing its original tag.
