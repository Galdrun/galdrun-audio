# Shared by the build-*.sh scripts in the repository root: source it, do not run it.

die() { printf '%s\n' "$*" >&2; exit 1; }

# Installs those of the given apt packages that are missing, via sudo unless root.
install_packages() {
    local package missing=()
    for package in "$@"; do
        dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'ok installed' || missing+=("$package")
    done
    (( ${#missing[@]} )) || return 0
    command -v apt-get >/dev/null || die "Install these packages, then run again: ${missing[*]}"
    local sudo=
    [[ $EUID -eq 0 ]] || sudo=sudo
    echo "Installing packages: ${missing[*]}"
    $sudo apt-get update
    $sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}"
}

# Makes `dotnet` the SDK the global.json in the current directory names (or a later
# feature band of it), installing it into ~/.dotnet with dotnet-install when missing.
ensure_dotnet() {
    if ! command -v dotnet >/dev/null || ! dotnet --version >/dev/null 2>&1; then
        export DOTNET_ROOT="$HOME/.dotnet" PATH="$HOME/.dotnet:$PATH"
        if ! dotnet --version >/dev/null 2>&1; then
            local installer
            installer=$(mktemp)
            curl --fail --location --retry 3 -o "$installer" https://dot.net/v1/dotnet-install.sh
            bash "$installer" --jsonfile "$PWD/global.json" --install-dir "$HOME/.dotnet"
            rm -f "$installer"
        fi
    fi
    echo "Using .NET SDK $(dotnet --version)"
}

# The cross toolchains are x86-64 binaries and are installed into the user's home.
require_build_user() {
    [[ $EUID -ne 0 ]] || die 'Run as your normal user, not root; tools are installed into your home directory.'
    [[ $(uname -m) == x86_64 ]] || die 'The cross toolchains need an x86-64 Linux host.'
}
