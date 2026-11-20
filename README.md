# NorseHUD

The shared on-screen layer of the Norse overhaul (NORSE: Oath of Blood, UE4SS v4.0.0): health bars over units (red
enemies, green party), a boss bar at the top, and a marker over NorseRealtimeNative's lock-on target. Modelled on
TrueHUD. Survey: `RESEARCH-more-frameworks.md` (#7). Screenshot: `docs/shot-6-fixed.png`.

## How it draws

Norse's HUD never fires Unreal's ReceiveDrawHUD Blueprint event (0 calls in 2 s), so NorseHUD hooks AHUD::DrawHUD
itself: Norse Address Library ID 10 (no reflected name - found offline as the one function reading the
ReceiveDrawHUD name, with `NorseAddressLibrary/tools/nal_strrefs.py` and `nal_callers.py`). Its vtable slot (251 on
this build) is looked up in the engine HUD at run time and the live HUD class's function in that slot is hooked; after
it returns, the HUD's Canvas is valid and NorseHUD calls the HUD's DrawRect / DrawText. Screen positions come from
GameplayStatics::ProjectWorldToScreen with the HUD's PlayerOwner - the camera the player actually sees (AHUD::Project
used the canvas's own view, which is not NorseRealtimeNative's follow camera). Nothing cooked, nothing in the paks.

Cost: the unit list, team, boss flag, name and the bar condition are refreshed once a second; per frame, per shown
unit, its location, one projection and two DrawRects.

## Settings (NorseHUD.ini)

`sBarCondition` (NorseConditions language, default `InFight AND NOT IsDead`), `fBarWidth`, `fBarHeight`, `fBarZ`
(height above the unit's middle, default 115 cm - just over the head), `fMaxDistance`, `bBossBar` (a living boss in
the fight: its unit type's bIsBoss), `bLockMarker` (NorseRealtimeNative's `NRN_LockTarget` export).

## Console

In the UE4SS console: `nhud.status` writes the settings, the HUD hook and unit counts to NorseHUD.log; `nhud.set <key> <value>` changes one NorseHUD.ini key for this session (the file is not changed); `nhud.reload` reads NorseHUD.ini again.

## Status

0.1.0 tested in game (TEST-RESULT-0.1.0.txt, five launches). Not yet: the game's own UI style (the bars are plain
rectangles), damage numbers, stamina / poise bars (they arrive with those modules), and hiding bars of units out of
sight. Build: `xmake f -p windows -a x64 -m release --toolchain=msvc -y`, then `xmake` (needs `../NorsePatcher`,
`../NorseManifest`, `../NorseAddressLibrary`; MinHook in `third_party/`).

## Licence

GPL-3.0-or-later (see LICENSE). MinHook (BSD 2-Clause), compiled into the DLL, keeps its own notice in THIRD-PARTY-NOTICES.txt.
