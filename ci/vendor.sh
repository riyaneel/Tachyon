#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: $O <target>"
	echo "Targets: go | java | node | rust"
	exit 1
fi

TARGET="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

case "${TARGET}" in
"go")
	DEST="${ROOT_DIR}/bindings/go/tachyon/_core_local"
	;;
"java")
	DEST="${ROOT_DIR}/bindings/java/src/native/_core_local"
	;;
"node")
	DEST="${ROOT_DIR}/bindings/node/src/native/_core_local"
	;;
"rust")
	DEST="${ROOT_DIR}/bindings/rust/tachyon-sys/vendor/core"
	;;
*)
	echo "Error: Unknown target '${TARGET}'"
	exit 1
	;;
esac

rm -rf "${DEST}"
mkdir -p "${DEST}"
cp -r "${ROOT_DIR}/core/include" "${DEST}/"
cp -r "${ROOT_DIR}/core/src" "${DEST}/"

echo "Vendored core for ${TARGET} → ${DEST}"
echo "Files:"
find "${DEST}" -type f | sort | sed 's|^|  |'
