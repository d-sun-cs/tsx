#!/bin/bash

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
   echo "please change directory to ceph/src/erasure-code/tsx to run this script"
   exit 1
fi

CEPH_BUILD=$(cd ../../../ && pwd)/build
CEPH_ROOT=$(cd ../../../ && pwd)

cd $CEPH_ROOT

# rm -rf build
sudo rm -rf build
# ./do_cmake.sh -DCMAKE_INSTALL_PREFIX=/opt/ceph -DCMAKE_C_FLAGS="-Og -g3 -gdwarf-4"
./do_cmake.sh -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-mavx2 -O3" -DCMAKE_C_FLAGS="-mavx2 -O3"

cd $CEPH_BUILD

# ninja vstart
# this command need to be executed one more time if error (in /ceph/build dir)
ninja

cd $current_dir
