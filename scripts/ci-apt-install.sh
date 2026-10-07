#!/usr/bin/env bash
# Installs Ubuntu packages on a GitHub Actions runner without risking a frozen job.
#
# A plain 'apt update && apt install' has no overall time limit: a mirror that stalls mid-transfer
# (typically azure.archive.ubuntu.com, first entry of the runner mirror list) blocks the job until the
# 6h job limit. Here every network phase is bounded by 'timeout' and retried, and after a failed attempt
# the Azure mirror is dropped from the mirror list so that retries go to archive.ubuntu.com.
# Packages are downloaded first, then installed from the local cache, so a timeout never interrupts dpkg.
#
# The package index is refreshed once per job (the marker lives in RUNNER_TEMP, cleaned between jobs),
# so several steps of the same job can call this script cheaply.
#
# Usage: scripts/ci-apt-install.sh [--llvm <version>] [package...]
#   --llvm <version>  add the apt.llvm.org repository of this LLVM version and install the same toolchain
#                     packages as the official llvm.sh script (clang, lldb, lld, clangd)

set -euo pipefail

readonly max_attempts=3
readonly update_timeout=120
readonly download_timeout=300
readonly install_timeout=900
readonly mirror_list=/etc/apt/apt-mirrors.txt
readonly updated_marker="${RUNNER_TEMP:-/tmp}/.ci-apt-updated"

llvm_version=''
packages=()
while (($# > 0)); do
  case "$1" in
  --llvm)
    llvm_version="$2"
    shift 2
    ;;
  *)
    packages+=("$1")
    shift
    ;;
  esac
done

# Runs apt-get as root, killed after the given number of seconds. timeout signals its whole process
# group, so the apt download method subprocesses are killed too.
AptGet() {
  local seconds="$1"
  shift
  sudo DEBIAN_FRONTEND=noninteractive timeout --kill-after=30 "$seconds" apt-get -y "$@"
}

# Runs a network-bound apt-get phase, retrying it after a failure or a timeout.
AptGetWithRetries() {
  local seconds="$1"
  shift
  local attempt
  for ((attempt = 1; ; ++attempt)); do
    if AptGet "$seconds" "$@"; then
      return 0
    fi
    if ((attempt == max_attempts)); then
      echo "::error::'apt-get $*' failed after $max_attempts attempts" >&2
      return 1
    fi
    echo "::warning::'apt-get $*' failed or timed out after ${seconds}s (attempt $attempt/$max_attempts), retrying" >&2
    if [[ -f $mirror_list ]] && grep -q 'azure\.archive\.ubuntu\.com' "$mirror_list"; then
      echo "Dropping azure.archive.ubuntu.com from $mirror_list" >&2
      sudo sed -i '/azure\.archive\.ubuntu\.com/d' "$mirror_list"
    fi
    sleep $((attempt * 10))
  done
}

# Give up on a connection after 30s without data, so that apt retries or moves to the next mirror early.
sudo tee /etc/apt/apt.conf.d/99-ci-network >/dev/null <<'EOF'
Acquire::Retries "3";
Acquire::http::Timeout "30";
Acquire::https::Timeout "30";
EOF

if [[ -n $llvm_version ]]; then
  # Same key and repository as https://apt.llvm.org/llvm.sh, without its unbounded apt-get calls.
  # shellcheck source=/dev/null
  codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
  curl -fsSL --retry 5 --retry-all-errors --connect-timeout 15 --max-time 60 https://apt.llvm.org/llvm-snapshot.gpg.key |
    sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
  echo "deb http://apt.llvm.org/$codename/ llvm-toolchain-$codename-$llvm_version main" |
    sudo tee "/etc/apt/sources.list.d/llvm-toolchain-$llvm_version.list" >/dev/null
  packages+=("clang-$llvm_version" "lldb-$llvm_version" "lld-$llvm_version" "clangd-$llvm_version")
  # The new repository needs a fresh index.
  rm -f "$updated_marker"
fi

if [[ ! -f $updated_marker ]]; then
  AptGetWithRetries "$update_timeout" update
  touch "$updated_marker"
fi

if ((${#packages[@]} > 0)); then
  AptGetWithRetries "$download_timeout" install --download-only "${packages[@]}"
  # Everything is in the local cache now: the actual install needs no network.
  AptGet "$install_timeout" install --no-download "${packages[@]}"
fi
