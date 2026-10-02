"""Select evidence-backed nodes whose pinned source files match the archive."""

import hashlib
from pathlib import Path


def verified_source_nodes(evidence: dict, source_commit: str, root: Path,
                          source_files: dict[str, str]) -> dict[str, dict]:
    """Return verified node metadata, rejecting any source/archive mismatch."""
    if evidence.get("source_commit") != source_commit:
        raise ValueError("evidence source commit does not match pinned commit")

    selected = {}
    for node in evidence.get("nodes", []):
        node_id = node.get("source_node_id")
        if not node.get("source_node_id_verified") or node_id is None:
            continue
        if node_id not in source_files:
            raise ValueError(f"unknown verified source node id: {node_id}")

        source = node.get("source", {})
        archive_member = source.get("archive_member", "")
        if "/" not in archive_member or archive_member.split("/", 1)[1] != source_files[node_id]:
            raise ValueError(f"source path mismatch for {node_id}")
        source_path = root / archive_member.split("/", 1)[1]
        content = source_path.read_bytes()
        digest = hashlib.sha256(content).hexdigest()
        if digest != source.get("sha256"):
            raise ValueError(f"source hash mismatch for {node_id}")
        if len(content) != source.get("bytes"):
            raise ValueError(f"source size mismatch for {node_id}")

        documentation = node.get("published_documentation", {})
        status = documentation.get("status")
        metadata = {
            "source_origin": "official pinned source",
            "source_evidence": {
                "source_commit": source_commit,
                "source_url": source["immutable_url"],
                "sha256": source["sha256"],
            },
            "documentation_status": status,
            "undocumented": status != 200,
            "source_only": status != 200,
        }
        if node_id in selected and selected[node_id] != metadata:
            raise ValueError(f"conflicting verified source evidence for {node_id}")
        selected[node_id] = metadata
    return selected
