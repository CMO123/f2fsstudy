#!/bin/bash
switch=br0
if [ -n "$1" ];then
	ip link set $1 up
	sleep 1
	brctl addif ${switch} $1
	exit 0
else
	echo "error"
	exit 1
fi
