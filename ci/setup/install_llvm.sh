#!/usr/bin/env bash

set -euo pipefail

LLVM_VERSION="${1:-21}"

if [[ ! -f /etc/os-release ]]; then
	echo "[llvm] Error: /etc/os-release not found: unable to detect OS" >&2
	exit 1
fi

. /etc/os-release
OS_ID="${ID}"

if [[ "${OS_ID}" == "ubuntu" || "${OS_ID}" == "debian" ]]; then
	echo "[llvm] Ubuntu/Debian detected: installing LLVM ${LLVM_VERSION} via apt.llvm.org"

	if ! command -v lsb_release &>/dev/null; then
		echo "[llvm] lsb-release missing. Installing prerequisites..."
		sudo apt-get update -y
		sudo apt-get install -y lsb-release wget software-properties-common gnupg
	fi

	SCRIPT="$(mktemp --suffix=.sh)"
	trap 'rm -f "${SCRIPT}"' EXIT

	wget -q --show-progress -O "${SCRIPT}" https://apt.llvm.org/llvm.sh
	chmod +x "${SCRIPT}"
	sudo bash "${SCRIPT}" "${LLVM_VERSION}" all

	for bin in \
		clang-${LLVM_VERSION} \
		clang++-${LLVM_VERSION} \
		lld-${LLVM_VERSION} \
		llvm-ar-${LLVM_VERSION} \
		llvm-nm-${LLVM_VERSION} \
		llvm-ranlib-${LLVM_VERSION}; do
		if ! command -v "${bin}" &>/dev/null; then
			echo "[llvm] Error: ${bin} not found after installation" >&2
			exit 1
		fi
	done

	for tool in clang clang++ clang-format clang-tidy llvm-ar llvm-nm llvm-ranlib llvm-profdata; do
		sudo update-alternatives --remove-all "${tool}" || true
	done

	sudo update-alternatives --install /usr/bin/clang clang \
		/usr/bin/clang-"${LLVM_VERSION}" 100 \
		--slave /usr/bin/clang++ clang++ /usr/bin/clang++-"${LLVM_VERSION}" \
		--slave /usr/bin/clang-format clang-format /usr/bin/clang-format-"${LLVM_VERSION}" \
		--slave /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-"${LLVM_VERSION}" \
		--slave /usr/bin/llvm-ar llvm-ar /usr/bin/llvm-ar-"${LLVM_VERSION}" \
		--slave /usr/bin/llvm-nm llvm-nm /usr/bin/llvm-nm-"${LLVM_VERSION}" \
		--slave /usr/bin/llvm-ranlib llvm-ranlib /usr/bin/llvm-ranlib-"${LLVM_VERSION}" \
		--slave /usr/bin/llvm-profdata llvm-profdata /usr/bin/llvm-profdata-"${LLVM_VERSION}"

	sudo update-alternatives --remove-all lld || true
	sudo update-alternatives --install /usr/bin/lld lld \
		/usr/bin/lld-"${LLVM_VERSION}" 100 \
		--slave /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-"${LLVM_VERSION}" \
		--slave /usr/bin/wasm-ld wasm-ld /usr/bin/wasm-ld-"${LLVM_VERSION}"

	echo "[llvm] LLVM ${LLVM_VERSION} installed (APT)"
elif [[ "${OS_ID}" == "fedora" ]]; then
	echo "[llvm] Fedora detected: installing LLVM ${LLVM_VERSION} via DNF"

	sudo dnf install -y \
		"clang${LLVM_VERSION}" \
		"clang-tools-extra${LLVM_VERSION}" \
		"lld${LLVM_VERSION}" \
		"llvm${LLVM_VERSION}" \
		"compiler-rt${LLVM_VERSION}" \
		"libcxx${LLVM_VERSION}-devel" \
		"libcxxabi${LLVM_VERSION}-devel"

	for candidate in \
		"clang-${LLVM_VERSION}" \
		"clang${LLVM_VERSION}"; do
		if command -v "${candidate}" &>/dev/null; then
			CLANG_BIN="${candidate}"
			break
		fi
	done

	if [[ -z "${CLANG_BIN:-}" ]]; then
		echo "[llvm] Error: no clang binary found after installation" >&2
		exit 1
	fi

	sudo ln -sf "/usr/bin/${CLANG_BIN}" /usr/bin/clang
	sudo ln -sf "/usr/bin/$(basename "${CLANG_BIN}" | sed 's/clang/clang++/')" /usr/bin/clang++
	sudo ln -sf /usr/bin/lld /usr/bin/ld.lld || true

	echo "[llvm] LLVM ${LLVM_VERSION} installed (DNF): binary: ${CLANG_BIN}"
else
	echo "[llvm] Error: unsupported OS: ${OS_ID}" >&2
	echo "[llvm] Supported: ubuntu, debian, fedora" >&2
	exit 1
fi

echo ""
echo "[llvm] Installed Versions"
clang --version | head -1
echo "lld  : $(ld.lld --version 2>&1 | head -1)"
echo "ar   : $(llvm-ar --version 2>&1 | head -1 || echo 'N/A')"
