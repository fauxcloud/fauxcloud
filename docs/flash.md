---
title: Flash the board
order: 20
lede: Put fauxcloud on your ESP32 board, and learn how to update it later without a cable.
---

You need a computer and a USB data cable, once. After this, you install firmware updates from **Settings** (the dashboard's **Admin** button, a padlock on a phone, opens it).

> [!NOTE]
> **Free software, as is**
> fauxcloud is free software under GPL-3.0-or-later, provided as is, without warranty of any kind; you flash and run it at your own risk.

## If the board is not seen

Most boards have a small square chip next to the USB port that talks to your computer. Its marking says CP2102 or CH340. Windows and macOS usually recognise the CP2102 without a driver; the CH340 often needs one. The official drivers: [Silicon Labs, CP2102](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers) and [WCH, CH340 for Windows](https://www.wch-ic.com/downloads/CH341SER_EXE.html).

A charge-only cable shows no port at all: if nothing appears, try another cable first. Linux needs no driver, but your user may need to be in the `dialout` group (`uucp` on some distributions) to open the port.

## Two ways to install

Both write the same release to the board over USB: from the browser, or by hand with esptool, a free command-line tool that writes the board.

### From the browser

Chrome or Edge on a computer and the USB data cable are all it takes; there is nothing to install. Phones, tablets, Safari and Firefox cannot write to a USB board (they have no Web Serial): use esptool there.

1. Open the [installer](https://fauxcloud.org/install/) and pick your **Board** and the **Version** (the newest release is chosen for you).
2. Plug the board in and press **Connect**. Your browser lists the serial ports: pick the board's.

   **You should see** **CONNECTED**, the chip and "4MB flash". If the list stays empty, see [If the board is not seen](#if-the-board-is-not-seen); if it says the board did not answer, hold the board's **BOOT** button while you press Connect.
3. Press **Install** and keep the cable plugged in for about a minute.

   **You should see** **FLASHING** with a percentage, then **DONE**, the name of the board's setup Wi-Fi and a QR code that joins it.

   Then unplug the board, wait 30 seconds and plug it in again: some boards bring their Wi-Fi up only after a power cut (see [Troubleshooting](troubleshooting.md#after-the-install-the-board-shows-no-wi-fi-or-does-not-join)).

Before it writes anything, the installer checks the download against the release's size and SHA-256, and reads the board's flash layout (its partition table). If the board already has a different layout, it stops with "This board has a different flash layout": tick the box below and install again, which erases the settings. A new board, with no layout yet, goes ahead. A board that came with other firmware on it stops once in the same way: tick the box, there is nothing on it to keep. Then it writes the same four parts as esptool below, at the offsets the release lists, checks each one after writing it, and erases nothing else, so a board that already runs fauxcloud keeps its settings.

The box **Erase everything, settings included: for a clean start only** erases the whole flash first: tick it for a clean start or when the installer asks for it, never to update.

[**Flash from the browser**](https://fauxcloud.org/install/)

### By hand with esptool

1. Download the files of the latest release from the [releases page](https://github.com/fauxcloud/fauxcloud/releases). For a first install you need `fauxcloud-<version>-factory.zip`.
2. Unzip it. It holds four parts, each with its address, and a `README.txt` with the command.
3. Connect the board to your computer with the USB cable.
4. Install [esptool](https://docs.espressif.com/projects/esptool/) and write the four parts. Replace `PORT` with the board's serial port and `<version>` with the release number:

   ```bash title="Write the four parts"
   esptool --chip esp32 --port PORT write-flash 0x1000 fauxcloud-<version>-bootloader.bin 0x8000 fauxcloud-<version>-partitions.bin 0xE000 fauxcloud-<version>-boot_app0.bin 0x10000 fauxcloud-<version>.bin
   ```

   esptool 4.x spells the command `write_flash`.
5. Unplug the board and plug it in again.

   **You should see** the board's LED blink fast, four times a second, and a Wi-Fi called `fauxcloud-setup-XXXX`. Go on with [Get started](get-started.md#set-up-fauxcloud).

> [!WARNING]
> **Never write a merged image at 0x0 over a working board**
> fauxcloud keeps its settings (Wi-Fi, the MQTT link to Home Assistant, keys, learned values) between the partition table (the map of the board's memory) and the app. A merged image (all parts in one file) written at `0x0` covers that gap and erases them. That is why a release ships the four parts separately and no merged image. On this site's installer, or any browser flasher that offers to erase the device first, leave that box unticked to keep your settings.

> [!TIP]
> For a clean start on purpose, run `esptool --chip esp32 --port PORT erase-flash` first.

## If the history is not saved

After some updates **Settings** may show **History not saved**: "this firmware needs one USB flash to prepare its storage". Readings still arrive and show, but the history is kept in memory only until the next restart. fauxcloud never formats the history area by itself.

The fix is one install over USB. Export the CSV of any range you want to keep first, because the history starts again.

1. Erase everything after the first app slot: the second app slot, the crash report and the history.

   ```bash title="Erase everything after the first app slot (history included)"
   esptool --chip esp32 --port PORT erase-region 0x1E0000 0x220000
   ```
2. Write the four parts again, as in [By hand with esptool](#by-hand-with-esptool). This step is required: it puts the app and the boot selector back.

Your settings, keys and learned values stay, as long as you follow the warning under [By hand with esptool](#by-hand-with-esptool).

## Update later, without a cable

Download `fauxcloud-<version>.bin` from the [releases page](https://github.com/fauxcloud/fauxcloud/releases). Not the `-factory.zip`: that one is for a first install over USB.

1. Open **Settings** from your home network and go to **Maintenance**.
2. Under **Firmware update**, press **Choose a file** and pick the `.bin` file.

   **You should see** the file name, its size and the room available, as in "fauxcloud-<version>.bin: N KB of M KB available". If the file is too large, the line says so.
3. Press **Install**.

   **You should see** "Uploading…" with a percentage, then "Installed" and the new version. fauxcloud restarts and the page reconnects by itself. An update takes about a minute.

fauxcloud checks that the file is a fauxcloud image, installs it next to the running version and restarts. Your stations, settings and history stay.

If the new version does not start properly, fauxcloud goes back to the old one by itself. **Maintenance** then says **The last update was rolled back.** There is no automatic update: nothing installs unless you choose it. To go back by hand, install an older `.bin` the same way: the update page checks that the file is a fauxcloud image, not its version.

If the page says "Not enough free memory for an update", restart fauxcloud from **Maintenance** and try again. If it says an update is already running, wait for it to finish.
