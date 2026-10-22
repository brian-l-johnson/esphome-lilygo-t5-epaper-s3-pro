# Deep sleep implementation plan

Goal: take a battery-powered portable remote from "12-15 hours standby" to
"weeks to months standby" by sleeping the ESP32-S3 between interactions.

## Power budget (current)

| State | Current | Source |
|---|---|---|
| Active (WiFi connected, display drawing) | ~85 mA | ESP32 + RF + LCD bus + TPS65185 |
| Active idle (WiFi connected, display idle) | ~75 mA | ESP32 + RF only; TPS off between refreshes |
| Deep sleep (target) | ~50-150 μA | ESP32-S3 RTC + GT911 standby + LDO leakage |

1500 mAh battery:
- Always-on: ~17 h standby
- Hourly wake (5 s active per hour, sleep otherwise): ~340 days
- Hourly + touch wake (assume 20 touches/day at 10 s each): ~280 days
- Daily GL16 wake (extra 5 s/day): ~negligible

Even aggressive wake schedules give months. The goal is "don't drain by morning."

## Wake sources

| Source | Trigger | Use |
|---|---|---|
| **Touch** | GT911 INT goes LOW on touch | Primary interaction |
| **Timer (RTC)** | ESP32-S3 RTC timer every 1 h | Battery indicator + connection check |
| **Timer (RTC)** | At 03:00 daily | GL16 full refresh to clear DU ghosting |
| **Charger plug-in** | TBD — see "Charger wake" below | Auto-clock mode entry |
| **S3 button** | PCA9535 INT goes LOW on press | Manual wake / forced GL16 / debug |

GT911 INT is on **GPIO38** (shared with PCA9535 INT). For deep sleep wake we
need an EXT0 or EXT1 source on a RTC-capable GPIO. GPIO38 is *not*
RTC-capable on ESP32-S3 — only GPIO0-21 are.

This is the first major problem to solve. Options:
1. **Reroute GT911 INT to a low-numbered GPIO via wire mod** (invasive, no-go).
2. **Use a GPIO matrix bridge** to mirror GPIO38 onto an RTC-capable GPIO during
   sleep — needs experimentation, not sure it works while CPU is in sleep.
3. **Use ULP-RISCV co-processor** to poll GT911 INT on the non-RTC pin and
   wake the main CPU. Complex but doable.
4. **Use light sleep instead of deep sleep** — keeps all GPIOs active, current
   draw is ~5-10 mA instead of <150 μA. Still 5-10× better than always-on.

**Recommended first iteration: light sleep.** Trades some power for huge
simplification — every wake source works without ULP gymnastics. Verify the
flow end-to-end. Move to deep sleep + ULP polling as a phase 2 optimization
if light sleep numbers don't meet the goal.

## Charger wake

If we want "plug into Qi pad → wake to clock face" the BQ25896's PG (Power
Good) status must reach the ESP32 on a wake-capable pin. Check the schematic
for whether PG is wired to a GPIO. If not, plug-in detection happens at the
next timer wake (worst case 1 h delay) — acceptable.

Alternative: always wake every 5 minutes specifically to poll
`charger_present()`. Light sleep makes this cheap.

## State preservation

PSRAM contents are lost in deep sleep but preserved in light sleep. Things
to think about either way:

| State | Where it lives now | Survives deep sleep? | Survives light sleep? |
|---|---|---|---|
| epdiy front_fb / back_fb | PSRAM | No → re-init on wake | Yes |
| display image | E-paper itself (physical) | Yes (e-paper retention) | Yes |
| active_room global | Flash (restore_value: true) | Yes | Yes |
| was_on_charger global | RAM (restore_value: false) | No | Yes |
| previous_room global | RAM (restore_value: false) | No | Yes |
| Optimistic UI slots | T5S3Epaper member | No | Yes |
| Battery cache | T5S3Epaper member | No | Yes |
| HA connection | RAM + sockets | No → reconnect | Reconnect after wake |
| WiFi connection | RF | No → reconnect | Possibly preserved (modem sleep) |

For deep sleep:
- Change `was_on_charger`, `previous_room` to `restore_value: true`.
- Save current optimistic-slot state to RTC slow memory (16 KB available).
- Re-init epdiy + render fresh on wake.
- Show "..." spinner during reconnect — first frame after wake should be
  the persisted image; render the spinner overlay; render fresh content
  once HA reconnects.

For light sleep:
- All state survives. Just resume.

## Sleep policy

Don't sleep when:
- On charger (always-on; user expects clock/remote to be live)
- WiFi/HA disconnected (reconnect first)
- OTA in progress
- Within N seconds of a touch event (N = 60? user might be reading)
- A refresh is in flight

Sleep when:
- All of above are false
- Idle for `sleep_after_idle_ms` (default: 60 s of no touch + no HA event)

Wake stays awake for:
- `min_wake_duration` (default: 30 s) — long enough to render, sync, accept
  a follow-up touch
- Reset to full duration on each new touch / HA event

## ESPHome integration

ESPHome has a [`deep_sleep:` component](https://esphome.io/components/deep_sleep.html)
that handles the basics. We'd add:

```yaml
deep_sleep:
  id: sleep_ctl
  wakeup_pin: GPIO38   # GT911 INT (shared)
  wakeup_pin_mode: KEEP_AWAKE
  run_duration: 60s    # min stay-awake after wake
  sleep_duration: 1h   # hourly wake for battery/health check
```

But the `wakeup_pin: GPIO38` only works if GPIO38 is RTC-capable — and it
isn't on ESP32-S3. So we either:
- Use the ESPHome light_sleep alternative (not currently a component — would
  need custom `esp_light_sleep_start()` calls in the component)
- Or hardware-mod GPIO38 → an RTC GPIO

**For an MVP, use the existing `deep_sleep:` component with whatever pin we
can make work**, even if it means losing touch wake initially and only
having timer + S3-button wake. Then iterate.

## Implementation phases

### Phase 1 — Scaffolding (no power savings yet)

1. Add `deep_sleep:` component to office-remote.yaml, disabled by default.
2. Expose a `switch.deep_sleep_enabled` so we can flip it without reflashing.
3. Add `prevent_sleep_until_ms_` member on T5S3Epaper. `notify_touch()`
   already exists — extend it to also defer sleep.
4. Add a 5 s interval that calls `id(sleep_ctl).allow_deep_sleep()` only
   when policy says we should sleep.
5. Persist `was_on_charger` and `previous_room` (restore_value: true).
6. Persist optimistic-slot state to RTC slow memory using
   `RTC_DATA_ATTR` (ESP-IDF native, callable from the component).

### Phase 2 — Verify wake / power flow

1. Flash + power-meter measurements at each state (active, idle, sleeping).
2. Configure wakeup_pin per available options. Verify each wake source
   actually fires (touch, timer, S3 button).
3. Verify post-wake display state — e-paper should hold last frame.
4. Test the wake-then-immediately-rerender path. Should feel near-instant
   for touch wake (DU is 180 ms; if we can skip WiFi reconnect by using
   cached state, even better).

### Phase 3 — Sleep policy tuning

1. Add the 60 s "after touch" holdoff.
2. Add charger-aware suppression (don't sleep when on charger).
3. Add HA-disconnected suppression (don't sleep until HA reconnects, so we
   don't sleep with a stale display).
4. Add OTA-in-progress suppression.

### Phase 4 — PCF85063 RTC integration (separate task)

The external RTC keeps absolute time accurate across deep sleep without
needing an immediate NTP resync. Useful for clock-on-charger and for
ghost-clearing wakes at 03:00 every day.

- Add PCF85063 driver to the component (3 registers for seconds/minutes/hours,
  I2C reg 0x04-0x06).
- Sync from NTP on first boot and once a day.
- Use the RTC's internal interrupt (INT pin) as a wake source if it's
  connected to an RTC-capable GPIO — check schematic.

### Phase 5 — Edge cases

1. Wake during OTA flash — must abort sleep.
2. Multiple consecutive wakes (timer fires while user is mid-touch).
3. First boot after flash — don't sleep until we've done a GL16.
4. Low-battery handling — if pct < 5%, just sleep and don't wake on touch.

## Testing checklist

- [ ] Active power draw measurement (baseline)
- [ ] Light sleep power draw
- [ ] Deep sleep power draw (if used)
- [ ] Touch wake latency
- [ ] Timer wake fires after sleep_duration
- [ ] Display state preserved across sleep
- [ ] Optimistic UI slots survive sleep (restored from RTC mem)
- [ ] HA reconnects within 5 s of wake
- [ ] Charger plug-in wakes (or first timer fires within 5 min)
- [ ] No regression in normal interactive use
- [ ] No issues with OTA flashing while sleep is configured
- [ ] 24-hour soak test — wake/sleep cycle stable for at least 100 cycles
- [ ] Battery: does it last at least 5× longer than baseline always-on?

## Open questions

1. **Is GPIO38 really not RTC-capable on ESP32-S3?** Datasheet says only
   GPIO0-21 are RTC GPIOs. But the EXT1 wake source can use any RTC GPIO,
   and we need to confirm GT911 INT can't somehow be wired up. Worth
   verifying with `gpio_is_valid_rtc_gpio(GPIO_NUM_38)`.
2. **Does the GT911 need to stay powered in sleep?** If we power it down
   to save more current, we lose touch wake. Probably needs its rail
   verified — if VDD0V3 (the rail it shares) is left on, we're fine.
3. **Does TPS65185 leak in standby?** If `epd_poweroff()` properly shuts
   it down (it should — that's its job), no problem. Verify by measurement.
4. **PSRAM in deep sleep:** does it auto-power-down, or do we need to
   explicitly de-init? The board has `CONFIG_SPIRAM_LEAKAGE_WORKAROUND`
   enabled in the generated sdkconfig — promising.
5. **WiFi reconnect time** — if it's 3-5 s every wake, that hurts touch
   latency for hourly checks. Might want WiFi off entirely between
   touches and only reconnect for HA sync.

## Estimated effort

| Phase | Hours |
|---|---|
| 1 — Scaffolding | 2 |
| 2 — Wake/power flow verification | 2-4 (depends on light vs deep sleep) |
| 3 — Policy tuning | 1 |
| 4 — PCF85063 RTC | 2-3 |
| 5 — Edge cases | 1-2 |
| Testing + soak | 2 |
| **Total** | **10-14 hours** |

Best done in a session where the device can be on a power meter and OTAs
can be tested without bricking. Don't start this on a workday when "it has
to keep working as a remote."
