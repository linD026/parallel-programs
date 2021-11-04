#!/usr/bin/bash

# the thrd-based-rcu read cannot run with more or equal the 20 threads
make -C locked-rcu read
make -C thrd-based-rcu read
./locked-rcu/test
echo "-------------------------"
./thrd-based-rcu/test
