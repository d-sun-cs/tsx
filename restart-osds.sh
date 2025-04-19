#!/bin/bash

current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
  echo "please change directory to ceph/src/erasure-code/tsx to run this script"
  exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

$CEPH_BUILD/bin/ceph-osd -i 0 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 1 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 2 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 3 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 4 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 5 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 6 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 7 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 8 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 9 --cluster ceph 
$CEPH_BUILD/bin/ceph-osd -i 10 --cluster ceph 

cd $current_dir