#!/bin/bash

SO2_WORKSPACE=/linux/tools/labs
SO2_VM_LOG=/tmp/so2_vm_log.txt
DMESG_LOG=""

ASSIGNMENT0_TIMEOUT=300 # 5 min
ASSIGNMENT0_MOD=list.ko
ASSIGNMENT0_DIR=${SO2_WORKSPACE}/skels/assignments/0-list
ASSIGNMENT0_CHECKER_LOCAL_DIR=checker/0-list-checker
ASSIGNMENT0_CHECKER_DIR=${SO2_WORKSPACE}/skels/assignments/0-list-checker
ASSIGNMENT0_OUTPUT=${SO2_WORKSPACE}/skels/0-list-output
ASSIGNMENT0_FINISHED=${SO2_WORKSPACE}/skels/0-list-finished

ASSIGNMENT1_TIMEOUT=300 # 5 min
ASSIGNMENT1_MOD=tracer.ko
ASSIGNMENT1_DIR=${SO2_WORKSPACE}/skels/assignments/1-tracer
ASSIGNMENT1_CHECKER_LOCAL_DIR=checker/1-tracer-checker
ASSIGNMENT1_CHECKER_DIR=${SO2_WORKSPACE}/skels/assignments/1-tracer-checker
ASSIGNMENT1_OUTPUT=${SO2_WORKSPACE}/skels/1-tracer-output
ASSIGNMENT1_FINISHED=${SO2_WORKSPACE}/skels/1-tracer-finished
ASSIGNMENT1_HEADER_OVERWRITE=${SO2_WORKSPACE}/templates/assignments/1-tracer/tracer.h
ASSIGNMENT1_CHECKER_AUX_LIST="${ASSIGNMENT1_CHECKER_DIR}/_helper/tracer_helper.ko"

ASSIGNMENT2_TIMEOUT=300 # 5 min
ASSIGNMENT2_MOD=uart16550.ko
ASSIGNMENT2_DIR=${SO2_WORKSPACE}/skels/assignments/2-uart
ASSIGNMENT2_CHECKER_LOCAL_DIR=checker/2-uart-checker
ASSIGNMENT2_CHECKER_DIR=${SO2_WORKSPACE}/skels/assignments/2-uart-checker
ASSIGNMENT2_OUTPUT=${SO2_WORKSPACE}/skels/2-uart-output
ASSIGNMENT2_FINISHED=${SO2_WORKSPACE}/skels/2-uart-finished
ASSIGNMENT2_HEADER_OVERWRITE=${SO2_WORKSPACE}/templates/assignments/2-uart/uart16550.h
ASSIGNMENT2_CHECKER_AUX_LIST="${ASSIGNMENT2_CHECKER_DIR}/_test/solution.ko"

ASSIGNMENT3_TIMEOUT=360 # 6 min
ASSIGNMENT3_MOD=ssr.ko
ASSIGNMENT3_DIR=${SO2_WORKSPACE}/skels/assignments/3-raid
ASSIGNMENT3_CHECKER_LOCAL_DIR=checker/3-raid-checker
ASSIGNMENT3_CHECKER_DIR=${SO2_WORKSPACE}/skels/assignments/3-raid-checker
ASSIGNMENT3_OUTPUT=${SO2_WORKSPACE}/skels/3-raid-output
ASSIGNMENT3_FINISHED=${SO2_WORKSPACE}/skels/3-raid-finished
ASSIGNMENT3_HEADER_OVERWRITE=${SO2_WORKSPACE}/templates/assignments/3-raid/ssr.h
ASSIGNMENT3_CHECKER_AUX_LIST="${ASSIGNMENT3_CHECKER_DIR}/_test/run-test"

ASSIGNMENT4_TIMEOUT=300 # 5 min
ASSIGNMENT4_MOD=af_stp.ko
ASSIGNMENT4_DIR=${SO2_WORKSPACE}/skels/assignments/4-stp
ASSIGNMENT4_CHECKER_LOCAL_DIR=checker/4-stp-checker
ASSIGNMENT4_CHECKER_DIR=${SO2_WORKSPACE}/skels/assignments/4-stp-checker
ASSIGNMENT4_OUTPUT=${SO2_WORKSPACE}/skels/4-stp-output
ASSIGNMENT4_FINISHED=${SO2_WORKSPACE}/skels/4-stp-finished
ASSIGNMENT4_HEADER_OVERWRITE=${SO2_WORKSPACE}/templates/assignments/4-stp/stp.h
#ASSIGNMENT4_CHECKER_AUX_LIST="${ASSIGNMENT3_CHECKER_DIR}/_test/run-test"


usage()
{
	echo "Usage: $0 <assignment>"
	exit 1
}


configure_logger()
{
	DMESG_LOG="/linux/tools/labs/skels/log.txt"
	cp ./checker/checker_daemons/so2_vm_checker_logger.sh /linux/tools/labs/rootfs/etc/init.d
	chmod +x /linux/tools/labs/rootfs/etc/init.d/so2_vm_checker_logger.sh
	chroot /linux/tools/labs/rootfs update-rc.d so2_vm_checker_logger.sh defaults 0 0
}

recover_grade_from_timeout()
{
	local output=$1
	if [ ! -f $output ]; then
		echo "$output not available"
	else
		points_total=$(echo $(cat $output | grep "....passed" | egrep -o "/.*[0-9]+\.*[0-9]*.*\]" | egrep -o "[0-9]+\.*[0-9]*" | head -n 1))
		list=$(echo $(cat $output | grep "....passed" | egrep  -o "\[.*[0-9]+\.*[0-9]*.*\/" |  egrep -o "[0-9]+\.*[0-9]*") | sed -e 's/\s\+/,/g')
		recovered_points=$(python3 -c "print(sum([$list]))")
		echo "Recovered from timeout => Total: [$recovered_points/$points_total]"
		echo "Please note that this is not a DIRECT checker output! Other penalties may be applied!"
		echo "Please contact a teaching assistant"
		python3 -c "print('Total: ' + str(int ($recovered_points * 100 / $points_total)) + '/' + '100')"
	fi
}

dmesg_log_dump()
{
	if [[ $DMESG_LOG != "" ]]; then
		echo "dumping DMESG_LOG=${DMESG_LOG} output"
		echo ">>>>---------------DMESG_LOG_STARTS_HERE------------------<<<<<"
		cat $DMESG_LOG
		echo ">>>>----------------DMESG_LOG_ENDS_HERE-------------------<<<<<"
	fi
}

timeout_exceeded()
{
	local output=$1
	pkill -SIGKILL qemu
	echo ""
	echo "TIMEOUT EXCEEDED !!! killing the process"

	dmesg_log_dump

	if [[ $RECOVER_GRADE_TIMEOUT == 0 ]]; then
		if [ -f $output ]; then
			echo "$output not available"
		else
			cat $output
		fi

		echo "The Recover Grade Timeout option is not set! Please contact a teaching assistant!"
	else
		recover_grade_from_timeout $output
	fi
	echo "<VMCK_NEXT_END>"
	# exit successfully for vmchecker-next to process output
        exit 0 # TODO: fixme
}

compute_total()
{

	local output=$1
	points=$(cat $output | egrep "Total:" | egrep "\ *([0-9]+)" -o  | head -n 1)
	points_total=$(cat $output | egrep "Total:" | egrep "\ *([0-9]+)" -o  | tail -n 1)
	if [[ $points != "" ]] && [[ $points_total != "" ]]; then
		python3 -c "print('Total: ' + str(int ($points * 100 / $points_total)) + '/' + '100')"
		echo "<VMCK_NEXT_END>"
	fi
}

dump_output()
{
	local output=$1
	local timeout=$2
	echo "<VMCK_NEXT_BEGIN>"
	cat $output
	echo "Running time $timeout/$TIMEOUT"

}

error_message()
{
	local output=$1
	echo "<VMCK_NEXT_BEGIN>"
	echo "Cannot find $assignment_mod"
	echo -e "\t-Make sure you have the sources directly in the root of the archive."
	echo -e "\t-Make sure you have not changed the header that comes with the code skeleton."
	echo -e "\t-Make sure the assignment compiles in a similar environment as vmchecker-next by running './local.sh checker <assignment-name>'."
	echo "After you have solved the problems, resubmit the assignment on moodle until the score appears as feedback, otherwise, the assignment will not be graded."
	echo "<VMCK_NEXT_END>"
}

run_checker()
{
	local assignment_mod=$1
	local assignment_dir=$2
	local local_checker_dir=$3
	local checker_dir=$4
	local output=$5
	local finished=$6
	local assignment=$7
	local header_overwrite=$8
	local aux_modules=$9

	local module_path="${assignment_dir}/${assignment_mod}"

	echo "Copying the contents of src/ into $assignment_dir"
	cp src/* $assignment_dir

	echo "Copying the contents of $local_checker_dir into $checker_dir"
	cp -r $local_checker_dir/* $checker_dir

	echo "Checking if $assignment_mod exists before build"
	if [ -f $module_path ]; then
			echo "$assignment_mod shouldn't exists. Removing ${module_path}"
			rm $module_path
	fi

	pushd $assignment_dir &> /dev/null
		echo "Cleaning $assignment_dir => Will remove: *.o *.mod *.mod.c .*.cmd *.ko modules.order"
		rm *.o &> /dev/null
		rm *.mod &> /dev/null
		rm *.mod.c &> /dev/null
		rm .*.cmd &> /dev/null
		rm *.ko &> /dev/null
		rm modules.order &> /dev/null

		if [[ $header_overwrite != "" ]]; then
			echo "Overwrite from $header_overwrite"
			cp $header_overwrite  .
		fi
	popd &> /dev/null

		
	pushd $SO2_WORKSPACE &> /dev/null
		if [ -f $output ]; then
			echo "Removing $output"
			rm $output &> /dev/null
		fi
		if [ -f $finished ]; then
			echo "Removing $finished"
			rm $finished &> /dev/null
		fi

		echo "Building..."
		make build

		if [ ! -f $module_path ]; then
			error_message $assignment_mod
			# exit successfully for vmchecker-next to process output
			exit 0 # TODO: fixme
		fi
	
		# copy *.ko in checker
		echo "Copying $module_path into $checker_dir"
		cp $module_path $checker_dir
		
		# copy aux modules in checker
		if [[ $aux_modules != "" ]]; then
			for mod in $aux_modules
			do
				echo "Copying $mod in $checker_dir"
				cp $mod $checker_dir
			done
		fi

		LINUX_ADD_CMDLINE="so2=$assignment" make checker &> ${SO2_VM_LOG} &

		timeout=0
		echo -n "CHECKER IS RUNNING"
		while [ ! -f $finished ]
		do
			if ((timeout >= TIMEOUT)); then
				if [ -f $output ]; then
					echo ""
					dump_output $output $timeout
					compute_total $output
				fi
				timeout_exceeded $output
			fi
			sleep 2
			(( timeout += 2 ))
			echo -n .
		done
		echo ""
		dump_output $output $timeout
		compute_total $output
	popd &> /dev/null
}



case $1 in
	0-list)
		TIMEOUT=$ASSIGNMENT0_TIMEOUT
		RECOVER_GRADE_TIMEOUT=0 # If set to 1, in case of a timeout, will calculate the total grade based on the output directory
		run_checker $ASSIGNMENT0_MOD $ASSIGNMENT0_DIR $ASSIGNMENT0_CHECKER_LOCAL_DIR $ASSIGNMENT0_CHECKER_DIR $ASSIGNMENT0_OUTPUT $ASSIGNMENT0_FINISHED $1
		;;
	1-tracer)
		TIMEOUT=$ASSIGNMENT1_TIMEOUT
		RECOVER_GRADE_TIMEOUT=0 # If set to 1, in case of a timeout, will calculate the total grade based on the output directory
		run_checker $ASSIGNMENT1_MOD $ASSIGNMENT1_DIR $ASSIGNMENT1_CHECKER_LOCAL_DIR $ASSIGNMENT1_CHECKER_DIR $ASSIGNMENT1_OUTPUT $ASSIGNMENT1_FINISHED $1 $ASSIGNMENT1_HEADER_OVERWRITE $ASSIGNMENT1_CHECKER_AUX_LIST
		;;
	2-uart)
		TIMEOUT=$ASSIGNMENT2_TIMEOUT
		RECOVER_GRADE_TIMEOUT=1 # If set to 1, in case of a timeout, will calculate the total grade based on the output directory
		configure_logger
		run_checker $ASSIGNMENT2_MOD $ASSIGNMENT2_DIR $ASSIGNMENT2_CHECKER_LOCAL_DIR $ASSIGNMENT2_CHECKER_DIR $ASSIGNMENT2_OUTPUT $ASSIGNMENT2_FINISHED $1 $ASSIGNMENT2_HEADER_OVERWRITE $ASSIGNMENT2_CHECKER_AUX_LIST
 		;;
	3-raid)
		TIMEOUT=$ASSIGNMENT3_TIMEOUT
		RECOVER_GRADE_TIMEOUT=0 # If set to 1, in case of a timeout, will calculate the total grade based on the output directory
		run_checker $ASSIGNMENT3_MOD $ASSIGNMENT3_DIR $ASSIGNMENT3_CHECKER_LOCAL_DIR $ASSIGNMENT3_CHECKER_DIR $ASSIGNMENT3_OUTPUT $ASSIGNMENT3_FINISHED $1 $ASSIGNMENT3_HEADER_OVERWRITE $ASSIGNMENT3_CHECKER_AUX_LIST
		;;
	4-stp)
		TIMEOUT=$ASSIGNMENT4_TIMEOUT
		RECOVER_GRADE_TIMEOUT=0 # If set to 1, in case of a timeout, will calculate the total grade based on the output file
		run_checker $ASSIGNMENT4_MOD $ASSIGNMENT4_DIR $ASSIGNMENT4_CHECKER_LOCAL_DIR $ASSIGNMENT4_CHECKER_DIR $ASSIGNMENT4_OUTPUT $ASSIGNMENT4_FINISHED $1 $ASSIGNMENT4_HEADER_OVERWRITE
		;;
	
	*)
		usage
		;;
esac
