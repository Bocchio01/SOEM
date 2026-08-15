# Machine setup for Xenomai 4 (EVL)

When compiling SOEM with `-DUSE_XENOMAI_EVL=ON`, the library will attempt to link against the Xenomai 4 (EVL) real-time core.

This requires:
* A Linux kernel patched with Xenomai 4 (EVL) support;
* The `libevl` user-space library installed on the system.

This guide provides the main steps to set up a machine for Xenomai 4 (EVL) development and testing.



## Installing a Xenomai 4 (EVL) Patched Kernel

Refer to the [Xenomai 4 (EVL) documentation](https://v4.xenomai.org/core/build-steps/index.html) for detailed instructions on how to build and install a Xenomai 4 (EVL) patched kernel.


### Install Build Dependencies

First, ensure your host has the necessary tools to compile a Linux kernel:
```bash
sudo apt-get update
sudo apt-get install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev bc
```


### Fetch the Kernel Sources

Clone the EVL kernel tree.
This repository is maintained by the Xenomai 4 project and already contains the Dovetail and EVL core implementations.

```bash
git clone --depth 1 https://gitlab.com/xenomai/xenomai4/linux-evl.git
cd linux-evl
```


### Configure the Kernel

Configure the kernel settings for your system:

```bash
make menuconfig
```

When configuring the kernel via your configuration tool (e.g., `menuconfig`), ensure the following specific options are set within the `General setup` menu to allow SOEM to function correctly:

* `CONFIG_EVL`: Enables the EVL core itself.
* `CONFIG_EVL_NET`: Enables out-of-band networking support. This is strictly required; without it, the eBPF filter cannot route EtherCAT frames into the out-of-band network stack, causing real-time socket communication to fail.
* `CONFIG_EVL_LATMUS` & `CONFIG_EVL_HECTIC`: Enable these drivers to run the `latmus` and `hectic` utilities provided by `libevl`. These are essential for measuring system latency and validating context-switching sanity before running high-frequency control loops.
* `CONFIG_EVL_RUNSTATS`: (Defaults to `Y`) Collects runtime statistics about threads, which is useful for profiling real-time tasks.
* `CONFIG_EVL_DEBUG_MEMORY`: Ensure this is disabled (`N`). The documentation explicitly warns that enabling this option adds significant overhead and degrades system latency figures.
* `CONFIG_COMPAT` & `CONFIG_COMPAT_VDSO`: Enable these only if you plan to run 32-bit applications on a 64-bit kernel (e.g., ARM32 code running over an arm64 kernel).

Beyond these specific flags, the default values for other EVL settings are safe to use.


### Build and Install

Once configured, compile the kernel and its modules.
This process will take some time depending on your CPU.

```bash
make -j$(nproc)
sudo make modules_install
sudo make install
```

The `make install` command will automatically copy the kernel image (`bzImage`) to `/boot` and update your GRUB bootloader.


### Reboot and Verify

Restart your machine and ensure you select the newly compiled EVL kernel from the GRUB menu if it does not boot by default.

You can verify your host kernel successfully supports EVL after booting by running:

```bash
dmesg | grep -i evl
```



## Installing the `libevl` User-Space Library

Again, refer to the [Xenomai 4 (EVL) documentation](https://v4.xenomai.org/core/build-steps/index.html) for instructions on how to build and install the `libevl` user-space library.

Briefly, installing `libevl` typically involves the common steps of downloading the source code, configuring the build, compiling, and installing it to the system.
To do this, you will need to install the following dependencies on your system (`EVL` project uses `meson` as the build system and `ninja` as the build tool):

```bash
sudo apt-get update
sudo apt-get install -y meson ninja-build libbpf-dev cpio
```

Then we fetch the sources of the libraries, compile them and install them in the system.

```bash
git clone --depth 1 https://gitlab.com/xenomai/xenomai4/linux-evl.git
git clone --depth 1 https://gitlab.com/xenomai/xenomai4/libevl.git

meson setup -Dbuildtype=release -Dprefix=/usr/local -Duapi=linux-evl build-evl libevl

cd build-evl
meson compile -C .
ninja -C . install
```

You can verify that `libevl` is installed correctly by checking with one of the following commands:

```bash
$: ldconfig -p | grep libevl
    libevl.so.8 (libc6,x86-64) => /usr/local/lib/x86_64-linux-gnu/libevl.so.8
    libevl.so (libc6,x86-64) => /usr/local/lib/x86_64-linux-gnu/libevl.so

$: which evl
/usr/local/bin/evl
```



## Compiling SOEM with Xenomai 4 (EVL) Support

Once the Xenomai 4 (EVL) patched kernel and `libevl` user-space library are installed, you can compile SOEM with Xenomai 4 (EVL) support by running the following commands:

```bash
mkdir build && cd build
cmake .. -DUSE_XENOMAI_EVL=ON
cmake --build .
cmake --install .
```


### Run an Example

Since raw sockets and EVL devices require elevated permissions, run the compiled binaries with `sudo`:

```bash
sudo ./build/install/bin/ec_sample eth0
```