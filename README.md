Linux Kernel Modul for Loading Designs into Altera FPGAs
========================================================

Use the dd command to load RBF files:

```
# dd if=design.rbf of=/dev/fpga_loader bs=5M
```

Only the iMX6 is supported. But the code can easily be adjusted if the GPIO subsystem can be used.

