#!/bin/sh
# print decompiled function by address from exe.c (or $2 file)
f=${2:-exe.c}
awk -v a="@ $1 " 'index($0,"// ==== ")==1{p=(index($0,a)>0)} p' $f
