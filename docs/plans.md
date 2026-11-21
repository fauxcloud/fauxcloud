---
title: Plans
order: 90
lede: What version 1.0 is, what is being worked on next, and what is wanted later. No dates.
---

This page lists directions, not promises. A feature ships when it works on a real console. An idea can change or be dropped when the readings say so.

## Version 1.0

**Verified on one unit:** a Vevor 7-in-1, model YT60234, display firmware 1.0.1, EU 868 MHz (see [Compatibility](station-profiles.md)).

The dashboard and **Settings** (the dashboard's **Admin** button, a padlock on a phone, opens it) come in **English and Polish**; setup is in English.

What it does:

- Its own Wi-Fi for your weather console; fauxcloud takes the upload the console meant for the cloud.
- A dashboard on your network, which you can pin to your phone's home screen.
- Home Assistant entities over MQTT.
- A history on the board, with export to CSV.
- A local outlook from your station's own readings.
- A notice that names the part that went quiet when something stops reporting.
- Firmware updates from **Settings**, without a cable, with rollback when a new version does not start properly.

These need internet through your home Wi-Fi; each is off until you turn it on:

- Sending your readings to online weather services.
- Airport weather.
- Weather Underground's forecast for tomorrow.

## Being worked on next

- **Station export and import.** Today you can save one station's settings to a file and load them again. Next comes one file with the history, the learned values and the settings, so a board can be rebuilt from it, for example before reinstalling.
- **More languages.** A translator's guide, so anyone can add a language to the dashboard and Settings.
- **Firmware files per language.** When there are more languages than fit in one firmware file, the release offers a choice.
- **More consoles.** Each [console report](report-a-console.md) can become a profile on the Compatibility page. A model is listed as working only after someone has run it.

## Wanted later

- **Wi-Fi from the installer.** Set the home Wi-Fi from the browser installer right after flashing (Improv Wi-Fi), instead of through the setup Wi-Fi.

## Have an idea

Start a discussion on [GitHub](https://github.com/fauxcloud/fauxcloud/discussions), or read [Contributing](../CONTRIBUTING.md) if you want to help build it.
