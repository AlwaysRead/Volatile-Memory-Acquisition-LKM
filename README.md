# memdump v2.0.0 — Optimized Volatile Memory Acquisition LKM

Read-only Linux Kernel Module for DFIR / cybersecurity research.

---

## What changed from v1

| Area | v1 | v2 |
|---|---|---|
| Buffer allocator | `kmalloc` (physically contiguous, fails >128KB) | `vmalloc` (virtually contiguous, works for 64MB+) |
| Buffer size | 4 KB hardcoded | Configurable via `buffer_size=` module param (default 64 MB) |
| Read chunk size | 4 KB (excessive syscalls) | 1 MB chunks |
| Zero-copy | ✗ | `mmap()` support via `remap_vmalloc_range` |
| Seeking | ✗ | `fixed_size_llseek` — jump to any offset |
| ioctl interface | ✗ | `GET_SIZE`, `GET_TS`, `GET_KVER`, `RESET` |
| /proc entry | ✗ | `/proc/memdump_info` — live status |
| Inline SHA-256 | ✗ (external `sha256sum`) | Inline via OpenSSL, written to `.json` sidecar |
| Chain-of-custody | ✗ | JSON sidecar with hash, timestamp, kernel version |
| Concurrency guard | ✗ | `mutex_trylock` — prevents simultaneous opens |
| Physical memory | Dummy pattern | PFN walker (`kmap_atomic` / `kunmap_atomic`) |
| Kernel compat | Hardcoded `class_create(THIS_MODULE,...)` | `#if LINUX_VERSION_CODE` branch for ≥6.4 |
| Build system | Single target | `all`, `debug`, `clean`, `reload`, `info`, `help` |

---

## Dependencies

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) libssl-dev
```

---

## Build

```bash
make          # release build
make debug    # with debug symbols + ASan for userspace tool
```

---

## Load

```bash
# Default: 64 MB buffer, live physical memory acquisition
sudo insmod mem_acquire.ko

# 128 MB buffer
sudo insmod mem_acquire.ko buffer_size=$((128*1024*1024))

# Test mode: fills buffer with 0x00-0xFF pattern, no physical memory access
sudo insmod mem_acquire.ko fill_pattern=1
```

Check status:

```bash
cat /proc/memdump_info
dmesg | tail -10
```

---

## Acquire

```bash
# Standard read()-based (1 MB chunks)
sudo ./dump_tool

# Zero-copy mmap mode
sudo ./dump_tool --mmap

# Custom output path
sudo ./dump_tool --mmap --out /evidence/node1.bin
```

Output:

```
=== memdump Acquisition Tool v2.0.0 ===
  Device:         /dev/memdump
  Output:         memory_dump.bin
  Kernel:         6.8.0
  Declared size:  64.0 MB
  Mode:           mmap (zero-copy)

  Acquired:   64.0 MB /   64.0 MB  [100.0%]
  Written:    64.0 MB
  SHA-256:    a3f2...

  Sidecar saved:  memory_dump.bin.json

Done.
```

Sidecar (`memory_dump.bin.json`):

```json
{
  "tool"            : "memdump dump_tool v2.0.0",
  "output_file"     : "memory_dump.bin",
  "acquired_bytes"  : 67108864,
  "kernel_version"  : "6.8.0",
  "acq_timestamp_ns": 1234567890000,
  "utc_time"        : "2025-01-01T12:00:00Z",
  "sha256"          : "a3f2..."
}
```

---

## Unload

```bash
sudo rmmod mem_acquire
```

---

## ioctl Reference

| Constant | Direction | Description |
|---|---|---|
| `MEMDUMP_GET_SIZE` | read `unsigned long` | Acquired byte count |
| `MEMDUMP_GET_TS` | read `unsigned long long` | Acquisition timestamp (ns since boot) |
| `MEMDUMP_GET_KVER` | read `char[64]` | Kernel version string |
| `MEMDUMP_RESET` | none | Re-run acquisition and refresh buffer |

---

## Analysis with Volatility

```bash
volatility -f memory_dump.bin linux_pslist
volatility -f memory_dump.bin linux_netstat
volatility -f memory_dump.bin linux_malfind
```

---

## Recommended test environment

- Ubuntu or Kali VM
- QEMU/KVM guest
- Kernel 5.x / 6.x test system
- Secure Boot **disabled**

---

## Security considerations

- Deploy only with explicit authorization
- Acquired dumps may contain credentials, keys, and PII
- Use encrypted transport for remote acquisition
- Destroy dumps securely after analysis
- Never run on production systems without incident-response authority

---

## License

GPL-2.0 — For educational and authorized defensive security research only.
