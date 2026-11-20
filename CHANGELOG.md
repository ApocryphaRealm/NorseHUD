# Changelog

## 1.0.0 - 2026-11-18 - working

### Added
- Ability strip in real-time fights (bAbilityStrip, fStripSize, fStripBottom): five boxes at the bottom centre for LT + LB / X / Y / A / B with each ability's own icon and name, a dark shutter and the seconds left while it cools down, a red tint without enough stamina, gold outlines while LT is held. Reads NorseRealtimeNative's NRN_GetAbilitySlots / NRN_LtHeld. The game's turn-based ability bar (it was still visible in real-time fights) is collapsed while the strip shows and given back at the fight's end. Proven on SideMission_SeasideSkirmish with a real pad (TEST-RESULT-1.0.0-ability-strip.txt).
- Console commands: nhud.status (settings, the HUD hook and unit counts to the log), nhud.set <key> <value> (one NorseHUD.ini key for this session), nhud.reload (reads NorseHUD.ini again). Added after 1.0.0 was recorded working; the 1.0.0 test package is rebuilt and needs a new load check.
- uLogLevel in NorseHUD.ini: 2 = info (the default), 0 = everything for a bug report.
- First release. It contains everything built and tested in 0.1.0, listed here and kept as history below:
- Health bars over units (sBarCondition, NorseConditions), a boss bar, a marker over the lock-on target - drawn on the HUD canvas after AHUD::DrawHUD (NAL ID 10, vtable slot found at run time).
- Positions from GameplayStatics::ProjectWorldToScreen (the player camera).
- main.dll carries a Windows version resource (file and product version 1.0.0, shown in its Properties > Details).

### Fixed
- A crash after a map change or when a unit was destroyed: the unit list (refreshed once a second) kept pointers to destroyed units and read their health. Each unit is now checked against the object array before every read (found by NorseEncounters 0.1.0 removing a unit, 2026-11-17).

## 0.1.0 - 2026-11-17 (tested in game: TEST-RESULT-0.1.0.txt)
### Added
- Health bars over units (sBarCondition, NorseConditions), a boss bar, a marker over the lock-on target - drawn on the HUD canvas after AHUD::DrawHUD (NAL ID 10, vtable slot found at run time).
- Positions from GameplayStatics::ProjectWorldToScreen (the player camera).

### Fixed
- A crash after a map change or when a unit was destroyed: the unit list (refreshed once a second) kept pointers to destroyed units and read their health. Each unit is now checked against the object array before every read (found by NorseEncounters 0.1.0 removing a unit, 2026-11-17).
