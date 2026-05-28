# Bring-up test logs — one folder per physical board

Each physical EVT board has its own folder under here. The folder name
encodes the form factor and board number; the file inside lists the
chip ID and the test result on a specific date.

```
bringup_logs/
├── evt2_brd1/                  ← physical EVT2 board #1
│   ├── 2026-05-27_demo.txt              (chip ID 1b62399045bf9c64)
│   └── 2026-05-27_demo_with_gnss.txt
├── evt2_brd2/                  ← when a second EVT2 board ships
│   └── ...
└── evt1_brd3/                  ← if we ever revalidate an EVT1
    └── ...
```

## Conventions

- **Folder name**: `<form-factor>_brd<N>` where N is the physical
  board number assigned at first test. The chip ID lives inside the
  file so you can confirm a folder maps to the right silicon.
- **File name**: `YYYY-MM-DD_<short-description>.txt`. Multiple files
  per board per day are fine — they're cheap, and capturing both
  "with feature X" and "without feature X" runs lets us diff later.
- **File contents**: the full RTT `test_report` summary block, plus
  a short header saying which board, chip ID, and any relevant
  config flags (`RUN_GNSS_PROBE`, `GNSS_ASSISTED`, etc.).

## Why this matters

The firmware's `BOARD_NUMBER` macro in main.c is a *firmware-set
counter*, not the physical board number — it can lag, get reset, or
mean different things across reflashes. The chip ID
(`hwinfo_get_device_id`) is the only durable identifier per physical
SoC. These captured logs are the audit trail of which physical chip
passed which tests on which date.
