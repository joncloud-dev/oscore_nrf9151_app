# Convenience: build this out-of-tree app against an EXISTING NCS/west workspace
# without running `west init`/`west update` on this repo's own west.yml. It points
# Zephyr at the workspace and registers the standalone oscore_cloud library
# (a sibling repo) as an extra Zephyr module.
#
# Usage (in a fresh toolchain shell):
#
#   nrfutil toolchain-manager launch --shell
#   source env.sh
#   west build -b nrf9151dk/nrf9151/ns
#
# Override the workspace location if yours differs:
#   NCS_WORKSPACE=/path/to/ncs source env.sh
#
# Override the oscore_cloud library location if it is not a sibling of this repo:
#   OSCORE_CLOUD_ROOT=/path/to/oscore_cloud source env.sh
#
# IMPORTANT: do NOT also source att/env.sh in the same shell. That one registers
# the whole Asset-Tracker-Template repo as a module, which pulls in a SECOND copy
# of the oscore_cloud module (att/modules/oscore_cloud) and causes duplicate
# symbols. This app needs only the standalone library below.
#
# The workspace's uoscore-uedhoc must be the fork with opaque PSA keys
# (branch oscore-opaque-psa-keys); the OSCORE wrapper will not compile otherwise.

: "${NCS_WORKSPACE:=/Users/jhn/work/ncs}"
export ZEPHYR_BASE="$NCS_WORKSPACE/zephyr"

# Resolve this repo's root (the directory containing this script).
if [ -n "${BASH_SOURCE:-}" ]; then
	_app_src="${BASH_SOURCE[0]}"
elif [ -n "${ZSH_VERSION:-}" ]; then
	_app_src="${(%):-%x}"
else
	_app_src="$0"
fi
APP_ROOT="$(cd "$(dirname "$_app_src")" && pwd)"
unset _app_src

# The oscore_cloud library defaults to a sibling directory of this repo.
: "${OSCORE_CLOUD_ROOT:=$(cd "$APP_ROOT/../oscore_cloud" 2>/dev/null && pwd)}"

if [ -z "$OSCORE_CLOUD_ROOT" ] || [ ! -f "$OSCORE_CLOUD_ROOT/zephyr/module.yml" ]; then
	echo "ERROR: oscore_cloud library not found. Set OSCORE_CLOUD_ROOT to its path." >&2
	return 1 2>/dev/null || exit 1
fi

export EXTRA_ZEPHYR_MODULES="${EXTRA_ZEPHYR_MODULES:+$EXTRA_ZEPHYR_MODULES;}$OSCORE_CLOUD_ROOT"

echo "ZEPHYR_BASE=$ZEPHYR_BASE"
echo "EXTRA_ZEPHYR_MODULES=$EXTRA_ZEPHYR_MODULES"
