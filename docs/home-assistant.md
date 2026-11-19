---
title: Home Assistant
order: 60
lede: Your station shows up in Home Assistant as a device with wind, rain, temperature and pressure entities, and notices you can trigger automations on.
---

## What you need

- An MQTT broker on your home network. MQTT is a small message system that Home Assistant uses; the broker is the server that passes its messages. In Home Assistant, the **Mosquitto broker** add-on does the job; any other MQTT broker works too.
- Home Assistant's MQTT integration, set up to use that broker.
- **Home Assistant 2025.10 or later.** Older versions are not tested.

> [!NOTE]
> MQTT is optional. Without it the dashboard works as usual and nothing is published.

## Connect the broker

1. Open **Settings** (`http://fauxcloud.local/admin`) and sign in.
2. Under **Home Assistant and online services**, open the **MQTT** tile.
3. Fill in **Broker address** (for example `192.168.1.10` or `homeassistant.local`, without `http://`), **Port** (usually 1883), **User name** and **Password**. Use a Home Assistant user, or the user you made for the broker.
4. Press **Test connection**, then save.

   **You should see** "Connected. Save to keep these settings." After saving, the tile says **Connected**.

The station appears in Home Assistant after its first upload. If nothing shows up, check [Troubleshooting](troubleshooting.md).

> [!NOTE]
> **No encryption to the broker**
> fauxcloud sends to your broker without encryption, with your user name and password. Keep the broker on your home network. fauxcloud only publishes; it subscribes to nothing, so Home Assistant cannot send it commands.

## What appears

Home Assistant finds everything by itself (this is called MQTT discovery). fauxcloud uses the default discovery prefix, `homeassistant`. You get two kinds of device:

- **One device per station**. A station is fauxcloud's record of one console (your weather station's display unit). The device is named after the station (by default `fauxcloud <station id>`) and holds the measurement entities below.
- **One bridge device**, `fauxcloud bridge <XXXX>`, with the board's own state.

Values are published as plain text under `fauxcloud/<XXXX>/<station id>/<entity key>`. `XXXX` is the last four characters of the board's hardware (MAC) address, as in the Wi-Fi name `fauxcloud-XXXX`.

fauxcloud does not ask the broker to keep measurements (they are not retained). This is on purpose: a reading that fauxcloud rejected is simply not updated, and old values cannot outlive a broker restart.

### Station entities

Pressure, wind speed and wind gust are as the console reports them.

| Entity | Unit | What it is |
|---|---|---|
| Temperature | °C | Outdoor air temperature |
| Dew point | °C | Outdoor dew point |
| Humidity | % | Outdoor relative humidity |
| Pressure | hPa | Air pressure. It follows REL, the console's relative pressure setting (set to sea-level pressure) |
| Wind speed | km/h | Wind speed |
| Wind gust | km/h | Gust |
| Wind direction | ° | Direction the wind comes from |
| Rain (last hour) | mm | Rain over the last 60 minutes, worked out by fauxcloud |
| Rain today | mm | Rain since midnight |
| UV index | none | UV index |
| Solar radiation | W/m² | Solar radiation |
| Wind speed, 2-min average | km/h | Mean wind over the last 2 minutes, worked out by fauxcloud |
| Wind direction, 2-min average | ° | Mean direction over the last 2 minutes |
| Wind gust, 10 min | km/h | Strongest gust of the last 10 minutes |
| Wind speed, 10-min average | km/h | Mean wind over the last 10 minutes |
| Last seen | time | When the last reading arrived |
| Outdoor sensor link | on / off | On means the outdoor sensor is not updating |
| Battery low | on / off | On means the console reports a low battery |
| Notices | text | What needs a look, in plain words |
| Local outlook | text | The [Local outlook](outlook.md) sentences |

The wind averages and the 10-minute gust are what you want for an awning rule that should ignore single spikes. The 2-minute average direction skips readings under 1 km/h.

- Some entities appear only when the console has sent the value.
- Entities for other values the console may send are added the same way: indoor temperature and humidity, wind chill, heat index, absolute pressure, rain this week, month and year, rain rate, gust direction, visibility.
- Entities you hide in a station's settings are removed from Home Assistant. They come back when you un-hide them.
- A station's **Diagnostic** group also holds seven counters named **Rejected ...**. They count readings that the filters dropped.

fauxcloud suggests entity IDs made from the station ID and the entity key, for example `sensor.fauxcloud_st1_temp_c` and `binary_sensor.fauxcloud_st1_sensor_stale`. Home Assistant uses the suggestion only when it first creates the entity.

### Bridge entities

All of these are in the diagnostic group of the bridge device.

| Entity | What it is |
|---|---|
| IP address | The board's address on your home network |
| Wi-Fi signal | Signal strength in dBm |
| AP clients | How many devices are on the fauxcloud Wi-Fi |
| Free heap | Free memory in bytes |
| Last restart | When the board last started |
| Last reset reason | Why it last restarted |
| Restarts | How many times it has restarted |
| Firmware | The firmware version |
| Time source | Where the board's clock comes from |
| Notices | Notices about the board itself |

## Unavailable stations

A station shows as unavailable when it stops uploading. The default is **4 minutes** without an upload. To change it, open the station in **Settings** and pick a time under **Show as offline after**: 2, 4, 10 or 30 minutes. The console uploads about once a minute.

If fauxcloud stops, loses power or drops off the network, the broker marks the whole bridge offline and every entity becomes unavailable with it.

Three more settings sit in the **Publishing** tile and in each station's settings:

- **Send to Home Assistant** (per station): switch it off to keep a station on the dashboard only.
- **Send new stations to Home Assistant automatically**: switch it off to keep new stations on the dashboard until you turn them on one by one.
- **"Last seen" update in Home Assistant**: how often the last-seen time moves while a station is quiet (every minute, 5 minutes or 15 minutes). Readings are always sent as they arrive.

## Notices

The **Notices** entity says what needs a look: for example "Outdoor sensor not updating since 03:40." When all is well it says `OK`. Its attributes carry a list called `items` with one entry per notice.

Trigger automations on the notice **id**, never on its text. The ids stay the same; the wording can change between versions.

| Notice id | Where | What it means |
|---|---|---|
| `sensor_stale` | station | The outdoor sensor has not updated for a while. A calm night can do this; a long gap means its battery or radio |
| `sensor_absent` | station | The outdoor sensor is missing: check its batteries |
| `baro_uncalibrated` | station | The console's pressure is at its factory value; set REL in its SET menu |
| `key_mismatch` | station | An upload arrived with another key for this station. Accept it in Settings if you re-paired the console |
| `console_clock` | station | The console's clock is off by more than 10 minutes |
| `profile_changed` | station | The station's **Console model** setting changed (shown for 24 hours) |
| `baro_ref` | station | The pressure reads noticeably above or below nearby airports; see [Airport weather](airport-weather.md) |
| `baro_check` | station | The pressure has been off from the usual for this place for days |
| `time_approximate` | bridge | The time is estimated since the last restart |
| `time_none` | bridge | fauxcloud is still waiting for the real time |
| `heap_low` | bridge | Memory is low; readings still arrive |
| `store_fail` | bridge | History could not be saved; readings still arrive |
| `heap_frag_restart` | bridge | fauxcloud restarted itself to free memory (shown for 24 hours) |
| `store_layout` | bridge | The board's storage needs one USB flash before history can be saved |
| `clock_behind` | bridge | The clock is behind the saved history, so history is paused until the time is confirmed |

Each item has `id`, and where it applies `since` (when it began, in UTC), `n` (a count) or `skew_s` (the clock difference in seconds).

## Example automations

Both examples use a station with the ID `st1`. Replace it with yours.

```yaml title="Close the awning when the 10-minute average wind is strong"
automation:
  - alias: Awning in
    trigger:
      - trigger: numeric_state
        entity_id: sensor.fauxcloud_st1_wind_avg10m_kmh
        above: 35
    action:
      - action: cover.close_cover
        target:
          entity_id: cover.awning
```

```yaml title="An automation that triggers on a notice id"
automation:
  - alias: Outdoor sensor needs a look
    trigger:
      - trigger: template
        value_template: >
          {{ state_attr('sensor.fauxcloud_st1_notices', 'items')
             | selectattr('id', 'eq', 'sensor_stale') | list | count > 0 }}
    action:
      - action: notify.notify
        data:
          message: The outdoor sensor has stopped updating. Check its batteries.
```

> [!NOTE]
> `cover.awning` and `notify.notify` are placeholders for your own devices.

## Limits

- Verified on one console (the YT60234). Another console model can send other values; see [Compatibility](station-profiles.md).
- The Weather Underground forecast line is not published to Home Assistant.
- MQTT goes without encryption to a broker you run; see [Security](../SECURITY.md) for what that means.
