[![Build Status](http://jenkins.teamblueridge.com/job/msm7x30-kernel-primoc/badge/icon)](http://jenkins.teamblueridge.com/job/msm7x30-kernel-primoc/)
#LINUX KERNEL
This builds the Linux for MSM7x30 devices. In this scenario, specifically for the HTC One V (primou/c).

#Code Review
Team BlueRidge uses Gerrit Code Review for changes to this repository. Pull requests are not accepted. To push to this repository clone the repository and create an acccount on [Team BlueRidge Gerrit](http://gerrit.teamblueridge.com). Then execute './gerrit.sh' in the repository directory.
Project Name: 'MSM7X30_KERNEL'
Branch: sense-ics/ics/jellybean (choose one)

#Toolchain
You need the proper toolchain from AOSP to build this kernel. You can get it using 'git clone https://android.googlesource.com/platform/prebuilts/tools

#Building
The recommended way to build this repository is to use a script like the following.
    export PATH=<path to toolchain repo>/linux-x86/toolchain/arm-eabi-4.4.3/bin:$PATH
    make ARCH=arm <device>_defconfig
    make ARCH=arm CROSS_COMPILE=arm-eabi-

#Support
Builders may contact kalaker or simonsimons34 for help.
