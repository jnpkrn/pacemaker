#!/bin/bash
make ccs2cib
for i in tests/test*.conf; do
	echo "${i}..."
	./ccs2cib -d -f -n -i $i -o $(echo $i | sed "s/\([^.]*\).conf/\1.conf.conv/g")
done
