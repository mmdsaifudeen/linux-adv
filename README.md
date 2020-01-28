# linux-adv
# Linux_Over_ZYNQ
The project bring up Hdmi display into Linux (ZC706,ZC702,ZEDBoard))

1. Create zynq_fsbl from hdf file
2. Create boot.bin as in the pictrure.No offset addresses needed But better to keep order.
3. copy (uImage,devicetree.dtb,BOOT.BIN) in to boot partition of SDCard.
4. Create uEnv.txt at boot partition and enter as below,
     ------------------------------------------------------------------------------------------------------
            uenvcmd=run adi_sdboot
            adi_sdboot=echo Copying Linux from SD to RAM... && fatload mmc 0 0x3000000 ${kernel_image} && fatload mmc 0 0x2A00000 ${devicetree_image} && if fatload mmc 0 0x2000000 ${ramdisk_image}; then bootm 0x3000000 0x2000000 0x2A00000; else bootm 0x3000000 - 0x2A00000; fi
            bootargs=console=ttyPS0,115200 root=/dev/mmcblk0p2 rw earlyprintk rootfstype=ext4 rootwait
     ------------------------------------------------------------------------------------------------------        
            
5. Make root partition
    wget http://releases.linaro.org/archive/12.12/ubuntu/vexpress/linaro-precise-ubuntu-desktop-20121124-560.tar.gz
    enter this command:
    sudo tar --strip-components=3 -C /media/rootfs -xzpf linaro-precise-ubuntu-desktop-20121124-560.tar.gz binary/boot/filesystem.dir

