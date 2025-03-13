current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
  echo "please change directory to ceph/src/erasure-code/tsx to run this script"
  exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

dd if=/dev/urandom of=/tmp/largefile bs=4M count=256
$CEPH_BUILD/bin/rados put largefile /tmp/largefile -p tsx-pool
$CEPH_BUILD/bin/rados ls -p tsx-pool
$CEPH_BUILD/bin/ceph osd map tsx-pool largefile 

$CEPH_BUILD/bin/ceph osd stop osd.8
$CEPH_BUILD/bin/ceph osd stop osd.3
$CEPH_BUILD/bin/ceph osd stop osd.1

rm -f $CEPH_BUILD/mylargefile
$CEPH_BUILD/bin/rados get largefile mylargefile -p tsx-pool
diff $CEPH_BUILD/mylargefile /tmp/largefile
if [ $? -eq 0 ]; then
  echo "Test passed! The files are identical"
fi

cd $current_dir