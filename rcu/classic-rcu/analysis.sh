#!/usr/bin/env bash

make all
for i in {1..10}; do
    make lrcu
    sleep 1
done
