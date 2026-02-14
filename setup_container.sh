#!/bin/bash
# setup_container.sh
# Docker container creation and initialization script
# Must be executed on the HOST MACHINE (outside container)
# Prerequisites: ceph directory must already exist with tsx plugin cloned

set -e

echo "FastTT Ceph Container Setup"
echo "============================"
echo ""

# Check if we're in the tsx directory
if [ ! -d "update_cmake.sh" ] && [ ! -f "update_cmake.sh" ]; then
    echo "Error: This script must be run from the tsx directory!"
    echo "Expected location: ceph/src/erasure-code/tsx/"
    exit 1
fi

echo "Step 1: Running initialization scripts..."

if [ -f "update_cmake.sh" ]; then
    echo "Running update_cmake.sh..."
    bash update_cmake.sh
else
    echo "Error: update_cmake.sh not found!"
    exit 1
fi

if [ -f "update_docker_build.sh" ]; then
    echo "Running update_docker_build.sh..."
    bash update_docker_build.sh
else
    echo "Warning: update_docker_build.sh not found!"
fi

echo ""
echo "Step 2: Creating development container..."
echo "Note: This may take 10-30 minutes depending on your internet connection"
echo ""

# Navigate to ceph root directory
cd ../../..

python3 ./src/script/build-with-container.py -d ubuntu22.04 -e interactive --keep-container

echo ""
echo "==========================================="
echo "Container creation complete!"
echo "You are now inside the container."
echo ""
echo "Next steps (inside the container):"
echo "1. bash init_container_env.sh"
echo "2. bash install_dependencies.sh"
echo "3. cd /ceph/src/erasure-code/tsx && bash recmake.sh"
echo "4. bash run_all_benchmarks.sh"
echo "==========================================="
