# memdump — Volatile Memory Acquisition LKM

Read-only Linux Kernel Module for DFIR and cybersecurity research.

---

## Directory Structure

```
memory-forensics/
├── Makefile
├── mem_acquire.c
├── dump_tool.c
└── README.md
```

---

## Dependencies

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) libssl-dev
```

---

## Build

```bash
make           # release build
make debug     # debug symbols + ASan on userspace tool
make clean     # remove all build artifacts
```

---

## Load

```bash
# Default: 64MB buffer, live physical memory acquisition
sudo insmod mem_acquire.ko

# Custom buffer size (128MB)
sudo insmod mem_acquire.ko buffer_size=$((128*1024*1024))

# Test mode: fills buffer with 0x00-0xFF pattern, no physical memory access
sudo insmod mem_acquire.ko fill_pattern=1
```

---

## Module Parameters

| Parameter | Default | Description |
|---|---|---|
| `buffer_size` | 64MB | Acquisition buffer size in bytes |
| `fill_pattern` | 0 | Set to 1 for test pattern instead of live memory |

---

## Status

```bash
cat /proc/memdump_info
dmesg | tail -10
```

`/proc/memdump_info` outputs:

```
driver_version : 2.0.0
buffer_size    : 67108864
acquired_bytes : 65536000
timestamp_ns   : 1234567890000
fill_pattern   : no
kernel_version : 6.8.0
```

---

## Acquire

```bash
# Standard read()-based (1MB chunks)
sudo ./dump_tool

# Zero-copy mmap mode
sudo ./dump_tool --mmap

# Custom output path
sudo ./dump_tool --out /evidence/node1.bin

# mmap with custom output
sudo ./dump_tool --mmap --out /evidence/node1.bin
```

---

## Output

The tool writes two files:

```
memory_dump.bin       ← raw memory image
memory_dump.bin.json  ← chain-of-custody sidecar
```

Sidecar format:

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

## ioctl Reference

| Command | Type | Description |
|---|---|---|
| `MEMDUMP_GET_SIZE` | `unsigned long` | Total acquired bytes |
| `MEMDUMP_GET_TS` | `unsigned long long` | Acquisition timestamp (ns since boot) |
| `MEMDUMP_GET_KVER` | `char[64]` | Kernel version string |
| `MEMDUMP_RESET` | — | Re-run acquisition and refresh buffer |

---

## Unload

```bash
sudo rmmod mem_acquire
```

---

## Makefile Targets

```
make          - release build (module + tool)
make debug    - debug build
make clean    - remove artifacts
make reload   - unload, rebuild, reload module
make info     - print /proc/memdump_info
make help     - print all targets
```

---

## Security

- Use only on systems you are authorized to access
- Acquired dumps may contain credentials, keys, and sensitive data
- Disable Secure Boot before loading
- Destroy dumps securely after analysis

---

## License

GPL-2.0 — For educational and authorized defensive security research only.
