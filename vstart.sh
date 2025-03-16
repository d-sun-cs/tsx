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

$CEPH_BUILD/bin/ceph config set global osd_max_object_size 2G

for i in {1..10}; do
  dd if=/dev/urandom of=/tmp/largefile$i bs=4M count=256
done

cd $current_dir