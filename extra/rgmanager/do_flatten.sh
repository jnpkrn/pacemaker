#!/bin/bash
./makelocal.sh
for i in tests/test*.conf; do
	echo "${i}..."
	./ccs_flatten $i    > $(echo $i | sed "s/\([^.]*\).conf/\1.conf.flat/g")
	./ccs_flatten $i -r > $(echo $i | sed "s/\([^.]*\).conf/\1.conf.flatref/g")
done
