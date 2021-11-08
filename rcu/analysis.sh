#!/usr/bin/bash

YES="Y"
yes="y"

read -p "perf      [Y/N]: " PERF
ls
read -p "which directory: " DIR

if [ $PERF == $YES ]  || [ $PERF == $yes ]
then 
    PERF_COM='sudo perf stat --repeat 1000 -e cache-misses,cache-references,instructions,cycles'
else
    PERF_COM=''
fi

$PERF_COM ./$DIR/test
