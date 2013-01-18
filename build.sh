#!/bin/bash -x
CYANOGENMOD=../../..
# Starting Timer
START=$(date +%s)

# Make mrproper
make mrproper

# Set config
make M712HC_defconfig
make menuconfig

# Make modules
nice -n 10 make -j16 modules

# Copy modules
find -name '*.ko' -exec cp -av {} $CYANOGENMOD/device/rockchip/rk2918/modules/ \;
find -name '*.ko' -exec cp -av {} installer/system/lib/modules/ \;
# Build kernel
nice -n 10 make -j16 kernel.img
# Copy kernel
cp kernel.img $CYANOGENMOD/device/rockchip/rk2918/kernel
cp kernel.img installer/kernel.img

#zip package
cd installer
zip -r vurrutKER_CWM_$(date +%Y%m%d).zip .
mv vurrutKER_CWM_$(date +%Y%m%d).zip ~
cd ..

# Make mrproper
make mrproper
rm kernel.img

