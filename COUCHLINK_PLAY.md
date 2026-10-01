# CouchLink play build (`fix/vk-cb-chain-wait`)

Fork-only line for **jrb00013/rpcs3** — not upstream. Goal: one Windows
Release binary that delivers what co-play needs end-to-end.

## What this build must deliver

1. **Stay up under co-op present load** — Multithreaded RSX + heavy present
   (split-screen, multi-pad, friends) must not *present-freeze*.
   Vulkan CB reclaim/wait instead of reusing a still-pending command buffer
   (`CB chain has run out of free entries`).
2. **Stay up under RSX semaphore stalls** — never abort `nv406e::semaphore_acquire`
   into an unmatched semaphore (that softlocks FPS under shader/capture load).
3. **Stay up under SPURS pressure** — `sys_spu_thread_send_event` WRCH abort
   must not hang the SPU path (split-screen job spikes).
4. **Pads stay reliable** — HID pad-handler lock so DualSense/VHID enumerate
   and update without racing the pad thread (GUI / hotplug / friend seats).
5. **Debuggable crashes** — PDB artifact uploaded for symbolication.
6. **Optimized binary** — CI `Configuration=Release` (MSVC x64), same deploy
   path as stock Windows builds.

## Softlock recoveries in this build

1. **Vulkan CB reclaim** — never reuse a still-pending CB (present death).
2. **nv406e semaphore acquire** — do **not** abort into an unmatched semaphore
   when driver recovery timeout fires. Flush/recover and keep waiting. Aborting
   was softlocking MK (BLUS30522) and other titles under shader+capture load
   (`semaphore_acquire has timed out` → stuck FPS).
3. **VK `on_semaphore_acquire_wait`** — always `do_local_task` so GPU work that
   releases the semaphore can retire while we spin.

## What this build does *not* fix alone

**BO2 (BLUS31011) SPURS wait-loop softlocks** with Multithreaded RSX on —
stuck FPS title, runaway `sys_timer_usleep`, often when opening the in-game
settings menu. That is separate from Vulkan CB / semaphore recoveries. For BO2:
- Keep `Multithreaded RSX: false` in the per-game config
- Install couchlink `contrib/rpcs3-bo2-splitscreen` game patches:
  hang-detect NOP **and** `force SPURS wait complete then exit` (writes the
  completion counter before returning — required for 4-pad split-screen)
Stage this binary anyway so present-death / MK semaphore softlock stay fixed.

## Commits on this line

| Area | Change |
|------|--------|
| Vulkan | CB ring 1024; scan free → flush-all retry → bounded wait; never return pending CB |
| RSX | semaphore_acquire: recover+wait instead of TDR abort softlock; VK always pumps on wait |
| SPURS | Fix hang when send_event WRCH is aborted |
| HID | Serialize Init / update_devices / list_devices / get_hid_device |
| CI | Upload `rpcs3.pdb` as its own artifact |

## Stage on the gaming PC

When Windows CI is green:

```bat
REM from C:\Users\josep\RPCS3 — after closing rpcs3.exe
switch-rpcs3.cmd couchlink-play
```

(Alias of the staged `20078-cbwait` / this branch artifact.)

BO2 per-game config: `Multithreaded RSX: false`, `Frame limit: Auto`.

## Not in this binary

Game patches (hang-detect NOP) and YAML overrides live in
`couchlink/contrib/rpcs3-bo2-splitscreen` — install those separately.
