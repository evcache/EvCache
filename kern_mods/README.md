The `setup_all_mods.sh` script will compile and install both the `vcolor` and `gpa_hpa` 
kernel modules under this directory.

Run using:
```bash
sudo ./setup_all_mods.sh
```

You can also compile and install them individually by running the `setup_mod.sh` under their
own directories.

> [!NOTE]
> Prerequisites to compile, install, and use the modules:
> * `vcolor_km`: Boot into custom vColor kernel. Refer to [the vcolor_km README](./vcolor_km/README.md).
> * `gpa_hpa_km`: Add support for the hypercall on the host. Refer to [the gpa_hpa_km README](./gpa_hpa_km/README.md).
