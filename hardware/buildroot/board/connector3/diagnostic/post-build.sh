#!/usr/bin/env bash
set -euo pipefail
target=$1
board=$(cd -- "$(dirname -- "$0")" && pwd)
: "${GALDRUN_SSH_PUBKEY:?Set GALDRUN_SSH_PUBKEY to your public SSH key}"
if ! grep -Eq '^(ssh-|ecdsa-|sk-)' "$GALDRUN_SSH_PUBKEY" || grep -q 'PRIVATE KEY' "$GALDRUN_SSH_PUBKEY"; then
    echo 'GALDRUN_SSH_PUBKEY must contain a public key, never a private key.' >&2
    exit 2
fi
ssh-keygen -l -f "$GALDRUN_SSH_PUBKEY" >/dev/null
install -m 0755 "$board/init.sh" "$target/init"
install -D -m 0600 "$GALDRUN_SSH_PUBKEY" "$target/root/.ssh/authorized_keys"
chmod 0700 "$target/root/.ssh"
cat >"$target/etc/ssh/sshd_config" <<'EOF'
Port 22
PermitRootLogin prohibit-password
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
PermitEmptyPasswords no
AuthenticationMethods publickey
AuthorizedKeysFile .ssh/authorized_keys
Subsystem sftp /usr/libexec/sftp-server
EOF
# An unmatchable, unlocked password permits pubkey login. Password auth is off.
sed -i 's/^root:[^:]*:/root:*:/' "$target/etc/shadow"
rm -f "$target"/etc/ssh/ssh_host_*key*
