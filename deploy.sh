#!/usr/bin/env bash
set -euo pipefail

WORKFLOW_PRESET="${WORKFLOW_PRESET:-cp0-cross-package}"
REMOTE_USER="${REMOTE_USER:-}"
REMOTE_HOST="${REMOTE_HOST:-}"
REMOTE_DIR="${REMOTE_DIR:-/home/${REMOTE_USER}}"
PACKAGE_GLOB="${PACKAGE_GLOB:-dist/DaliCam_*_arm64.deb}"
REMOTE_INSTALL="${REMOTE_INSTALL:-1}"
REMOTE_CLEANUP="${REMOTE_CLEANUP:-1}"
SSH_PASSWORD="${SSH_PASSWORD:-}"
CMAKE_BIN="${CMAKE_BIN:-}"

if [[ -z "${CMAKE_BIN}" ]]; then
  if [[ -x /tmp/cmake-venv/bin/cmake ]]; then
    CMAKE_BIN="/tmp/cmake-venv/bin/cmake"
  else
    CMAKE_BIN="$(command -v cmake)"
  fi
fi

log() {
  printf '\n==> %s\n' "$*"
}

log "Workflow: ${CMAKE_BIN} --workflow --preset ${WORKFLOW_PRESET}"
"${CMAKE_BIN}" --workflow --preset "${WORKFLOW_PRESET}"

shopt -s nullglob
packages=( ${PACKAGE_GLOB} )
shopt -u nullglob

if (( ${#packages[@]} == 0 )); then
  printf 'No package matched: %s\n' "${PACKAGE_GLOB}" >&2
  exit 1
fi

latest_package="$(ls -t "${packages[@]}" | head -n 1)"

if [[ -z "${REMOTE_USER}" || -z "${REMOTE_HOST}" ]]; then
  printf 'Package ready: %s\n' "${latest_package}" >&2
  printf 'Set REMOTE_USER and REMOTE_HOST to deploy, for example:\n' >&2
  printf '  REMOTE_USER=pi REMOTE_HOST=192.168.1.10 ./deploy.sh\n' >&2
  exit 1
fi

remote_target="${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}"
remote_package_path="${REMOTE_DIR}/$(basename "${latest_package}")"

ssh_cmd=(ssh)
scp_cmd=(scp)
if [[ -n "${SSH_PASSWORD}" ]]; then
  if ! command -v sshpass >/dev/null 2>&1; then
    printf 'sshpass is required when SSH_PASSWORD is set\n' >&2
    exit 1
  fi
  ssh_cmd=(sshpass -p "${SSH_PASSWORD}" ssh)
  scp_cmd=(sshpass -p "${SSH_PASSWORD}" scp)
fi

log "Deploy: scp ${latest_package} ${remote_target}"
"${scp_cmd[@]}" "${latest_package}" "${remote_target}"

if [[ "${REMOTE_INSTALL}" == "1" ]]; then
  log "Install: ssh ${REMOTE_USER}@${REMOTE_HOST} sudo -n dpkg -i ${remote_package_path}"
  "${ssh_cmd[@]}" "${REMOTE_USER}@${REMOTE_HOST}" "sudo -n dpkg -i '${remote_package_path}'"
fi

if [[ "${REMOTE_CLEANUP}" == "1" ]]; then
  log "Cleanup: ssh ${REMOTE_USER}@${REMOTE_HOST} rm -f ${remote_package_path}"
  "${ssh_cmd[@]}" "${REMOTE_USER}@${REMOTE_HOST}" "rm -f '${remote_package_path}'"
fi

log "Done: ${latest_package} -> ${remote_target}"
