#!/usr/bin/bash

make -C locked-rcu read
make -C thrd-based-rcu read
./locked-rcu/test
echo "-------------------------"
./thrd-based-rcu/test
