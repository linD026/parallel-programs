#!/usr/bin/bash

read -p "perf /trace time [p/t]: " OPT
ls
read -p "which directory : " DIR

if [ $OPT == 'p' ]  || [ $OPT == 'perf' ]
then 
    PERF_COM='sudo perf stat --repeat 1000 -e cache-misses,cache-references,instructions,cycles'
elif [ $OPT == 't' ] || [ $OPT  == 'trace time' ]
then
	PERF_COM=''
	read -p "make " BENCHMARK
	make -C $DIR $BENCHMARK CONFIG_TRACE_TIME=y 
else
	PERF_COM=''
fi

$PERF_COM ./$DIR/test
