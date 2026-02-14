#!/bin/bash
# install_dependencies.sh
# This script installs Redis and hiredis dependencies inside the Ceph container
# Must be executed INSIDE the container

set -e

echo "Installing Redis and hiredis dependencies..."

# Update package manager
sudo apt-get install lsb-release curl gpg -y

# Install Redis from official repository
echo "Adding Redis APT repository..."
curl -fsSL https://packages.redis.io/gpg | sudo gpg --dearmor -o /usr/share/keyrings/redis-archive-keyring.gpg
sudo chmod 644 /usr/share/keyrings/redis-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/redis-archive-keyring.gpg] https://packages.redis.io/deb $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/redis.list

sudo apt-get update
sudo apt-get install redis-server -y

# Install hiredis (Redis C client library)
echo "Building and installing hiredis..."
TSX_DIR=$(pwd)
cd ~
git clone https://github.com/redis/hiredis.git
cd hiredis
make
sudo make install
cd "$TSX_DIR"

# Start Redis server
echo "Starting Redis server..."
redis-server &

# Configure Redis for benchmarking (disable persistence for speed)
echo "Configuring Redis..."
sleep 1  # Wait for Redis to start
redis-cli CONFIG SET save ""
redis-cli CONFIG SET appendonly no

echo "Dependencies installation complete!"
echo "Redis is running in the background"
