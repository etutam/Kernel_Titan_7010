# Make mrproper
rm kernel.img
make mrproper
#echo "Etutam" > $HOME/linux-compile-by
#echo "Titan7010A" > $HOME/linux-compile-host

# Set config
#make M712HC_defconfig
make titan7010A_defconfig
#make titan7010B_defconfig
make menuconfig

# Make modules
#nice -n 10 make -j16 modules
make modules

# Copy modules
find -name '*.ko' -exec cp -av {} /root/etutamROM/kernel/modules/ \;


# Make kernel
#nice -n 10 make -j8 kernel.img
make -j2 kernel.img

# Copy kernel
cp kernel.img /root/etutamROM/kernel/

# Make mrproper
#make mrproper
#rm kernel.img
#/root/rk2918/arch/arm/configs/titan7010A_defconfig
