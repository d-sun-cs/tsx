current_dir=$(pwd)

if [[ ! $current_dir =~ /ceph/src/erasure-code/tsx$ ]]; then
  echo "please change directory to ceph/src/erasure-code/tsx to run this script"
  exit 1
fi

CEPH_BUILD=$(cd ../../../build && pwd)

cd $CEPH_BUILD

if $CEPH_BUILD/bin/ceph osd tree | grep -q "down"; then
    echo "Error: Found 'down' status in OSD tree. Running restart-osds.sh and exiting."
    ./restart-osds.sh
    echo "Please run this script again after the OSDs have restarted."
    cd $current_dir
    exit 1
fi

file_size=$(stat -c%s /tmp/largefile)
N=10
total_put_time=0
total_get_time=0

for i in {1..10}; do
    start_time=$(date +%s.%N)
    $CEPH_BUILD/bin/rados put largefile /tmp/largefile -p isa-pool
    end_time=$(date +%s.%N)
    put_time=$(echo "$end_time - $start_time" | bc)
    total_put_time=$(echo "$total_put_time + $put_time" | bc)
done

avg_put_time=$(echo "$total_put_time / $N" | bc -l)
put_speed=$(echo "$file_size / $avg_put_time / 1024 / 1024" | bc -l)

$CEPH_BUILD/bin/rados ls -p isa-pool
$CEPH_BUILD/bin/ceph osd map isa-pool largefile 

$CEPH_BUILD/bin/ceph osd stop osd.2
$CEPH_BUILD/bin/ceph osd stop osd.0
$CEPH_BUILD/bin/ceph osd stop osd.5

rm -f $CEPH_BUILD/mylargefile*

for i in {1..10}; do
    start_time=$(date +%s.%N)
    $CEPH_BUILD/bin/rados get largefile mylargefile -p isa-pool
    end_time=$(date +%s.%N)
    get_time=$(echo "$end_time - $start_time" | bc)
    total_get_time=$(echo "$total_get_time + $get_time" | bc)
done

avg_get_time=$(echo "$total_get_time / $N" | bc -l)
get_speed=$(echo "$file_size / $avg_get_time / 1024 / 1024" | bc -l)

for osd_id in 2 0 5; do
    until ! $CEPH_BUILD/bin/ceph osd tree | grep "osd.$osd_id " | grep -q "down"; do
        $CEPH_BUILD/bin/ceph-osd -i $osd_id --cluster ceph
        sleep 1
    done
end

diff $CEPH_BUILD/mylargefile /tmp/largefile
if [ $? -ne 0 ]; then
    echo "ISA test failed! The files largefile and mylargefile are not identical!"
    exit 1
fi

$CEPH_BUILD/bin/rados cleanup -p isa-pool
rm $CEPH_BUILD/mylargefile

echo "ISA test passed! All files are identical!"

echo "File Size: $(echo "$file_size / 1024 / 1024" | bc -l) MB"
echo "Write Time (avg of $N runs): $avg_put_time seconds"
echo "Write Speed: $put_speed MB/s"
echo "Read Time (avg of $N runs): $avg_get_time seconds"
echo "Read Speed: $get_speed MB/s"

cd $current_dir