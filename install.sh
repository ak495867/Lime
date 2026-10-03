#!/bin/bash
set -e

echo "=> Installing LIME..."

# Detect OS and architecture
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
if [ "$ARCH" = "x86_64" ]; then
    ARCH="amd64"
elif [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    ARCH="arm64"
else
    echo "Unsupported architecture: $ARCH"
    exit 1
fi

DOWNLOAD_URL="https://github.com/ak495867/Lime/releases/latest/download/lime-${OS}-${ARCH}"

echo "=> Downloading from $DOWNLOAD_URL"
curl -fsSL "$DOWNLOAD_URL" -o lime
chmod +x lime

echo "=> Moving to /usr/local/bin/lime (requires sudo)"
sudo mv lime /usr/local/bin/lime

echo "=> LIME installed successfully! You can now run 'lime' from anywhere."
