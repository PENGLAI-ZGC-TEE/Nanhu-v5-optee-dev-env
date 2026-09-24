# Nanhu V5 OP-TEE Development Environment

## Get the Sources

After cloning the repository, initialize all submodules:

```bash
git submodule update --init --recursive --progress
```

The compressed root filesystem is tracked as
`config/rootfs_nopasswd.cpio.gz`. The Makefile generates the uncompressed
archive automatically when Linux is built.

To unpack the root filesystem for modification:

```bash
make rootfs-extract
```

After changing files under `rootfs/`, repack it with:

```bash
make rootfs-pack
```

## Build

Show all supported targets:

```bash
make help
```

Build the QEMU, OP-TEE, Linux, DTB, and OpenSBI components used by the TEE
boot flow:

```bash
make qemu
make optee_os
make linux-image
make opensbi-jump
```

The main build outputs are:

```text
build/qemu/qemu-system-riscv64
build/optee_os/core/tee.bin
build/linux/arch/riscv/boot/Image
build/nanhu-v5.dtb
build/opensbi/platform/generic/firmware/fw_jump.bin
```

## Run

Build missing components and start Linux with OP-TEE:

```bash
make run-tee
```

Start Linux without OP-TEE, using the OpenSBI payload image:

```bash
make run-linux
```

Build the merged OpenSBI, OP-TEE, and Linux image:

```bash
make merge
```

Run the merged image in QEMU:

```bash
make run-fpga
```

Use `Ctrl-a x` to exit the QEMU `-nographic` console.

## Debug

Start QEMU with the GDB server enabled and the CPUs stopped at reset:

```bash
make debug
```

In another terminal, load the OpenSBI, Linux, and OP-TEE symbols:

```bash
gdb-multiarch -x nanhu_gdbinit
```

## Clean

Clean individual components:

```bash
make qemu-clean
make linux-clean
make optee_os-clean
make opensbi-clean
make dtb-clean
make merge-clean
```

Remove all generated build outputs:

```bash
make clean-all
```

`guest_log.txt`, `build/`, and the generated uncompressed root filesystem are
ignored by Git.
