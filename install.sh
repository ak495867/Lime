#!/bin/bash
set -e

GREEN='\033[0;32m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}======================================${NC}"
echo -e "${CYAN}       Installing LIME Hypervisor     ${NC}"
echo -e "${CYAN}======================================${NC}"

# Detect OS and architecture
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
if [ "$ARCH" = "x86_64" ]; then
    ARCH="amd64"
elif [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    ARCH="arm64"
else
    echo -e "${CYAN}Unsupported architecture: $ARCH${NC}"
    exit 1
fi

DOWNLOAD_URL="https://github.com/ak495867/Lime/releases/latest/download/lime-$OS-$ARCH"

echo -e "${BLUE}==>${NC} Fetching latest binary for $OS-$ARCH..."
curl -# -fSL "$DOWNLOAD_URL" -o lime
chmod +x lime

echo -e "${BLUE}==>${NC} Installing to /usr/local/bin/lime (requires sudo)"
sudo mv lime /usr/local/bin/lime

echo -e "${GREEN}==> LIME installed successfully! ??${NC}"
echo -e "${CYAN}Run 'lime --help' to get started.${NC}"
