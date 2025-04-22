#!/bin/bash

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
  echo "please change directory to ceph/src/erasure-code/tsx to run this script"
  exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)
CEPH_SRC=$(cd ../.. && pwd)

cd $CEPH_BUILD

$CEPH_BUILD/bin/init-ceph stop
$CEPH_SRC/stop.sh
rm -rf out dev
# 删除largefile
rm -f ./largefile

cd $current_dir
