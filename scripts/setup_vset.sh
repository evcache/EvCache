#!/usr/bin/env bash
set -euo pipefail

CGROUP_ROOT="/sys/fs/cgroup"
HI_GROUP="$CGROUP_ROOT/hi_prgroup"
LW_GROUP="$CGROUP_ROOT/lw_prgroup"

if [[ ${EUID} -ne 0 ]]; then
    echo "error: run this script as root, e.g. sudo ./scripts/setup_vset.sh" >&2
    exit 1
fi

if [[ ! -f "$CGROUP_ROOT/cgroup.controllers" ]]; then
    echo "error: cgroup v2 is not mounted at $CGROUP_ROOT" >&2
    exit 1
fi

for controller in cpu cpuset; do
    if ! grep -qw "$controller" "$CGROUP_ROOT/cgroup.controllers"; then
        echo "error: cgroup controller '$controller' is not available" >&2
        exit 1
    fi
done

echo "+cpu" > "$CGROUP_ROOT/cgroup.subtree_control"
echo "+cpuset" > "$CGROUP_ROOT/cgroup.subtree_control"

mkdir -p "$HI_GROUP" "$LW_GROUP"

echo "threaded" > "$LW_GROUP/cgroup.type"
echo "threaded" > "$HI_GROUP/cgroup.type"
echo 1 > "$LW_GROUP/cpu.idle"
echo -20 > "$HI_GROUP/cpu.weight.nice"

echo "vset cgroups are ready"
