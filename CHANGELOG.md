# Changelog

## 0.1.0 - 2026-11-17 (tested in game: TEST-RESULT-0.1.0.txt)
### Added
- Health bars over units (sBarCondition, NorseConditions), a boss bar, a marker over the lock-on target - drawn on the HUD canvas after AHUD::DrawHUD (NAL ID 10, vtable slot found at run time).
- Positions from GameplayStatics::ProjectWorldToScreen (the player camera).

### Fixed
- A crash after a map change or when a unit was destroyed: the unit list (refreshed once a second) kept pointers to destroyed units and read their health. Each unit is now checked against the object array before every read (found by NorseEncounters 0.1.0 removing a unit, 2026-11-17).
