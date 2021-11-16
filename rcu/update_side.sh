#!/usr/bin/env bash

make -C locked-rcu update
make -C thrd-based-rcu update
./locked-rcu/test
echo "-------------------------"
./thrd-based-rcu/test
