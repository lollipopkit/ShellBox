#!/bin/sh
set -eu

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    apt-utils \
    bash-completion \
    build-essential \
    ca-certificates \
    curl \
    dbus \
    file \
    fish \
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
    systemd \
    systemd-sysv \
    tzdata \
    vim-tiny \
    wget

apt-get clean
rm -rf /var/lib/apt/lists/*

if command -v fish >/dev/null 2>&1; then
    fish -lc 'curl -sL https://raw.githubusercontent.com/jorgebucaran/fisher/main/functions/fisher.fish | source; fisher install jorgebucaran/fisher; fisher install (curl -fsSL https://raw.githubusercontent.com/lollipopkit/fish-cfg/main/fish/fish_plugins | string split \n)'
fi

echo "Debian common utilities are installed."
