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

## Next steps for the agent (run on EC2)

This `cuse_gpio` spike is a **dead end for transparent GPIO** (CUSE cannot hand a
line-request fd to the caller — see "Important limitation"). Do **not** invest
more in making it transparent. Instead, evaluate the kernel-backed `gpio-sim`
path, which is the preferred candidate in
[docs/12_CUSE_MIGRATION_PLAN.md §2.3](../../../AgentCockpit/docs/12_CUSE_MIGRATION_PLAN.md).

Run these on EC2 Graviton (this dev box has no EC2 access; the agent on EC2 does):

1. Check whether the running kernel provides `gpio-sim`:

   ```bash
   modinfo gpio-sim 2>/dev/null && echo "MODULE_OK" || echo "NO_MODULE"
   zcat /proc/config.gz 2>/dev/null | grep -i gpio_sim || \
     grep -i gpio_sim /boot/config-"$(uname -r)" 2>/dev/null
   uname -r        # record kernel version (gpio-sim needs Linux 5.17+)
   ```

2. If `gpio-sim` is available, create a virtual chip via configfs and confirm a
   real `/dev/gpiochipN` appears (this one DOES return real line fds):

   ```bash
   sudo modprobe gpio-sim
   sudo mount -t configfs none /sys/kernel/config 2>/dev/null || true
   cd /sys/kernel/config/gpio-sim
   sudo mkdir agp && cd agp
   echo 8 | sudo tee bank0/num_lines        # create bank0 dir first if needed
   echo 1 | sudo tee live
   ls -l /dev/ | grep gpiochip               # expect a new gpiochipN
   ```

   Then drive a line and verify with `gpioget`/`gpioset` (libgpiod):

   ```bash
   gpiodetect; gpioinfo                       # find the sim chip name
   gpioset <sim-chip> 18=1                     # should NOT error on fd handoff
   gpioget <sim-chip> 17
   ```

3. Report back ONE of:
   - **GPIO_SIM_OK** + kernel version + the new `/dev/gpiochipN` name → we switch
     the GPIO plan to gpio-sim and the web-bridge talks to its sysfs/configfs
     value attributes instead of the bridge socket.
   - **NO_GPIO_SIM** → fall back to the CUSE internal-virtual-fd design in
     [docs/12 §4.2](../../../AgentCockpit/docs/12_CUSE_MIGRATION_PLAN.md), i.e. keep
     a real line fd inside the daemon and multiplex follow-up ioctls on the main
     fd (this is what `gpio_shim.so` already does in LD_PRELOAD form).

Do NOT report "done" until LED18 toggle is actually reflected in the web panel
and Button17 actually reaches `sensor_demo`. A passing `GPIO_GET_CHIPINFO_IOCTL`
alone is NOT "done".
