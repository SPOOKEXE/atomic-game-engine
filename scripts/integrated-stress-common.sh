#!/usr/bin/env bash

# Parse the driver's stable host-ready record and require the planned endpoints.
# Return 0 when complete, 1 while endpoints are still missing, and 2 on bad or
# duplicate records.
parse_ready_endpoints() {
	local log_path=$1
	shift
	local -a expected_worlds=("$@")
	local line host world port expected_world
	local -A expected=() seen_hosts=() seen_worlds=() seen_ports=()
	READY_ENDPOINTS=()

	for world in "${expected_worlds[@]}"; do
		expected["$world"]=1
	done

	while IFS= read -r line; do
		if [[ ! $line =~ host-ready[[:space:]]host=([^[:space:]]+)[[:space:]]world=([^[:space:]]+)[[:space:]]port=([0-9]+) ]]; then
			continue
		fi
		host=${BASH_REMATCH[1]}
		world=${BASH_REMATCH[2]}
		port=${BASH_REMATCH[3]}
		if [[ ! ${expected["$world"]+present} ]]; then
			printf 'unexpected hosted world in ready log: %s\n' "$world" >&2
			return 2
		fi
		if [[ ${seen_hosts["$host"]+present} || ${seen_worlds["$world"]+present} || ${seen_ports["$port"]+present} ]]; then
			printf 'duplicate host, world or port in ready log: host=%s world=%s port=%s\n' "$host" "$world" "$port" >&2
			return 2
		fi
		if ((10#$port < 1 || 10#$port > 65535)); then
			printf 'invalid hosted port in ready log: %s\n' "$port" >&2
			return 2
		fi
		seen_hosts["$host"]=1
		seen_worlds["$world"]=1
		seen_ports["$port"]=1
		READY_ENDPOINTS+=("$host $world $port")
	done < "$log_path"

	for expected_world in "${expected_worlds[@]}"; do
		if [[ ! ${seen_worlds["$expected_world"]+present} ]]; then
			return 1
		fi
	done
	return 0
}
