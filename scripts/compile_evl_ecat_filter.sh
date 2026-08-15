#!/bin/bash

# Convert the eBPF filter source code to an object file using clang.
# The object file can then be used by the Xenomai EVL framework to allow only EtherCAT frames to be processed by the application, filtering out other traffic.
clang -O2 -target bpf -I/usr/include/$(uname -m)-linux-gnu -c oshw/linux/evl_ecat_filter.c -o oshw/linux/evl_ecat_filter.o

# In case we want to observe the bytecode in a C header file, we can use xxd to convert the object file to a C array.
# xxd -i oshw/linux/evl_ecat_filter.o > oshw/linux/filter_bytecode.h