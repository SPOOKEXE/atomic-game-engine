# Native byte import profiles

These codecs accept bytes and explicit dependencies. They do not open files, follow paths or invoke processes. They publish complete results and preserve prior outputs on failure.

| Codec | Native coverage | Explicit limits |
| --- | --- | --- |
| OpenRaster | Merged image, named layer pixels, offsets and XML metadata | PNG members; bounded ZIP count, sizes and CRCs |
| Krita | Merged image and BGRA8 paint layers with raw/LZF tiles | Syntax 2.0; paint and group layers; unsupported layer types fail |
| Composer OBJ | Named material runs, triangles/quads, source centering, flat normals, winding and axis conversion | Positive indices; bounded vectors, faces and output vertices |
| Element JSON | Named face materials, UV rectangles and nested local transforms | Bounded tree, elements and output vertices |
| Composer XML | Root/prolog tree, ordered attributes and raw text | Attribute entities use source replacement order; external entities are refused |
| GameMaker room | Supplied sprite/object dependencies, layer composition and named tile overrides | First sprite frame/layer thumbnails; bounded JSON, pixels and drawing work |

The dev bake suite passed 146 cases and 15,709 assertions on 2026-10-02. Those checks cover byte codecs, including explicit failure cases. They do not establish licensed Pixel Composer executable parity or complete Studio host integration.
