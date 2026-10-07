# Source full-text monospace sizing

The pinned Pixel Composer source is commit
`b69eca232217360cf1502ef0223523d818606652`, inspected from the supplied archive.
`scripts/node_text/node_text.gml` assigns `rawStr` after case conversion and
before trimming (line 474). Its full-text branch (lines 594-596) selects
`__monoW * string_length(rawStr)` for unwrapped monospaced width. Height still
comes from `string_height(rawStr)`. The wrapped branch uses
`string_width_ext(rawStr, -1, _lineW)` independently of the monospace control.

`FontTextLayout.cpp` therefore applies the unwrapped monospace width after
choosing observed or native size records. An observed proportional width does
not supersede this source expression. Newlines count toward that expression,
and trimming does not change its count.

`FontTextLayoutSizing.cpp` uses owned literal font metrics to cover these
branches, including expansion from observed casing. These are source-derived
CPU checks, not licensed runtime captures or claims of font raster parity.
