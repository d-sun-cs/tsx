current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
  echo "please change directory to ceph/src/erasure-code/tsx to run this script"
  exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

if $CEPH_BUILD/bin/ceph osd tree | grep -q "down"; then
    echo "Error: Found 'down' status in OSD tree. Exiting."
    cd $current_dir
    exit 1
fi

file_size=$(stat -c%s /tmp/largefile)
start_time=$(date +%s.%N)
$CEPH_BUILD/bin/rados put largefile /tmp/largefile -p tsx-pool
end_time=$(date +%s.%N)
put_time=$(echo "$end_time - $start_time" | bc)
put_speed=$(echo "$file_size / $put_time / 1024 / 1024" | bc -l)

$CEPH_BUILD/bin/rados ls -p tsx-pool
$CEPH_BUILD/bin/ceph osd map tsx-pool largefile 

$CEPH_BUILD/bin/ceph osd stop osd.8
$CEPH_BUILD/bin/ceph osd stop osd.3
$CEPH_BUILD/bin/ceph osd stop osd.1

rm -f $CEPH_BUILD/mylargefile
start_time=$(date +%s.%N)
$CEPH_BUILD/bin/rados get largefile mylargefile -p tsx-pool
end_time=$(date +%s.%N)
get_time=$(echo "$end_time - $start_time" | bc)
get_speed=$(echo "$file_size / $get_time / 1024 / 1024" | bc -l)

for osd_id in 8 3 1; do
    until ! $CEPH_BUILD/bin/ceph osd tree | grep "osd.$osd_id " | grep -q "down"; do
        $CEPH_BUILD/bin/ceph-osd -i $osd_id --cluster ceph
        sleep 1
    done
done

diff $CEPH_BUILD/mylargefile /tmp/largefile
if [ $? -eq 0 ]; then
  echo "TSX test passed! The files are identical!"
fi

echo "File Size: $(echo "$file_size / 1024 / 1024" | bc -l) MB"
echo "Write Time: $put_time seconds"
echo "Write Speed: $put_speed MB/s"
echo "Read Time: $get_time seconds"
echo "Read Speed: $get_speed MB/s"


cd $current_dir