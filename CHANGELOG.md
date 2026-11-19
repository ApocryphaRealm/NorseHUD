# Changelog

## 1.0.0 - 2026-11-18 - working

### Added
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
