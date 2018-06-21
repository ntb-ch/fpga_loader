Linux Kernel Modul for Loading Designs into Altera FPGAs
========================================================

Use the dd command to load RBF files:

```
# dd if=design.rbf of=/dev/fpga_loader bs=5M
```

The current module supports the Colibri modul from Toradex (iMX6 processor). The loader uses the GPIO subsystem. For other platforms you simply have to adjust the GPIO pins numbers.


## Building

Choose the target plattform and find its appropriate kernel configuration together with the kernel headers.

  * arch: target plattform such as powerpc or arm 
  * comp: cross compiler such as powerpc-linux-gnu- or arm-linux-gnueabi-
  * path: path to kernel headers (path to root of kernel repository)

### Example for ARM
```bash
user@hostn:~/myproj/fpga_loader$  export ARCH=arm
user@hostn:~/myproj/fpga_loader$  export CROSS_COMPILE=arm-linux-gnueabihf-
user@hostn:~/myproj/fpga_loader$  make KERNELDIR=~/kernels/linux-toradex
```

Using a compiler which is not installed on the system, the corresponding path must be added to the environment variable:
```bash
user@hostn:~/myproj/fpga_loader$ export PATH=~/compilers/gcc-linaro/bin/:$PATH 
user@hostn:~/myproj/fpga_loader$  export ARCH=arm
user@hostn:~/myproj/fpga_loader$  export CROSS_COMPILE=arm-linux-gnueabihf-
user@hostn:~/myproj/fpga_loader$  make KERNELDIR=~/kernels/linux-toradex
```
