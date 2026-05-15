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

echo "Debian common utilities are installed."
