#!/bin/sh

# THIS SCRIPT RUNS INSIDE THE SO2 VM

LOG_FILE=/home/root/skels/log.txt

start()
{
	set -x
	echo "" > ${LOG_FILE}
	while true
	do
		sleep 1
		dmesg -c >> $LOG_FILE
	done
}

# Carry out specific functions when asked to by the system
case "$1" in
        start)
                echo "Starting so2_vm_checker_logger.sh..."
                start & # start in background
                ;;
        *)
                echo "Usage: /etc/init.d/foo {start|stop}"
                exit 1
                ;;
        esac

exit 0

