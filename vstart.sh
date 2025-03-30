#!/bin/bash

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
   echo "please change directory to ceph/src/erasure-code/tsx to run this script"
   exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)
CEPH_SRC=$(cd ../.. && pwd)

cd $CEPH_BUILD

ninja vstart

$CEPH_BUILD/bin/init-ceph stop
$CEPH_SRC/stop.sh
env OSD=11 $CEPH_SRC/vstart.sh -d -n

$CEPH_BUILD/bin/ceph osd erasure-code-profile set tsx-profile plugin=tsx crush-failure-domain=osd
$CEPH_BUILD/bin/ceph osd pool create tsx-pool erasure tsx-profile
$CEPH_BUILD/bin/ceph osd pool set tsx-pool min_size 8

$CEPH_BUILD/bin/ceph osd erasure-code-profile set isa-profile plugin=isa k=8 m=3 crush-failure-domain=osd
$CEPH_BUILD/bin/ceph osd pool create isa-pool erasure isa-profile
$CEPH_BUILD/bin/ceph osd pool set isa-pool min_size 8

$CEPH_BUILD/bin/ceph osd erasure-code-profile set tsx-8k-profile plugin=tsx crush-failure-domain=osd stripe_unit=8K
$CEPH_BUILD/bin/ceph osd pool create tsx-8k-pool erasure tsx-8k-profile
$CEPH_BUILD/bin/ceph osd pool set tsx-8k-pool min_size 8

$CEPH_BUILD/bin/ceph osd erasure-code-profile set isa-8k-profile plugin=isa k=8 m=3 crush-failure-domain=osd stripe_unit=8K
$CEPH_BUILD/bin/ceph osd pool create isa-8k-pool erasure isa-8k-profile
$CEPH_BUILD/bin/ceph osd pool set isa-8k-pool min_size 8

$CEPH_BUILD/bin/ceph osd erasure-code-profile set tsx-16k-profile plugin=tsx crush-failure-domain=osd stripe_unit=16K
$CEPH_BUILD/bin/ceph osd pool create tsx-16k-pool erasure tsx-16k-profile
$CEPH_BUILD/bin/ceph osd pool set tsx-16k-pool min_size 8

$CEPH_BUILD/bin/ceph osd erasure-code-profile set isa-16k-profile plugin=isa k=8 m=3 crush-failure-domain=osd stripe_unit=16K
$CEPH_BUILD/bin/ceph osd pool create isa-16k-pool erasure isa-16k-profile
$CEPH_BUILD/bin/ceph osd pool set isa-16k-pool min_size 8

$CEPH_BUILD/bin/ceph config set global osd_max_object_size 3G

dd if=/dev/zero of=/tmp/largefile bs=2GiB count=1

cd $current_dir