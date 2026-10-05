# rtl8156b-macos

Fix downloads stuck at 1.95 Gbit/s with USB 2.5 Gigabit Ethernet adapters built on the Realtek RTL8156B, on macOS 15 and later.

macOS 15 made the blocks these adapters send to the Mac much bigger:

| | Block size | Full-size frames per block |
| --- | --- | --- |
| macOS 14 | 12,288 bytes | 8 |
| macOS 15 and later | 32,764 bytes | 21 |

With 21 frames per block, the adapter pauses the sender before the block is full, so each block waits for the next 125 µs tick. Downloads then stop at about 1.95 Gbit/s instead of 2.35 Gbit/s. This happens when the device at the other end of the cable obeys Ethernet pause frames, as many routers and switches do. Uploads are not affected.

`ncmsize` sets a smaller block size on the running adapter, with the standard NCM request `SET_NTB_INPUT_SIZE`. Apple's driver stays in place: there is no driver to install, no security setting to change, and no need for root.

macOS sets its own size again whenever the adapter is plugged in or its network interface comes up. Run `ncmsize` again after that, or keep it running in the background.

## Usage

Build it with the Xcode command line tools (`xcode-select --install`):

```sh
clang -O2 -o ncmsize ncmsize.c -framework IOKit -framework CoreFoundation
```

Or download `ncmsize` from the [latest release](https://github.com/plcharriere/rtl8156b-macos/releases/latest) (Apple silicon, macOS 15 or later).

With the adapter plugged in:

```sh
./ncmsize             # list the adapters, with their chip and block size
./ncmsize -b 12288    # 8 frames per block, as macOS 14
./ncmsize -b 24572    # 16 frames per block: full speed
./ncmsize -b 32764    # back to what macOS 15 and later set
```

By default, `ncmsize` only changes RTL8156B adapters. `-c` picks other chips, such as `-c rtl8157`, and `-i` one interface, such as `-i en7`.

## Background

To keep the size set, run `ncmsize` once with `-d`:

```sh
./ncmsize -d -b 24572    # keep 24572 set, from now on and at every login
./ncmsize -d status      # is it running, and with which settings
./ncmsize -d off         # stop it
```

It puts the size back within a second or two whenever macOS resets it, and logs to `~/Library/Logs/ncmsize.log`. If you move `ncmsize`, run `-d` again.

## Measurements

The table shows 10-second iperf3 downloads from a router's 2.5 Gbit/s port to a MacBook Pro M1 Pro on macOS 27, through an RTL8156BG adapter, with flow control on. "Lost" counts TCP retransmissions.

| Block size (bytes) | Full-size frames per block | Download | Pause frames per second | Lost |
| --- | --- | --- | --- | --- |
| 4,092 | 2 | 1.02 Gbit/s | 23,330 | 0 |
| 8,188 | 5 | 1.84–1.88 Gbit/s | 14,100–14,400 | about 1,200 |
| 12,284 | 8 | 2.32–2.35 Gbit/s | 16–221 | 0–6 |
| 12,288 (as macOS 14) | 8 | 2.31–2.33 Gbit/s | 17–19 | 0–1 |
| 16,380 | 10 | 2.32 Gbit/s | 36 | 1 |
| 20,476 | 13 | 2.34 Gbit/s | 15,562 | 0 |
| 21,500–22,524 | 14 | 1.29–1.41 Gbit/s | 4,400–4,900 | about 1,000 |
| 23,548 | 15 | 2.31–2.35 Gbit/s | 1–2 | 0 |
| **24,572** | **16** | **2.35 Gbit/s** | **2** | **0** |
| 26,620 | 17 | 2.34 Gbit/s | 1 | 0 |
| 28,668 | 18 | 2.33 Gbit/s | 3 | 0 |
| 29,692 | 19 | 2.33 Gbit/s | 1 | 0 |
| 30,716 | 20 | 1.94 Gbit/s | 16,917 | 1 |
| 32,764 (macOS 15 and later) | 21 | 1.95 Gbit/s | 13,729 | 0 |

15 to 19 frames per block give full speed with almost no pause frames and no loss, and 24,572 is in the middle of that range. 20 frames or more bring the cap back.

## License

MIT, see [LICENSE](LICENSE).
