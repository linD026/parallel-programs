#!/usr/bin/env bash

make all
for i in {1..100};
do
    make rcu
    sleep 1 
done
