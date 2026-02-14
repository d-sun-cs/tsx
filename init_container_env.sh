#!/bin/bash
# init_container_env.sh
# Basic environment initialization inside the container
# Must be executed INSIDE the container

set -e

echo "Initializing container environment..."

echo "Step 1: Installing sudo..."
apt install sudo -y

echo "Step 2: Configuring Git for safe directories..."
git config --global --add safe.directory "*"

echo ""
echo "Container environment initialization complete!"
echo ""
echo "Next steps:"
echo "1. Run: bash install_dependencies.sh"
echo "2. Run: cd /ceph/src/erasure-code/tsx/ && bash recmake.sh"
