#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source "$root/scripts/integrated-stress-common.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
log="$tmp/driver.log"
worlds=(alpha beta)

cat > "$log" <<'EOF'
[12:00:00.000] [info] host-ready host=host.shared.0 world=alpha port=41001
[12:00:00.001] [info] host-ready host=host.shared.1 world=beta port=41002
EOF
parse_ready_endpoints "$log" "${worlds[@]}"
[[ ${#READY_ENDPOINTS[@]} -eq 2 ]]
[[ ${READY_ENDPOINTS[0]} == "host.shared.0 alpha 41001" ]]

cat > "$log" <<'EOF'
host-ready host=host.shared.0 world=alpha port=41001
EOF
if parse_ready_endpoints "$log" "${worlds[@]}"; then
	echo "FAIL: parser accepted a missing world" >&2
	exit 1
else
	status=$?
	[[ $status -eq 1 ]]
fi

cat > "$log" <<'EOF'
host-ready host=host.shared.0 world=alpha port=41001
host-ready host=host.shared.0 world=beta port=41002
EOF
if parse_ready_endpoints "$log" "${worlds[@]}" 2>/dev/null; then
	echo "FAIL: parser accepted a duplicate host" >&2
	exit 1
else
	status=$?
	[[ $status -eq 2 ]]
fi

cat > "$log" <<'EOF'
host-ready host=host.shared.0 world=alpha port=41001
host-ready host=host.shared.1 world=beta port=41001
EOF
if parse_ready_endpoints "$log" "${worlds[@]}" 2>/dev/null; then
	echo "FAIL: parser accepted a duplicate endpoint port" >&2
	exit 1
else
	status=$?
	[[ $status -eq 2 ]]
fi

echo "integrated stress endpoint parser tests passed"
