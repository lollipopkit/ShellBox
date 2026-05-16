#!/bin/sh
set -u

LOG=/ish/setup-debian-utils.log
mkdir -p /ish
rm -f /ish/fish-setup-failed
: >"$LOG"

export DEBIAN_FRONTEND=noninteractive

fish_only=0
if [ "${1:-}" = "--fish-only" ]; then
    fish_only=1
fi

run_logged() {
    echo "$ $*" >>"$LOG"
    "$@" >>"$LOG" 2>&1
}

print_log_tail() {
    echo "---- /ish/setup-debian-utils.log ----"
    tail -n 80 "$LOG" 2>/dev/null || true
    echo "---- end log ----"
}

clean_apt_lists() {
    if [ -d /var/lib/apt/lists ]; then
        find /var/lib/apt/lists -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    fi
    mkdir -p /var/lib/apt/lists/partial
}

configure_dns() {
    cat >/etc/resolv.conf <<'EOF'
nameserver 1.1.1.1
nameserver 8.8.8.8
nameserver 2606:4700:4700::1111
nameserver 2001:4860:4860::8888
EOF
}

configure_apt_sources() {
    release=trixie
    security_suite=trixie-security
    if [ -r /etc/debian_version ]; then
        case "$(cat /etc/debian_version)" in
            12.*|bookworm*)
                release=bookworm
                security_suite=bookworm-security
                ;;
        esac
    fi

    mkdir -p /etc/apt/sources.list.d
    rm -f /etc/apt/sources.list.d/debian.sources
    cat >/etc/apt/sources.list <<EOF
deb http://deb.debian.org/debian $release main contrib non-free non-free-firmware
deb http://deb.debian.org/debian $release-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $security_suite main contrib non-free non-free-firmware
EOF
}

configure_hosts_fallback() {
    if ! grep -q 'shellbox apt fallback' /etc/hosts 2>/dev/null; then
        cat >>/etc/hosts <<'EOF'
# shellbox apt fallback
146.75.46.132 deb.debian.org
151.101.2.132 security.debian.org
EOF
    fi
}

repair_profile_file() {
    if [ -d /etc/profile ]; then
        rm -rf /etc/profile
    fi
    if [ ! -f /etc/profile ]; then
        cp /usr/share/base-files/profile /etc/profile 2>/dev/null || cat >/etc/profile <<'EOF'
if [ "$(id -u)" -eq 0 ]; then
  PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
else
  PATH="/usr/local/bin:/usr/bin:/bin:/usr/local/games:/usr/games"
fi
export PATH
EOF
        chmod 644 /etc/profile
    fi
}

repair_base_files_postinst() {
    script=/var/lib/dpkg/info/base-files.postinst
    [ -f "$script" ] || return 0

    tmp="$script.shellbox"
    sed \
        -e 's/^[[:space:]]*install_from_default profile[[:space:]][[:space:]]*\/etc\/profile/: # shellbox-skip-profile-install/' \
        -e 's/^[[:space:]]*update_to_current_default profile[[:space:]][[:space:]]*\/etc\/profile/: # shellbox-skip-profile-update/' \
        -e 's/^[[:space:]]*install_from_default dot.profile[[:space:]][[:space:]]*\/root\/.profile/: # shellbox-skip-profile-install/' \
        -e 's/^[[:space:]]*update_to_current_default dot.profile[[:space:]][[:space:]]*\/root\/.profile/: # shellbox-skip-profile-update/' \
        "$script" >"$tmp"
    cat "$tmp" >"$script"
    rm -f "$tmp"
    chmod 755 "$script" 2>/dev/null || true
}

repair_debian_file_paths() {
    rm -rf /etc/shells
    cat >/etc/shells <<'EOF'
/bin/sh
/usr/bin/sh
/bin/bash
/usr/bin/bash
/usr/bin/fish
EOF
    chmod 644 /etc/shells

    rm -rf /etc/ssl/certs/ca-certificates.crt
    mkdir -p /etc/ssl/certs
    tmp=/etc/ssl/certs/ca-certificates.crt.shellbox
    : >"$tmp"
    for cert_dir in /usr/share/ca-certificates/mozilla /usr/local/share/ca-certificates; do
        if [ -d "$cert_dir" ]; then
            find "$cert_dir" -type f -name '*.crt' -exec cat {} \; >>"$tmp" 2>/dev/null || true
        fi
    done
    cat "$tmp" >/etc/ssl/certs/ca-certificates.crt
    rm -f "$tmp"
    chmod 644 /etc/ssl/certs/ca-certificates.crt
}

repair_problem_package_postinsts() {
    if [ -f /var/lib/dpkg/info/fish.postinst ]; then
        cat >/var/lib/dpkg/info/fish.postinst <<'EOF'
#!/bin/sh
set -e
rm -rf /etc/shells
cat >/etc/shells <<'SHELLS'
/bin/sh
/usr/bin/sh
/bin/bash
/usr/bin/bash
/usr/bin/fish
SHELLS
chmod 644 /etc/shells
exit 0
EOF
        chmod 755 /var/lib/dpkg/info/fish.postinst
    fi

    if [ -f /var/lib/dpkg/info/ca-certificates.postinst ]; then
        cat >/var/lib/dpkg/info/ca-certificates.postinst <<'EOF'
#!/bin/sh
set -e
rm -rf /etc/ssl/certs/ca-certificates.crt
mkdir -p /etc/ssl/certs
tmp=/etc/ssl/certs/ca-certificates.crt.shellbox
: >"$tmp"
for cert_dir in /usr/share/ca-certificates/mozilla /usr/local/share/ca-certificates; do
    if [ -d "$cert_dir" ]; then
        find "$cert_dir" -type f -name '*.crt' -exec cat {} \; >>"$tmp" 2>/dev/null || true
    fi
done
cat "$tmp" >/etc/ssl/certs/ca-certificates.crt
rm -f "$tmp"
chmod 644 /etc/ssl/certs/ca-certificates.crt
exit 0
EOF
        chmod 755 /var/lib/dpkg/info/ca-certificates.postinst
    fi
}

reset_fish_package_state() {
    rm -f /ish/fish-ready
    if run_logged apt-get purge -y fish fish-common; then
        return 0
    fi

    echo "apt-get purge fish failed; forcing broken fish package state removal."
    repair_debian_file_paths
    repair_problem_package_postinsts
    run_logged dpkg --remove --force-remove-reinstreq fish fish-common || true
    run_logged dpkg --purge --force-remove-reinstreq fish fish-common || true
    run_logged apt-get install -f -y --no-install-recommends || true
}

repair_profile_file
repair_base_files_postinst
repair_debian_file_paths
repair_problem_package_postinsts
configure_dns
configure_apt_sources
configure_hosts_fallback
clean_apt_lists

echo "Updating APT indexes..."
if ! run_logged apt-get update -o APT::Update::Error-Mode=any; then
    echo "apt-get update failed; applying DNS hosts fallback and retrying."
    configure_hosts_fallback
    if ! run_logged apt-get update -o APT::Update::Error-Mode=any; then
        touch /ish/fish-setup-failed
        echo "apt-get update failed; fish cannot be installed without package indexes."
        print_log_tail
        echo "Run /ish/setup-debian-utils.sh --fish-only to retry after fixing the APT/network error."
        exit 1
    fi
fi

echo "Resetting fish package state..."
reset_fish_package_state

echo "Installing fish..."
if ! run_logged apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    fish; then
    echo "apt-get install fish failed; repairing package postinst scripts and retrying configuration."
    repair_debian_file_paths
    repair_problem_package_postinsts
    if ! run_logged dpkg --configure -a; then
        touch /ish/fish-setup-failed
        echo "dpkg --configure -a failed after package postinst repair."
        print_log_tail
        echo "See /ish/setup-debian-utils.log, then run /ish/setup-debian-utils.sh --fish-only to retry."
        exit 1
    fi
    if ! run_logged apt-get install -f -y --no-install-recommends; then
        touch /ish/fish-setup-failed
        echo "apt-get install -f failed after package postinst repair."
        print_log_tail
        echo "See /ish/setup-debian-utils.log, then run /ish/setup-debian-utils.sh --fish-only to retry."
        exit 1
    fi
    if ! run_logged apt-get install -y --no-install-recommends fish; then
        touch /ish/fish-setup-failed
        echo "apt-get install fish failed after package postinst repair."
        print_log_tail
        echo "See /ish/setup-debian-utils.log, then run /ish/setup-debian-utils.sh --fish-only to retry."
        exit 1
    fi
fi

if command -v fish >/dev/null 2>&1; then
    touch /ish/fish-ready
    rm -f /ish/fish-setup-failed
    (
        if ! fish -lc 'curl --connect-timeout 15 --max-time 60 -sL https://raw.githubusercontent.com/jorgebucaran/fisher/main/functions/fisher.fish | source; fisher install jorgebucaran/fisher; fisher install (curl --connect-timeout 15 --max-time 60 -fsSL https://raw.githubusercontent.com/lollipopkit/fish-cfg/main/fish/fish_plugins | string split \n)' >>"$LOG" 2>&1; then
            echo "fisher plugin setup failed; fish itself is installed." >>"$LOG"
        fi
    ) &
else
    touch /ish/fish-setup-failed
    echo "fish binary was not found after apt-get install."
    exit 1
fi

if [ "$fish_only" -eq 1 ]; then
    echo "fish is installed."
    exit 0
fi

echo "Installing common utilities..."
if ! run_logged apt-get install -y --no-install-recommends \
    apt-utils \
    bash-completion \
    build-essential \
    dbus \
    file \
    git \
    iproute2 \
    iputils-ping \
    less \
    locales \
    nano \
    netbase \
    openssh-client \
    procps \
    sudo \
    tzdata \
    vim-tiny \
    wget; then
    echo "common utilities install failed; fish itself is installed."
    print_log_tail
    echo "Run /ish/setup-debian-utils.sh again after fixing the APT/network error."
    exit 1
fi

apt-get clean
clean_apt_lists

echo "Debian common utilities are installed."
