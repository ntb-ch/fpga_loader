Linux Kernel Modul for Loading Designs into Altera FPGAs
========================================================

Use the dd command to load RBF files:

```
# dd if=design.rbf of=/dev/fpga_loader bs=5M
```

The current module supports the Colibri modul from Toradex (iMX6 processor). The loader uses the GPIO subsystem. For other platforms you simply have to adjust the GPIO pins numbers.

