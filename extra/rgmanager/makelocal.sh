#!/bin/bash
RA_PATH=$(pwd)/agents

make CFLAGS=-DSHAREDIR="'\"$RA_PATH\"'"
