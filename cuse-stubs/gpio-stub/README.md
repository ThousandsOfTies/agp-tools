# cuse_gpio

Experimental CUSE GPIO chip stub for AgentCockpit.

This stub can expose `/dev/gpiochip0`, answer chip/line metadata ioctls, and
route simple output/input value operations to `web-bridge/bridge.py`.

Important limitation:

Linux GPIO chardev v1 and v2 line request ioctls return a new line-request file
descriptor to the caller. A userspace CUSE daemon cannot install a fresh file
descriptor into another process. Because of that, this stub cannot be a fully
transparent replacement for `gpio_shim.so` for existing applications that call
`GPIO_GET_LINEHANDLE_IOCTL` or `GPIO_V2_GET_LINE_IOCTL` and then operate on the
returned fd.

Use this as an ABI/bridge spike. A production replacement needs one of:

- a small application-side GPIO abstraction that keeps using the chip fd in sim,
- a kernel-backed fake GPIO provider such as `gpio-mockup`,
- a dedicated kernel module, or
- keeping an `LD_PRELOAD`/seccomp-style fd mediation layer for the fd-returning
  request ioctl only.

Run:

```bash
make -C cuse-stubs/gpio-stub
sudo cuse-stubs/gpio-stub/cuse_gpio -f --devname=gpiochip0
sudo chmod 666 /dev/gpiochip0
```

## Current verification

Verified on 2026-06-02:

- Built in Codespace with `aarch64-linux-gnu-gcc`.
- Deployed to EC2 Graviton as `/home/ubuntu/cuse_gpio`.
- EC2 already exposes a real `/dev/gpiochip0` (`ARMH0061:00`), so the spike was
  run as `/dev/agp-gpiochip0` to avoid collision.
- `GPIO_GET_CHIPINFO_IOCTL` succeeds and returns:

```text
name= gpiochip0_sim
label= AgentCockpit CUSE GPIO
lines= 54
```

The spike now proves that the CUSE node can be created and can answer a basic
GPIO ioctl. It does not yet prove LED/Button bridge behavior, and it still does
not solve the line-request fd handoff limitation described above.
