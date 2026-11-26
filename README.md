# Tele Radio T20

Flipper Zero FAP to record, save and replay Tele Radio T20 fixed code remotes.
This is only tested with the T20 869 MHz remote. The app also offers 433.92 MHz since it should be the same.

## What it does

- **Record** at the selected band using the CC1101 preset embedded in the app,
  decodes the frames (8N1 framing + CRC-16/KERMIT), shows the captured ID and
  button, and saves the key to `/ext/subghz/TeleRadioRemote/`.
- **Replay** from the button list. OK sends the highlighted button. A short press
  sends the configured number of frames, and holding OK keeps sending until you release.

## How it works

- 869.85 MHz or 433.92 MHz 2-FSK. Bit cell 423 us. Frame = 12 idle HIGH cells + 11 bytes x 10 cells
  (8N1, start `0`, 8 data bits LSB-first, stop `1`) = 122 cells ~ 51.6 ms.
- Bytes sent `FF FF | ID0 ID1 ID2 | 00 00 | BTN | 00 | CRC_HI CRC_LO`.
  CRC-16/KERMIT over `ID0 ID1 ID2 00 00 BTN 00`, big-endian on the wire.
  Fixed 24-bit code, buttons one-hot `01/02/04`.
- The preset is written into every saved key file.

## Build

```sh
pip install ufbt
ufbt
./test/run_tests.sh
```

`application.fam` must be at the repo root.

## Install / use

Copy `tele_radio_remote.fap` to `/ext/apps/` on the SD card. Record to 
capture a remote, then pick the remote ID and the button and press OK to 
send. After release, frames already queued in the radio's DMA buffer still 
finish, so transmission ends a few frames later.

Every saved remote and button is listed, sorted by ID.
The stock firmware declines transmission at the 869 MHz frequency. Enable unsupported
Sub-GHz frequencies on stock firmware or use custom firmware which removes the regional
transmit lock entirely and exposes all supported frequencies.

## License

[EUPL-1.2](LICENSE)
