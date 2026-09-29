# Pixel Composer documentation archive recovery

Retrieved on 2026-09-30. `source-manifest-2026-09-30.json` pins the exact response bytes and an archive in the adjacent supplied-fixture repository. Its `archive` path is relative to this directory:

`../../../atomic-game-engine-hidden-docs/files/pixel-composer-com/official-docs-2026-09-30.tar.gz`

Archive size: 49,222,213 bytes. SHA-256:

`9091bc95e7599211dc9ede5028999875b54382f66a4297d6f796c0569bcc3013`

The historical manifest is preserved byte for byte in `source-manifest-2026-09-23.json`. Its file SHA-256 is `62db7a83fdfa6e009ffdd4ae2be7bdb960d599dd271ba047f3a64436728cfa2e`. A broader search under `/home/declan`, including hidden and ignored files, did not recover the original archive. One Trash/docker subtree was inaccessible.

The fresh archive reconstructs the page bytes described by every historical record: all 1,343 historical response hashes match. It has new retrieval provenance and a different tar/gzip container hash. It is not the recovered original archive.

## Inventory and verification

Every historical URL was requested. Successful pages were recursively searched for same-host HTML links, normalizing backslashes in hrefs to slashes. The manifest records each requested URL, final URL, response status, exact size and SHA-256, last-modified header, retrieval timestamp, and tar member path.

| Evidence | Result |
| --- | --- |
| Requested URLs | 3,014 |
| HTTP 200 responses | 2,244 |
| HTTP 404 responses | 770 |
| New successful URLs | 907, all `/nodes/_index/` meta-refresh aliases |
| New node content pages | 0 |
| Historical records retained | 1,343 of 1,343 |
| Historical response hashes matched | 1,343 of 1,343 |
| Node matrix URLs retained | 990 of 990 |
| Node matrix source hashes matched | 990 of 990 |

Override Channel initially returned HTTP 503. A retry returned HTTP 200 with the historical SHA-256. Both responses are archived, making 3,015 response members.

Verification read every response member and checked its byte count and SHA-256. It checked the embedded retrieval manifest against the external page records, the archive SHA-256, the preserved historical manifest hash, and byte agreement between the active and dated fresh manifests. All checks passed. The supplied native source pin and node/product matrices were not changed.

## Remaining source gaps

Five existing node matrix rows have empty node ids and archived HTTP 404 pages: Armature, Armature Bone, Armature IK, Armature Mirror, and Armature Subdivide under `nodes/compose/armature/build/`. Their index page is also HTTP 404. These are unresolved documentation rows, preserved exactly rather than removed.

The official current comparison candidate is [Pixel Composer 1.22.0.0 stable](https://makham.itch.io/pixel-composer/devlog/1680206/122-stable). The [official devlog index](https://makham.itch.io/pixel-composer/devlog) dates its announcement September 28, 2026, and the release page lists the Linux `Pixel_Composer_1.22.0.0-x86_64 Itch.zip` upload at 00:43 UTC that day. The September 23 inventory instead named 1.21.10.0 beta.

The 1.22 release notes name Array Cumulative, Path Redistribute, and Channel Swizzle. These nodes are absent from this documentation navigation and the existing node matrix. `mono.engine/imagegraph/src/SourceCatalogue.inc:8264` already lists Path Redistribute as `undocumented`. Array Cumulative and Channel Swizzle have no title entry in that pinned catalogue; their internal ids are unverified. Thus the archive restores reproducible documentation evidence but does not establish a release-complete 1.22 catalogue.

A licensed reference executable remains unavailable as confirmed by the user. No executable hash, runtime comparison, or exact executable parity is claimed. M0's wider reference and catalogue reconciliation gates remain open.
