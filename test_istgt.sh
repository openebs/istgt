#!/bin/bash

DIR=$PWD
SETUP_ISTGT=$DIR/src/setup_istgt.sh
REPLICATION_TEST=$DIR/src/replication_test
TEST_SNAPSHOT=$DIR/test_snapshot.sh
MEMPOOL_TEST=$DIR/src/mempool_test
ISTGT_INTEGRATION=$DIR/src/istgt_integration
ISCSIADM=iscsiadm
ISTGTCONTROL=istgtcontrol
SETUP_PID=-1
device_name=""
LOGFILE="/tmp/istgt.log"
INTEGRATION_TEST_LOGFILE="/tmp/integration_test.log"
CONTROLLER_IP="127.0.0.1"
CONTROLLER_PORT="6060"
REPLICATION_FACTOR=3
CONSISTTENCY_FACTOR=2
IOPING="ioping"

CURDIR=$PWD

which $IOPING >> /dev/null
if [ $? -ne 0 ]; then
	echo "$IOPING is not installed.. exiting.."
	exit 1
fi

on_exit() {
	cd $CURDIR
	COREFILE=`find $CURDIR |grep core`
	if [ ! -z $COREFILE ]; then
		tail -100 $LOGFILE
		tail -100 $INTEGRATION_TEST_LOGFILE
		gdb  -ex "quit" -c $COREFILE
		file=`strings $COREFILE  |grep ^'_=' |grep "\/" |tail -1 |awk -F '=' '{print $2}'`
		echo "Corefile generated by $file"
		gdb --batch --quiet -ex "thread apply all bt full" -ex "quit" $file $COREFILE
	fi
	tail -100 $LOGFILE
	tail -100 $INTEGRATION_TEST_LOGFILE
}

ulimit -c unlimited
trap 'on_exit' EXIT

source $SETUP_ISTGT

login_to_volume() {
	$ISCSIADM -m discovery -t st -p $1
	$ISCSIADM -m node -l
}

logout_of_volume() {
	$ISCSIADM -m node -u
	$ISCSIADM -m node -o delete
}

get_scsi_disk() {
	device_name=$($ISCSIADM -m session -P 3 |grep -i "Attached scsi disk" | awk '{print $4}')
	i=0
	while [ -z $device_name ]; do
		sleep 5
		device_name=$($ISCSIADM -m session -P 3 |grep -i "Attached scsi disk" | awk '{print $4}')
		i=`expr $i + 1`
		if [ $i -eq 10 ]; then
			echo "scsi disk not found";
			tail -20 $LOGFILE
			exit;
		else
			continue;
		fi
	done
}

start_istgt() {
	cd $DIR/src
	run_istgt $* >> $LOGFILE 2>&1 &
	SETUP_PID=$!
	echo $SETUP_PID
	## wait for couple of seconds to start the istgt target
	sleep 5
	cd ..
}

start_replica() {
	$REPLICATION_TEST $* >> $LOGFILE 2>&1
}

stop_istgt() {
	if [ $SETUP_PID -ne -1 ]; then
		pkill -9 -P $(list_descendants $SETUP_PID)
		kill -9 $(list_descendants $SETUP_PID)
		pkill -9 -P $SETUP_PID
		kill -9 $SETUP_PID
	fi

}

run_mempool_test()
{
	$MEMPOOL_TEST
	[[ $? -ne 0 ]] && echo "mempool test failed" && tail -20 $LOGFILE && exit 1
	return 0
}

run_istgt_integration()
{
	local pid_istgt=$(sudo lsof -t -i:6060)
	echo "istgt PID is $pid_istgt"
	kill -9 $pid_istgt
	pkill -9 -x istgt/src/replication_test
	ps -aux | grep 'istgt'
	truncate -s 5G /tmp/test_vol1 /tmp/test_vol2 /tmp/test_vol3
	export externalIP=127.0.0.1
	echo $externalIP
	$ISTGT_INTEGRATION >> $INTEGRATION_TEST_LOGFILE 2>&1
	[[ $? -ne 0 ]] && echo "istgt integration test failed" && tail -30 $INTEGRATION_TEST_LOGFILE && exit 1
	rm -f /tmp/test_vol*
	rm $INTEGRATION_TEST_LOGFILE
	return 0
}

run_and_verify_iostats() {
	login_to_volume "$CONTROLLER_IP:3260"
	get_scsi_disk
	if [ "$device_name"!="" ]; then
		sudo mkfs.ext4 -F /dev/$device_name
		[[ $? -ne 0 ]] && echo "mkfs failed for $device_name" && exit 1

		sudo mount /dev/$device_name /mnt/store
		[[ $? -ne 0 ]] && echo "mount for $device_name" && exit 1

		sudo dd if=/dev/urandom of=/mnt/store/file1 bs=4k count=10000 oflag=direct
		$ISTGTCONTROL iostats
		var1="$($ISTGTCONTROL iostats | grep -oP "(?<=TotalWriteBytes\": \")[^ ]+" | cut -d '"' -f 1)"
		if [ $var1 -eq 0 ]; then
			echo "iostats command failed" && exit 1
		fi

		sudo dd if=/dev/urandom of=/mnt/store/file1 bs=4k count=10000 oflag=direct
		$ISTGTCONTROL iostats
		var2="$($ISTGTCONTROL iostats | grep -oP "(?<=TotalWriteBytes\": \")[^ ]+" | cut -d '"' -f 1)"
		if [ $var2 -eq 0 ]; then
			echo "iostats command failed" && exit 1
		fi

		if [ "$var2" == "$var1" ]; then
			echo "iostats command failed, both the values are same" && exit 1
		else
			echo "iostats test passed"
		fi

		sudo umount /mnt/store
		logout_of_volume
		readReqTime="$($ISTGTCONTROL -q iostats | jq '.Replicas[0].ReadReqTime' | cut -d '"' -f 2)"
		readRespTime="$($ISTGTCONTROL -q iostats | jq '.Replicas[0].ReadRespTime' | cut -d '"' -f 2)"
		writeReqTime="$($ISTGTCONTROL -q iostats | jq '.Replicas[0].WriteReqTime' | cut -d '"' -f 2)"
		writeRespTime="$($ISTGTCONTROL -q iostats | jq '.Replicas[0].WriteRespTime' | cut -d '"' -f 2)"
		if [ $readReqTime -gt $readRespTime ] || [ $writeReqTime -gt $writeRespTime ]; then
			echo "Issue in replica latency stats" && tail -20 $LOGFILE && exit 1
		fi
		sleep 5
	else
		echo "Unable to detect iSCSI device, login failed"; exit 1
	fi
}

write_and_verify_data(){
	login_to_volume "$CONTROLLER_IP:3260"
	sleep 5
	get_scsi_disk
	if [ "$device_name"!="" ]; then
		mkfs.ext4 -F /dev/$device_name
		[[ $? -ne 0 ]] && echo "mkfs failed for $device_name" && tail -20 $LOGFILE && exit 1

		mount /dev/$device_name /mnt/store
		[[ $? -ne 0 ]] && echo "mount for $device_name" && tail -20 $LOGFILE && exit 1

		dd if=/dev/urandom of=file1 bs=4k count=10000
		hash1=$(md5sum file1 | awk '{print $1}')
		cp file1 /mnt/store
		hash2=$(md5sum /mnt/store/file1 | awk '{print $1}')
		if [ $hash1 == $hash2 ]; then echo "DI Test: PASSED"
		else
			rm file1
			echo "DI Test: FAILED";
			tail -20 $LOGFILE
			exit 1
		fi
		rm file1

		umount /mnt/store
		logout_of_volume
		sleep 5
	else
		echo "Unable to detect iSCSI device, login failed";
		tail -20 $LOGFILE
		exit 1
	fi
}

write_data()
{
	local offset=$1
	local len=$2
	local base=$3
	local output1=$4
	local output2=$5

	seek=$(( $offset / $base ))
	count=$(( $len / $base ))

	dd if=/dev/urandom | tr -dc 'a-zA-Z0-9'| head  -c $len  | \
	    tee >(dd of=$output1 conv=notrunc bs=$base count=$count seek=$seek oflag=direct) | \
	    (dd of=$output2 conv=notrunc bs=$base count=$count seek=$seek oflag=direct)
}

setup_test_env() {
	rm -f /tmp/test_vol*
	mkdir -p /mnt/store
	truncate -s 5G /tmp/test_vol1 /tmp/test_vol2 /tmp/test_vol3 $1
	logout_of_volume
	sudo killall -9 istgt
	sudo killall -9 replication_test
	touch $INTEGRATION_TEST_LOGFILE
	start_istgt 5G
}

cleanup_test_env() {
	stop_istgt
	rm -rf /mnt/store
}

wait_for_pids()
{
	for p in "$@"; do
		wait $p
		status=$?
		if [ $status -ne 0 ] && [ $status -ne 127 ]; then
			tail -20 $LOGFILE
			exit 1
		fi
	done
}

list_descendants ()
{
	local children=$(ps -o pid= --ppid "$1")

	for pid in $children
	do
		list_descendants "$pid"
	done

	echo "$children"
}

run_data_integrity_test() {
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"

	setup_test_env
	$TEST_SNAPSHOT 0

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V "/tmp/test_vol1" -q  &
	replica1_pid=$!
	$TEST_SNAPSHOT 0

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V "/tmp/test_vol2" -q  &
	replica2_pid=$!
	$TEST_SNAPSHOT 0

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V "/tmp/test_vol3" -q &
	replica3_pid=$!
	sleep 15

	$TEST_SNAPSHOT 1 &
	test_snapshot_pid=$!

	write_and_verify_data
	wait_for_pids $test_snapshot_pid

	$TEST_SNAPSHOT 1

	pkill -9 -P $replica1_pid
	kill -SIGKILL $replica1_pid
	sleep 5
	write_and_verify_data

	#sleep is required for more than 60 seconds, as status messages are sent every 60 seconds
	sleep 65
	ps -auxwww
	ps -o pid,ppid,command
	$TEST_SNAPSHOT 0

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V "/tmp/test_vol1" -q &
	replica1_pid=$!
	sleep 5
	write_and_verify_data
	$TEST_SNAPSHOT 1

	sleep 65
	ps -auxwww
	ps -o pid,ppid,command
	$TEST_SNAPSHOT 1

	pkill -9 -P $replica1_pid
	kill -SIGKILL $replica1_pid

	# test replica IO timeout
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V "/tmp/test_vol1" -q -n 500&
	replica1_pid1=$!
	sleep 5
	write_and_verify_data
	sleep 5
	write_and_verify_data
	sleep 5

	run_and_verify_iostats

	wait $replica1_pid1
	if [ $? == 0 ]; then
		echo "Replica timeout failed"
		exit 1
	else
		echo "Replica timeout passed"
	fi

	$ISTGTCONTROL -q iostats

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V "/tmp/test_vol1" -q &
	replica1_pid=$!
	sleep 5
	ps aux |grep replication_test

	pkill -9 -P $replica1_pid
	pkill -9 -P $replica1_pid1
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid

	kill -SIGKILL $replica1_pid
	kill -SIGKILL $replica1_pid1
	kill -SIGKILL $replica2_pid
	kill -SIGKILL $replica3_pid

	cleanup_test_env

	ps -auxwww
	ps -o pid,ppid,command

}

run_read_consistency_test ()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local replica2_vdev="/tmp/test_vol2"
	local replica3_vdev="/tmp/test_vol3"
	local file_name="/root/data_file"
	local device_file="/root/device_file"
	local w_pid

	# Test to check if replication module is not initialized then
	# istgtcontrol should return an error
	export ReplicationDelay=40
	setup_test_env
	sleep 2
	ps -aux | grep 'istgt'
	istgtcontrol status >/dev/null  2>&1
	if [ $? -ne 0 ]; then
		echo "ISTGTCONTROL returned error as replication module not initialized"
	else
		echo "ISTGTCONTROL returned success .. something went wrong"
		stop_istgt
		return
	fi

	unset ReplicationDelay

	sleep 60
	rm -rf $file_name $device_file

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q  &
	replica1_pid=$!

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica2_vdev  -q  &
	replica2_pid=$!

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica3_vdev -q  &
	replica3_pid=$!
	sleep 5

	login_to_volume "$CONTROLLER_IP:3260"
	sleep 5

	get_scsi_disk
	if [ "$device_name" == "" ]; then
		echo "error happened while running read consistency test"
		pkill -9 -P $replica1_pid
		pkill -9 -P $replica2_pid
		pkill -9 -P $replica3_pid
		return
	fi

	write_data 0 41943040 512 "/dev/$device_name" $file_name
	sync

	write_data 0 10485760 4096 "/dev/$device_name" $file_name &
	w_pid=$!
	sleep 1
	pkill -9 -P $replica1_pid
	wait $w_pid
	sync

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q  -d &
	replica1_pid=$!
	sleep 5
	write_data 13631488 10485760 4096 "/dev/$device_name" $file_name &
	w_pid=$!
	sleep 1
	pkill -9 -P $replica2_pid
	wait $w_pid
	sync

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica2_vdev -q  -d &
	replica2_pid=$!
	sleep 5
	write_data 31457280 10485760 4096 "/dev/$device_name" $file_name &
	w_pid=$!
	sleep 1
	pkill -9 -P $replica3_pid
	wait $w_pid
	sync

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica3_vdev -q -d &
	replica3_pid=$!
	sleep 5

	dd if=/dev/$device_name of=$device_file bs=4096 iflag=direct oflag=direct count=10240
	diff $device_file $file_name >> /dev/null 2>&1
	if [ $? -ne 0 ]; then
		echo "read consistency test failed"
		tail -50 $LOGFILE
		exit 1
	else
		echo "read consistency test passed"
	fi

	logout_of_volume
	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	rm -rf ${replica1_vdev}* ${replica2_vdev}* ${replica3_vdev}*
	rm -rf $file_name $device_file
	stop_istgt
}

run_lu_rf_test ()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local replica2_vdev="/tmp/test_vol2"
	local replica3_vdev="/tmp/test_vol3"
	local replica4_ip="127.0.0.1"
	local replica4_port="6164"
	local replica4_vdev="/tmp/test_vol4"

	>$LOGFILE
	sed -i -n '/LogicalUnit section/,$!p' src/istgt.conf
	setup_test_env $replica4_vdev

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q -r &
	replica1_pid=$!

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica2_vdev -q -r &
	replica2_pid=$!

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica3_vdev -q -r &
	replica3_pid=$!
	sleep 5

	git checkout src/istgt.conf
	echo "# LogicalUnit section
[LogicalUnit2]
  TargetName vol1
  TargetAlias nicknamefor-vol1
  Mapping PortalGroup1 InitiatorGroup1
  AuthMethod None
  AuthGroup None
  UseDigest Auto
  ReadOnly No
  ReplicationFactor 3
  ConsistencyFactor 2
  UnitType Disk
  UnitOnline Yes
  BlockLength 512
  QueueDepth 32
  Luworkers 6
  UnitInquiry "CloudByte" "iscsi" "0" "4059aab98f093c5d95207f7af09d1413"
  PhysRecordLength 4096
  LUN0 Storage 5G 32k
  LUN0 Option Unmap Disable
  LUN0 Option WZero Disable
  LUN0 Option ATS Disable
  LUN0 Option XCOPY Disable"	>> /usr/local/etc/istgt/istgt.conf

	$ISTGTCONTROL refresh

	sleep 5

	grep "is ready for IOs now" $LOGFILE > /dev/null 2>&1
	if [ $? -eq 0 ]; then
		echo "lun refresh passed"
	else
		echo "lun refresh failed"
		cat $LOGFILE
		exit 1
	fi

	sleep 5

	pkill -9 -P $replica3_pid
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$(($replica3_port + 10))" -V $replica3_vdev -q &
	replica3_pid=$!
        sleep 5

	## Checking whether process exist or not
	if ps -p $replica3_pid > /dev/null 2>&1
	then
		echo "Replica identification test passed"
	else
		echo "Replica identification test failed"
		cat $LOGFILE
		exit 1
	fi

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica4_ip" -P "$replica4_port" -V $replica4_vdev &
	replica4_pid=$!
	sleep 5

	if ps -p $replica4_pid > /dev/null 2>&1
	then
		echo "Non-quorum-replica connection test failed(3 quorum + 1 non-quorum)"
		cat $LOGFILE
		exit 1
	else
		echo "Non-quorum-replica connection test passed(3 quorum + 1 non-quorum)"
	fi

	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	rm -rf ${replica1_vdev}* ${replica2_vdev}* ${replica3_vdev}*
	stop_istgt
}

## wait_for_healthy_replicas will wait for max of 5 minutes for replicas to become healthy
## and verifies the count of the helathy replicas with desired count.
wait_for_healthy_replicas()
{

	no_of_replicas=$1
	local i=0

	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[].Mode' | grep -w Healthy"
	cnt=$(eval $cmd | wc -l)

	## Wait untill all the replicas are converted to healthy.
	## It takes atleast 30 seconds for each replica to become healthy
	## checking for desired healthy replica count for 5 minutes with an interval of 3 seconds.
	if [ $cnt -ne $no_of_replicas ]
	then
		for (( i = 0; i < 100; i++ )) do
			cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[].Mode' | grep -w Healthy"
			cnt=$(eval $cmd | wc -l)
			if [ $cnt -eq $no_of_replicas ]
			then
				break
			fi
			sleep 3
		done
	fi

	if [ $cnt -ne $no_of_replicas ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "Health check is failed expected healthy replica count $no_of_replicas but got $cnt"
	    	exit 1
	fi

	echo "Health state check is passed"

	check_degraded_quorum 0 $no_of_replicas
}

## waits for volume to be degraded for 5 mins
wait_for_degraded_vol()
{
	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"

	for (( i = 0; i < 100; i++ )) do
		rt=$(eval $cmd)
		if [ ${rt} == "\"Degraded\"" ]; then
			break
		fi
		sleep 3
	done

	if [ ${rt} != "\"Degraded\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume is not turned to degraded"
	    	exit 1
	fi
}

## check_degraded_quorum used to check the degraded replica count and quorum value count.
check_degraded_quorum()
{
	local expected_degraded_count=$1
	local expected_quorum_count=$2
	local cnt=0

	## Check the no.of degraded replicas
	$ISTGTCONTROL -q REPLICA vol1
	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[].Mode'"
	cnt=$(eval $cmd | grep -w Degraded | wc -l)

	if [ $cnt -ne $expected_degraded_count ]
	then
		$ISTGTCONTROL -q REPLICA vol1
		echo "Degraded replica count is not matched: expected degraded replica count $expected_degraded_count and got $cnt"
		exit 1
	fi

	## Check for quorum replicas. It takes approximately another 10 seconds to set quorum to 1 after rebuilding
	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[].quorum'"
	cnt=$(eval $cmd | grep -vw 0 | wc -l)

	if [ $cnt -ne $expected_quorum_count ]
	then
		$ISTGTCONTROL -q REPLICA vol1
		eval $cmd
		echo "Quorum test failed: expected quorum count is $expected_quorum_count and got $cnt"
		exit 1
	fi

	echo "Test passed for checking the count of Degraded and Quorum count"
}

# run_quorum_test is used to test by connecting n Quorum replicas and m Non Quorum replicas
run_quorum_test()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica4_port="6164"
	local replica5_port="6165"
	local replica6_port="6166"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica4_ip="127.0.0.1"
	local replica5_ip="127.0.0.1"
	local replica6_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"

	REPLICATION_FACTOR=4
	CONSISTENCY_FACTOR=3

	setup_test_env


	## Below Test will connect 1 QUORUM REPLICA and 5 NON-QUORUM Replica
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q &
	replica1_pid=$!
	sleep 2	#Replica will take some time to make successful connection to target

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica1_vdev &
	replica2_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica1_vdev &
	replica3_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica4_ip" -P "$replica4_port" -V $replica1_vdev &
	replica4_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica5_ip" -P "$replica5_port" -V $replica1_vdev -q &
	replica5_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica6_ip" -P "$replica6_port" -V $replica1_vdev &
	replica6_pid=$!
	sleep 1

	# Status check with (2 Quorum + 3 Non Quorum replicas) should be ofline.
	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"

	rt=$(eval $cmd)
	if [ ${rt} != "\"Offline\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be Offline, but, $rt"
		exit 1
	fi

	## Kill any one of the replica and re-connect with Quorum value as 1
	pkill -9 -P $replica2_pid
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica1_vdev -q &
	replica2_pid=$!
	sleep 2

	wait_for_degraded_vol

	## After connecting it should have 5 as Degraded and 3 quorum value
	check_degraded_quorum 5 3

	## 6th should fail to make data connect because the replica count is > MAXREPLICA
	if ps -p $replica6_pid > /dev/null 2>&1
	then
		echo "Non-quorum-replica connection test is failed(3 quorum + 3 non-quorum)"
		cat $LOGFILE
		exit 1
	else
		echo "Non-quorum-replica connection test is passed(3 quorum + 3 non-quorum)"
	fi

	rt=$(eval $cmd)
	if [ ${rt} != "\"Degraded\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be Degraded, but, $rt"
		exit 1
	fi

	# Pass the expected healthy replica count
	wait_for_healthy_replicas 4

	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	pkill -9 -P $replica4_pid
	pkill -9 -P $replica5_pid
	stop_istgt
	rm -rf ${replica1_vdev::-1}*
}

check_order_of_rebuilding()
{

	local cnt=0
	local cmd1=""
	local cmd2=""
	local done_status=0
	while [ 1 ]; do
		cnt=0
		for (( i = 0; i < 3; i++ )) do
			cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"["$i"].Mode'"
			rt=$(eval $cmd)
			if [ ${rt} == "\"Healthy\"" ]; then
				cnt=`expr $cnt + 1`
				if [ $cnt -eq 2 ]; then
					## Non quorum replicas are listed last checking with last index.
					cmd1="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[2].Mode'"
					cmd2="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[2].quorum'"
					rt1=$(eval $cmd1)
					rt2=$(eval $cmd2)
					if [ ${rt1} != "\"Degraded\"" -o ${rt2} != "\"0\"" ]; then
						echo "Order of rebuild is failed"
						exit 1
					fi
					done_status=1
					break
				fi
			fi
		done
		if [ $done_status -eq 1 ]; then
			echo "Order of rebuilding is passed"
			break
		fi
		## If 2 healthy replicas are not found sleeping for 10 seconds and retrying
		sleep 10
	done
}

##run_non_quorum_replica_errored_test is used to kill the replica while forming management connection
run_non_quorum_replica_errored_test()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local replica2_vdev="/tmp/test_vol2"
	local replica3_vdev="/tmp/test_vol3"

	REPLICATION_FACTOR=3
	CONSISTENCY_FACTOR=2

	setup_test_env

	## Below Test will connect 1 QUORUM REPLICA and 5 NON-QUORUM Replica
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q  &
	replica1_pid=$!
	sleep 2	#Replica will take some time to make successful connection to target

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica2_vdev -q  &
	replica2_pid=$!
	sleep 2

        wait_for_healthy_replicas 2

	login_to_volume "$CONTROLLER_IP:3260"
	sleep 5
	get_scsi_disk

	if [ "$device_name"!="" ]; then
		mkfs.ext4 -F /dev/$device_name
		[[ $? -ne 0 ]] && echo "mkfs failed for $device_name" && tail -20 $LOGFILE && exit 1

		mount /dev/$device_name /mnt/store
		[[ $? -ne 0 ]] && echo "mount for $device_name" && tail -20 $LOGFILE && exit 1
	fi

	date_log=$(eval date +%Y-%m-%d/%H:%M:%S.%s)

	# As long as we are not running any IOs we can use the same vdev file
	## Making delay in management connection
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica3_vdev -s 10 &
	replica3_pid=$!
	sleep 5

	## Introduced delay in management connection and killing the replica during mgmt_connection
	kill_non_quorum_replica mgmt_connection

	umount /mnt/store
	logout_of_volume
	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	stop_istgt
	rm -f /tmp/check_sum*
	rm -rf ${replica1_vdev::-1}*

}

## kill_non_quorum_replica used to kill the replica during connections
kill_non_quorum_replica()
{
	state=$1
	local val=0
	if [ $state == "data_connection" ]
	then
		cmd="grep 'replica($2) connected successfully' $LOGFILE | awk -v date_logs=\"$date_log\" '\$0 > date_logs'"
		val=$(eval $cmd |wc -l)
		if [ $val -eq 0 ]
		then
			echo "Management connection is not formed"
			exit 1
		fi
	elif [ $state == "mgmt_connection" ]
	then
		if ps -p $replica3_pid > /dev/null 2>&1
		then
			echo "Breaking mgmt connection"
		else
			echo "Replica handshake is not initiated"
			cat $LOGFILE
			exit 1
		fi
	else
		echo "Pass valid arguments"
		return
	fi

	pkill -9 -P $replica3_pid
	if [ $? -ne 0 ]
	then
		echo "No Non-quorum replica present to disconnect"
	fi
	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[].replicaId'"
	rt=$(eval $cmd | wc -l)
	if [ ${rt} -ne 2 ]
	then
		echo "Error occured after killing the replica during $state process"
		exit 1
	fi

	ps -aux | grep 'istgt'
	dd if=/dev/urandom of=/mnt/store/file2 bs=4k count=100
	if [ $? -ne 0 ]
	then
		echo "Error occured after killing the non-quorum and while performing writes"
		exit 1
	fi
	echo "Erroring out replica done successfully"
}

data_integrity_with_non_quorum()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local replica2_vdev="/tmp/test_vol2"
	local replica3_vdev="/tmp/test_vol3"

	REPLICATION_FACTOR=3
	CONSISTENCY_FACTOR=2

	setup_test_env

	## Below Test will connect 1 QUORUM REPLICA and 5 NON-QUORUM Replica
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q &
	replica1_pid=$!
	sleep 2	#Replica will take some time to make successful connection to target

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica3_vdev &
	replica3_pid=$!
	sleep 2

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica2_vdev -q &
	replica2_pid=$!
	sleep 2

	login_to_volume "$CONTROLLER_IP:3260"
	sleep 10
	get_scsi_disk



	## Cross check non-quorum replica shuould be in degraded state
	## Index 2 is used because non quorum replicas are listed last
	cmd1="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[2].Mode'"
	cmd2="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[2].quorum'"
	rt1=$(eval $cmd1)
	rt2=$(eval $cmd2)
	if [ ${rt1} != "\"Degraded\"" -o ${rt2} != "\"0\"" ]; then
		echo "Non quorum replica Should be in degraded mode and quorum value must be 0, but $rt1"
		exit 1
	fi

	if [ "$device_name"!="" ]; then
		sudo dd if=/dev/urandom of=$device_name bs=4k count=1000 oflag=direct
                var1="$($ISTGTCONTROL -q iostats | jq '.TotalWriteBytes')"
		sleep 5

		hash1=$(md5sum $replica1_vdev | awk '{print $1}')
		hash2=$(md5sum $replica3_vdev | awk '{print $1}')
		if [ $hash1 == $hash2 ]; then echo "DI Test: PASSED"
		else
			echo "DI Test: FAILED Hash of quorum is $hash1 and non-quorum is $hash2 with writebytes $var1";
			tail -20 $LOGFILE
			exit 1
		fi
	fi

	check_order_of_rebuilding

	## Checking the read IO's with quorum and non-quorum replicas
	sudo dd if=$device_name of=/dev/null bs=4k count=100
	if ps -p $replica3_pid > /dev/null 2>&1
	then
		echo "Passed the read IO test on non-quorum"
	else
		echo "Read Test is failed"
		sleep 10
		exit 1
	fi

	pkill -9 -P $replica3_pid

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica3_vdev &
	replica3_pid=$!
	date_log=$(eval date +%Y-%m-%d/%H:%M:%S.%s)

	$ISTGTCONTROL -q replica | jq
	wait_for_healthy_replicas 3

	logout_of_volume
	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	stop_istgt
	rm -f /tmp/check_sum*
	rm -rf ${replica1_vdev::-1}*

}
run_rebuild_time_test_in_single_replica()
{
	local replica1_port="6161"
	local replica1_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local ret=0

	echo "run rebuild time test in single replica"
	REPLICATION_FACTOR=1
	CONSISTENCY_FACTOR=1
	setup_test_env

	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"
	rt=$(eval $cmd)
	if [ ${rt} != "\"Offline\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be 1"
		exit 1
	fi

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q &
	replica1_pid=$!
	sleep 2

	rt=$(eval $cmd)
	if [ ${rt} != "\"Degraded\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be 2"
		exit 1
	fi

	while [ 1 ]; do
		# With replica poll timeout as 10, volume should become
		# healthy in less than 40 seconds.
		cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[0].\"upTime\"'"
		rt=$(eval $cmd)
		echo "replica start time $rt"
		if [ $rt -gt 20 ]; then
			cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"[0].Mode'"
			rstatus=$(eval $cmd)
			echo "replica status $rstatus"
			if [ ${rstatus} != "\"Healthy\"" ]; then
				echo "replication factor(1) test failed"
				exit 1
			else
				cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"
				rt=$(eval $cmd)
				if [ ${rt} != "\"Healthy\"" ]; then
					$ISTGTCONTROL -q REPLICA vol1
					echo "volume status is supposed to be 4"
					exit 1
				elif [ ${rt} == "\"Healthy\"" ]; then
					break
				else
					echo "volume status is not proper"
					exit 1
				fi
			fi
		elif [ $rt -le 20 ]; then
			sleep 10
		else
			echo "replication factor(4) test failed"
			exit 1
		fi
	done
	pkill -9 -P $replica1_pid
	stop_istgt
	rm -rf ${replica1_vdev::-1}*
}

run_rebuild_time_test_in_multiple_replicas()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica4_port="6164"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica4_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local ret=0
	local cnt=0
	local rt=0
	local done_test=0

	echo "run rebuild time test in multiple replica, param1: $1"
	REPLICATION_FACTOR=3
	CONSISTENCY_FACTOR=2
	setup_test_env

	cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"
	rt=$(eval $cmd)
	if [ ${rt} != "\"Offline\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be 1"
		exit 1
	fi

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q &
	replica1_pid=$!
	sleep 2

	rt=$(eval $cmd)
	if [ ${rt} != "\"Offline\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be 1"
		exit 1
	fi

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica1_vdev -q &
	replica2_pid=$!
	sleep 2

	rt=$(eval $cmd)
	if [ ${rt} != "\"Degraded\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be 2"
		exit 1
	fi

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica1_vdev -q  &
	replica3_pid=$!
	sleep 3

	rt=$(eval $cmd)
	if [ ${rt} != "\"Degraded\"" ]; then
		$ISTGTCONTROL -q REPLICA vol1
		echo "volume status is supposed to be 2"
		exit 1
	fi

	done_test=0
	while [ 1 ]; do
		# We have started istgt with 20 second replica timeout
		# and rf=2, cf=3. so, it will take around 2 minutes for the
		# replica to become healthy. So on safe side, we will
		# check replica status after 130 seconds.
		for (( i = 0; i < 3; i++ )) do
			cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"["$i"].\"upTime\"'"
			rt=$(eval $cmd)
			echo "replica start time $rt"
			if [ $1 -eq 0 ]; then
				if [ $rt -gt 40 ]; then
					cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"["$i"].Mode'"
					rstatus=$(eval $cmd)
					echo "replica status $rstatus"
					if [ ${rstatus} == "\"Healthy\"" ]; then
						cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"
						rstatus=$(eval $cmd)
						if [ ${rstatus} != "\"Degraded\"" ]; then
							$ISTGTCONTROL -q REPLICA vol1
							echo "volume status is supposed to be 2"
							exit 1
						fi
						done_test=1
						break
					fi
				elif [ $rt -le 40 ]; then
					: # This kind of checks are required when eval cmd fails
				else
					echo "replication factor(5) test failed"
					exit 1
				fi
			else
				if [ $rt -le 30 ]; then
					cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"["$i"].Mode'"
					rstatus=$(eval $cmd)
					echo "replica status $rstatus"
					if [ ${rstatus} == "\"Healthy\"" ]; then
						echo "replication factor(3) test failed"
						exit 1
					fi
				elif [ $rt -gt 30 ]; then
					done_test=1
					break
				else
					echo "replication factor(6) test failed"
					exit 1
				fi
			fi
		done
		if [ $1 -eq 0 ]; then
			if [ $rt -gt 40 ]; then
				echo "replica start time $rt done_test: $done_test"
				if [ $done_test -eq 0 ]; then
					echo "replication factor(2) test failed"
					exit 1
				fi
			fi
		fi
		if [ $done_test -eq 1 ]; then
			break
		fi
		sleep 10
	done
	while [ 1 ]; do
		if [ $1 -eq 1 ]; then
			break
		fi
		cnt=0
		for (( i = 0; i < 3; i++ )) do
			cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].\"replicaStatus\"["$i"].Mode'"
			rt=$(eval $cmd)
			if [ ${rt} == "\"Healthy\"" ]; then
				cnt=`expr $cnt + 1`
			fi
		done
		if [ ${cnt} -ge 2 ]; then
			cmd="$ISTGTCONTROL -q REPLICA vol1 | jq '.\"volumeStatus\"[0].status'"
			rstatus=$(eval $cmd)
			if [ ${rstatus} != "\"Healthy\"" ]; then
				$ISTGTCONTROL -q REPLICA vol1
				echo "volume status is supposed to be 3"
				exit 1
			elif [ ${rstatus} == "\"Healthy\"" ]; then
				break
			else
				echo "replication factor(7) test failed"
				exit 1
			fi
		fi
		sleep 2
	done

	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	stop_istgt
	rm -rf ${replica1_vdev::-1}*
}

run_replication_factor_test()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica4_port="6164"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica4_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local ret=0

	run_rebuild_time_test_in_single_replica
	run_rebuild_time_test_in_multiple_replicas 0

	export non_zero_inflight_replica_cnt=1
	run_rebuild_time_test_in_multiple_replicas 1

	export non_zero_inflight_replica_cnt=0
	sleep 1
	REPLICATION_FACTOR=3
	CONSISTENCY_FACTOR=2

	setup_test_env

	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q  &
	replica1_pid=$!
	sleep 2	#Replica will take some time to make successful connection to target

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica1_vdev -q  &
	replica2_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica1_vdev -q  &
	replica3_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	start_replica -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica4_ip" -P "$replica4_port" -V $replica1_vdev -q &
	replica4_pid=$!
	sleep 5

	wait $replica4_pid
	if [ $? == 0 ]; then
		echo "replica limit test failed"
		pkill -9 -P $replica4_pid
		ret=1
	else
		echo "replica limit test passed"
	fi

	pkill -9 -P $replica1_pid
	pkill -9 -P $replica2_pid
	pkill -9 -P $replica3_pid
	stop_istgt
	rm -rf ${replica1_vdev::-1}*

	if [ $ret == 1 ]; then
		exit 1
	fi
}

run_test_env()
{
	REPLICATION_FACTOR=3
	CONSISTENCY_FACTOR=2
	TEST_ENV=1
	setup_test_env

	cnt=$(eval $ISTGTCONTROL dump -a | grep "Luworkers:9" | wc -l)
	if [ $? -ne 0 ] || [ $cnt -ne 1 ]
	then
		echo "test env failed"
		exit 1
	fi
	cleanup_test_env

	unset TEST_ENV
	setup_test_env

	cnt=$(eval $ISTGTCONTROL dump -a | grep "Luworkers:6" | wc -l)
	if [ $? -ne 0 ] || [ $cnt -ne 1 ]
	then
		echo "test env failed"
		exit 1
	fi
	cleanup_test_env
}


run_io_timeout_test()
{
	local replica1_port="6161"
	local replica2_port="6162"
	local replica3_port="6163"
	local replica1_ip="127.0.0.1"
	local replica2_ip="127.0.0.1"
	local replica3_ip="127.0.0.1"
	local replica1_vdev="/tmp/test_vol1"
	local injected_latency=10

	REPLICATION_FACTOR=3
	CONSISTENCY_FACTOR=2

	setup_test_env

	$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -q  &
	replica1_pid=$!
	sleep 2	#Replica will take some time to make successful connection to target

	# As long as we are not running any IOs we can use the same vdev file
	$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica1_vdev -q  &
	replica2_pid=$!
	sleep 2

	# As long as we are not running any IOs we can use the same vdev file
	$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica1_vdev -q  &
	replica3_pid=$!
	sleep 2

	login_to_volume "$CONTROLLER_IP:3260"
	get_scsi_disk
	if [ "$device_name"!="" ]; then
		# Test to verify impact of replica's delay on volume latency
		sudo kill -9 $replica1_pid
		sleep 2

		$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -t $injected_latency -q &
		replica1_pid=$!
		sleep 2 #Replica will take some time to make successful connection to target

		ps -aux | grep istgt
		iopinglog=`mktemp`
		$IOPING -c 1 -B -WWW /dev/$device_name > $iopinglog
		if [ $? -ne 0 ]; then
			exit 1
		fi
		latency=`awk -F ' '  '{print $6}' $iopinglog`
		# ioping gives latency in usec for raw output
		latency=$(( $latency / 1000000 ))
		echo "got latency $latency"
		cat $iopinglog
		if [ $latency -lt $injected_latency ]; then
			echo "Injected latency is $injected_latency seconds, but got $latency usec latency"
			exit 1
		fi

		# Test to verify disconnection of replica if delay from replica is more than maxiowait
		$ISTGTCONTROL maxiowait 5
		$IOPING  -c 1 -B -WWW /dev/$device_name > $iopinglog
		wait $replica1_pid
		if [ $? -eq 0 ]; then
			echo "IO timeout test failed"
			exit 1
		fi

		$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica1_ip" -P "$replica1_port" -V $replica1_vdev -t $injected_latency -q &
		replica1_pid=$!
		sleep 2

		kill -9 $replica2_pid
		$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica2_ip" -P "$replica2_port" -V $replica1_vdev  -t $injected_latency -q &
		replica2_pid=$!
		sleep 2

		kill -9 $replica3_pid
		$REPLICATION_TEST -i "$CONTROLLER_IP" -p "$CONTROLLER_PORT" -I "$replica3_ip" -P "$replica3_port" -V $replica1_vdev -t $injected_latency -q &
		replica3_pid=$!
		sleep 2

		# Test to verify volume status if all replica have delay more than maxiowait
		$IOPING  -c 1 -B -WWW /dev/$device_name > $iopinglog
		if [ $? -eq 0 ]; then
			echo "IO timeout test failed"
			exit 1
		fi

	else
		exit 1
	fi

	kill -9  $replica1_pid
	kill -9  $replica2_pid
	kill -9  $replica3_pid
	stop_istgt
	rm -rf ${replica1_vdev::-1}*
}

run_lu_rf_test
run_quorum_test
data_integrity_with_non_quorum
run_non_quorum_replica_errored_test
run_data_integrity_test
run_mempool_test
run_istgt_integration
run_read_consistency_test
run_replication_factor_test
run_io_timeout_test
run_test_env
echo "===============All Tests are passed ==============="
tail -20 $LOGFILE

exit 0
