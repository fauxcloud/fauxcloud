---
title: Troubleshooting
order: 50
lede: Find your symptom, learn the likely cause, and see what to do and what fauxcloud shows.
---

Start with the dashboard. fauxcloud names the part that went quiet, so the notice often points to the fix. The notices are explained in [Dashboard](dashboard.md#notices). This page is written for the verified console (your weather station's display unit): Vevor 7-in-1, model YT60234, firmware 1.0.1, EU 868 MHz.

## No AP appears when I hold BARO

**Cause.** The console starts its AP (its own setup Wi-Fi) only on its wall adapter, not on batteries. Or the model is not one fauxcloud works with.

**Fix.**

1. Plug in the wall adapter.
2. Hold **BARO** for more than 3 seconds until the display shows `AP`.
3. Look for a network named `WWS-` plus six characters.

If your console pairs only through an app (the SmartLife / Tuya model, YT60307), fauxcloud does not apply. See [Compatibility](station-profiles.md).

**fauxcloud shows this as** **No console paired yet.** on the dashboard, with the pairing steps.

## The setup page at 192.168.1.1 does not open

**Cause.** The console's own network takes one device at a time and sometimes drops it. If your router also uses `192.168.1.1`, the browser may show the router's page from its memory instead.

**Fix.**

- Turn off your phone's mobile data, then join `WWS-XXXXXX` on that one device only.
- Open `http://192.168.1.1` with `http://`.
- If the page is wrong, try a private window or another device.

fauxcloud's own Wi-Fi uses `192.168.77.x`, so it does not clash with a router at `192.168.1.x`.

**fauxcloud shows this as** nothing yet: this step happens on the console's own page, before fauxcloud is involved.

## The setup page of fauxcloud does not open

**Cause.** The phone may keep using mobile data because the Wi-Fi has no internet.

**Fix.** Join `fauxcloud-setup-XXXX`. Type the whole address `http://fauxcloud.setup`, including `http://`. The page is not available over `https://`. On Android and Samsung phones answer "Stay connected" or "Connect only this time".

## fauxcloud.local does not open

**Fix.**

- Use the board's IP address (for example `192.168.1.50`), which setup showed. It is also under **Details** in **Settings** (the page the **Admin** button opens; on a phone it is a padlock), as **Address on home network**.
- No address at hand? Some routers and phones do not resolve names like `fauxcloud.local`. Find the board in your router's list of connected devices: it appears there as `fauxcloud` (or, after a restart, the name you set under **Name on your network**). Open that address. It may change after a router restart unless you reserve it in the router.
- A name your router gives the board under its own domain, such as `fauxcloud.fritz.box`, shows only the word "success": fauxcloud answers only its address, `fauxcloud`, `fauxcloud.local` and a few private router names (`.lan`, `.home`, `.internal`, `.home.arpa`). Open the address or `fauxcloud.local` instead.
- If your home network itself uses `192.168.77.x`, the dashboard will not open from it, because fauxcloud's own Wi-Fi uses that range. Change your router's range, then unplug the board for a few seconds.

## The console joined, but the dashboard shows nothing

**Cause.** No upload has arrived yet, or the console is not using the right Wi-Fi details.

**Fix.**

- Give it two minutes: on the verified console the first upload comes about 12 seconds after it joins.
- Check the console's page: the network must be `fauxcloud-XXXX`, with the password you chose in setup. It is not your home Wi-Fi password. **Settings** → **fauxcloud Wi-Fi for your station** → **Show password** shows it.
- Check that **Weather Underground** is ticked and a station ID and key are filled in.
- Check that the console is on its adapter.
- Open **Settings**, the station (fauxcloud's record of the console), and its **Console** tab. If lines appear, the console is uploading.

**fauxcloud shows this as** **No readings yet** or **Waiting for the first upload**. If a second console tried to upload, **A console was turned away: no free station place**. If you changed the key, **Key changed**: accept it in **Settings**.

## Offline since a power cut

**Cause.** On batteries the console switches its Wi-Fi off, so nothing arrives until its adapter has power again.

**Fix.** Nothing. Readings resume by themselves. A gap in the history stays a gap.

**fauxcloud shows this as** **Offline since 14:05** (your time) and a line saying the console sends nothing on batteries or during a power cut.

## Sensor not updating, or the outdoor values froze

**Cause.** The outdoor sensor lost its link to the console. The console keeps uploading its last values unchanged, with no marker. A calm night can look the same for a while.

**Fix.** Check the outdoor sensor's batteries and the distance to the console. If it stays silent, take the console's batteries out briefly so it searches for the sensor again.

**fauxcloud shows this as** **Sensor not updating** or **Sensor missing**.

## Time is one hour off

**Cause.** The console's time zone and daylight saving are set by hand in its menu. They change what the display shows, not what it uploads. The console's own setup page also overwrites the time zone you set in its menu when you press Apply.

**Fix.**

- Check the time zone in the console's menu and on its setup page.
- If the clock is off by more, switch NTP (the time from a time server) off and on in the console's SET menu.
- fauxcloud takes its own time zone from the city you picked in setup. Change it in **Settings** → **Location and time**.

**fauxcloud shows this as** **Console clock off**, with how far the clock is off.

## Pressure jumped to 1013

**Cause.** The console resets REL (its relative pressure setting, which you set to sea-level pressure) to the factory value, 1013.2 hPa, when you change a display unit, step past item 15 of its SET menu, or take its batteries out.

**Fix.** Look up the current sea-level pressure of a nearby official station or airport. Set REL again in the console's SET menu. Do not change display units later without setting REL again.

**fauxcloud shows this as** **Pressure at factory value**. With airport weather on, it may show **Pressure looks off** with a value to set.

## Temperature reads 158

**Cause.** 158 °F is 70 °C. Other users report that a temperature stuck at 70 °C with humidity at 1 %, never changing, means a failed outdoor temperature and humidity board, not the weather.

**Fix.** Other users report that only the outdoor temperature and humidity board fails; the console's display and Wi-Fi keep working. See [Known hardware weak spots](#known-hardware-weak-spots).

**fauxcloud shows this as** **Sensor not updating** once the values stop changing. There is no separate notice for a faulty sensor board.

## Rain is not counting

**Cause.**

- The rain gauge counts about 0.2 mm per tip of its bucket. Drizzle and the start of a shower may not show.
- On the verified console, the first tip of the day does not reach the upload at all. **Rain today** starts at the second tip.
- Snow does not tip the bucket. It counts when it melts.

**Fix.** Pour a measured cup of water slowly over the funnel and watch the count. Keep the funnel clear of leaves and insects. Other users report that a count stuck at zero can be the gauge's tipping rocker (the see-saw) or its magnet: see [Known hardware weak spots](#known-hardware-weak-spots).

**fauxcloud shows this as** nothing: it shows what the console sends. There is no notice for a silent rain gauge.

## The wind direction is wrong

**Cause.** The wind vane can be seated off the flat of its rod. On the verified console a vane seated off the flat read about 60 degrees off.

**Fix.** Seat the vane with its screw on the flat of the rod. With its tip toward the funnel, the vane reads 0 degrees, so the funnel must face north. To correct a small error, open **Settings**, the station, its **Calibration** tab, and use **Set north from the vane** or the **Wind direction offset**. If your console has its own calibration, set it in one place only.

## Home Assistant shows unavailable

**Cause.** Nothing arrived from the station for 4 minutes (the default), so Home Assistant shows **unavailable** instead of a frozen value. Or fauxcloud cannot reach your MQTT broker.

**Fix.**

- Look at the dashboard first. If it shows **Offline since** or a sensor notice, fix that.
- If it shows **MQTT disconnected**, open **Settings** → **MQTT**, check the broker address, user and password, and press **Test connection**.
- Trigger automations on the notice id, not on its text.

More in [Home Assistant](home-assistant.md).

## The console's Wi-Fi keeps dropping

> [!NOTE]
> **Community tip, not tested by fauxcloud**
> Other users report that the console's link to a network drops sometimes. Keep the console on its adapter, and note the time of each drop.

**fauxcloud shows this as** **Offline since** and then readings resume on their own.

## My weather is not on the Weather Underground website

**Cause.** With fauxcloud the console talks to fauxcloud, not to Weather Underground, so that website sees nothing from it. Weather Underground also refuses uploads without a parameter this console never sends. This was found by replaying the console's own request against the service, not on the console.

**Fix.** The dashboard is your real status. If you want a public page, turn on online services in **Settings**: fauxcloud adds the missing parameter when it sends. See [Online services](online-services.md).

## The board is dark, or opens no Wi-Fi

**Fix.** Check the power supply and the board's LED (see [Boards](boards.md#the-led)). If the board sits in a case, check that nothing presses its EN or BOOT button (the board's reset and boot buttons). If your computer does not see the board at all, see [the USB cable and drivers](flash.md#if-the-board-is-not-seen).

## After the install the board shows no Wi-Fi, or does not join

**Cause.** After a flash from the installer the board restarts by itself, but on some boards the radio does not come up until the power is cut. This was seen on one DevKit during testing.

**Fix.** Unplug the board, wait 30 seconds and plug it in again. Then look for the setup Wi-Fi `fauxcloud-setup-XXXX` (a new board) or open the dashboard at `fauxcloud.local` (a board that kept its settings).

**fauxcloud shows this as** nothing: the board cannot be reached until it starts again. The installer's last step says to unplug it.

## A new version works worse than the one before

**Fix.** Install the version you had before: download its `fauxcloud-<version>.bin` from the [releases page](https://github.com/fauxcloud/fauxcloud/releases) and install it as in [Update later, without a cable](flash.md#update-later-without-a-cable). The update page installs an older version too. Then tell us in a [bug report](https://github.com/fauxcloud/fauxcloud/issues/new?template=bug_report.yml).

## History not saved

**Cause.** The history storage on the board has a different layout from the one this firmware needs. fauxcloud never erases it by itself.

**Fix.** One install over USB: see [Flash the board](flash.md#if-the-history-is-not-saved). Export the CSV of anything you want to keep first.

**fauxcloud shows this as** **History not saved**.

## I forgot the admin password

**Fix.** Hold the board's BOOT button for 5 seconds. This erases everything, including your stations and settings, and runs setup again. See [Boards](boards.md#the-boot-button).

## Known hardware weak spots

> [!NOTE]
> **Community-reported, not tested by fauxcloud**
> Everything in this section comes from other users' reports. We have not repeated any repair. If you try one, you do so at your own risk.

- **Outdoor temperature and humidity board.** Several users report readings stuck at 70 °C or 1 % humidity after moisture reached the board in the rain pod, usually after 6 months to 2 years. Replacing the console is not needed: the display and Wi-Fi are fine. One repair, reported by a single user, swaps the sensor chip for a small module on the sensor connector; other sensor chips are reported not to work.
- **Rain gauge see-saw.** The magnet can fall out, or the see-saw can be fitted backwards, so no rain is counted. The reported fix is to glue the magnet in place and check the see-saw's direction.
- **Wind bearings near the sea.** The anemometer and vane bearings are reported to corrode after about two years in sea air.

For a console that is not on the list, see [Compatibility](station-profiles.md) and send a console report.
