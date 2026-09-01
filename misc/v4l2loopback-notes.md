# v4l2loopback on kernel 6.18

Release 0.15.1 crashes on 6.18 (`vidioc_querycap`, `vidioc_s_fmt_vid`)
because `v4l2_fh_add()` / `v4l2_fh_del()` gained a `struct file *`
parameter in 6.18.

The upstream `main` branch already carries the compatibility shim:

```c
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
#define v4l2_fh_add(fh, filp) v4l2_fh_add(fh)
```

so build from `main`, not from the 0.15.1 tag.

This was only needed for the intermediate userspace-bridge approach
(see `tools/bridge.c`). Once libcamera drives the hardware directly,
the loopback is unnecessary.
