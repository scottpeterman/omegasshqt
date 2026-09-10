#!/usr/bin/env bash
# tests/labsshd.sh
#
# Stands up a throwaway sshd on 127.0.0.1:2222 with a key-only lab account,
# so the examples have something real to talk to without depending on any
# device being reachable. Everything lands under tests/.lab/ and nothing
# touches the system ssh configuration.
#
#   ./tests/labsshd.sh start     # bring it up, print the connect details
#   ./tests/labsshd.sh stop      # shut it down
#   ./tests/labsshd.sh clean     # stop and delete all generated material
#
# Needs root (useradd, sshd) and openssh-server. Linux only; on macOS use a
# container or point the examples at a VM instead.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAB="${HERE}/.lab"
PORT="${OMEGASSH_LAB_PORT:-2222}"
USER_NAME="${OMEGASSH_LAB_USER:-labuser}"
SSHD_BIN="${OMEGASSH_SSHD:-/usr/sbin/sshd}"

start() {
    mkdir -p "${LAB}"

    if [[ ! -f "${LAB}/hostkey" ]]; then
        ssh-keygen -q -t ed25519 -f "${LAB}/hostkey" -N ''
        chmod 600 "${LAB}/hostkey"
    fi
    if [[ ! -f "${LAB}/clientkey" ]]; then
        ssh-keygen -q -t ed25519 -f "${LAB}/clientkey" -N ''
    fi
    # A second, unrelated key. policy_suite pins this one to prove that a
    # MISMATCH fails closed -- a corrupted key blob would not do, since an
    # unparseable line is a file-load error rather than a verification result.
    if [[ ! -f "${LAB}/decoy" ]]; then
        ssh-keygen -q -t ed25519 -f "${LAB}/decoy" -N ''
        awk '{print $1" "$2}' "${LAB}/decoy.pub" > "${LAB}/decoy.hostline"
    fi

    if ! id "${USER_NAME}" >/dev/null 2>&1; then
        useradd -m -s /bin/bash "${USER_NAME}"
    fi
    # A fresh account has no password and is therefore locked; sshd refuses
    # a locked account even for pubkey auth. '*' means "no password login"
    # without being locked.
    usermod -p '*' "${USER_NAME}"

    local home
    home="$(getent passwd "${USER_NAME}" | cut -d: -f6)"
    install -d -m 700 -o "${USER_NAME}" -g "${USER_NAME}" "${home}/.ssh"
    install -m 600 -o "${USER_NAME}" -g "${USER_NAME}" \
        "${LAB}/clientkey.pub" "${home}/.ssh/authorized_keys"

    cat > "${LAB}/sshd_config" <<EOF
Port ${PORT}
ListenAddress 127.0.0.1
HostKey ${LAB}/hostkey
PidFile ${LAB}/sshd.pid
UsePAM no
PasswordAuthentication no
PubkeyAuthentication yes
PermitRootLogin no
Subsystem sftp /usr/lib/openssh/sftp-server
EOF

    mkdir -p /run/sshd
    stop >/dev/null 2>&1 || true
    "${SSHD_BIN}" -f "${LAB}/sshd_config" -E "${LAB}/sshd.log"
    sleep 1

    cat <<EOF
lab sshd listening on 127.0.0.1:${PORT}

  key         ${LAB}/clientkey
  user        ${USER_NAME}
  decoy line  ${LAB}/decoy.hostline
  log         ${LAB}/sshd.log

  ./build/examples/c/shell_smoke      ${LAB}/clientkey /tmp/kh_empty
  ./build/examples/c/policy_suite     ${LAB}/clientkey /tmp/kh_lab ${LAB}/decoy.hostline
  ./build/examples/c/knownhosts_probe ${LAB}/clientkey /tmp/kh_lab
  ./build/examples/qt/qt_smoke        127.0.0.1 ${PORT} ${USER_NAME} ${LAB}/clientkey
EOF
}

stop() {
    if [[ -f "${LAB}/sshd.pid" ]]; then
        kill "$(cat "${LAB}/sshd.pid")" 2>/dev/null || true
        rm -f "${LAB}/sshd.pid"
    fi
    echo "lab sshd stopped"
}

clean() {
    stop || true
    rm -rf "${LAB}"
    echo "lab material removed (the ${USER_NAME} account is left alone)"
}

case "${1:-start}" in
    start) start ;;
    stop) stop ;;
    clean) clean ;;
    *)
        echo "usage: $0 {start|stop|clean}" >&2
        exit 2
        ;;
esac
