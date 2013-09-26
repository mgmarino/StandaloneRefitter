#!/bin/bash
#PBS -q regular
#PBS -l mppwidth=24
#PBS -l walltime=00:40:00
#PBS -N Refitter
#PBS -j oe

export CRAY_ROOTFS=DSL
cd $SCRATCH
cp $HOME/edison/StandaloneRefitter/Refitter .
aprun -n 1 -d 12 -ss -cc 0-11 ./Refitter proc00004544-000.root run00004544-000.root Denoised0.root 0 1000 : \
      -n 1 -d 12 -ss -cc 12-23 ./Refitter proc00004544-000.root run00004544-000.root Denoised1.root 1000 1000
