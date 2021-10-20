#!/bin/bash
if [ $# -ne 1 ]
then
	echo "you need to input one parameter which means the CPU core number of Light stack"
	exit 1
fi
if [ $1 -lt 1 -o $1 -gt 6 ]
then 
	echo "the CPU core number of Light stack must be in 1 ~ 16, inclusive"
	exit 1
fi

echo "$1 stacks will run in $1 cores"
echo "Please make sure the First Core is running!"
echo "Start Second Core... ..."

log_level=2
export LD_LIBRARY_PATH=/usr/lib/light
echo 0 >/proc/sys/kernel/randomize_va_space

# nohup /usr/bin/light_srv -c 1 -n 4 -d librte_pmd_ixgbe.so  --proc-type=auto  -- -p 0x3 -l $log_level --num-procs=$1 --proc-id=0 1>/dev/null 2>&1 &

#run command below if your NIC is e1000
#/usr/bin/light_srv -c 1 -n 4 -d librte_pmd_e1000.so  --proc-type=auto  -- -p 0x3 --num-procs=2 --proc-id=0
if [ $1 == 1 ]; then exit 0; fi;
# echo "start sleep 10s for primary process finish init work"
# sleep 10
# echo "sleep end"
for proc_id in $( seq 1 $[$1-1])
do
	temp=$[1<<$proc_id]
	mask=`echo "ibase=10;obase=16;$temp"|bc`
	echo "start to run $proc_id process, its coremask=$mask"
	/usr/bin/light_srv -c $mask -n 4 -d librte_pmd_ixgbe.so  --proc-type=auto  -- -p 0x3 -l $log_level --num-procs=$1 --proc-id=$proc_id
done
# echo "bingo"
