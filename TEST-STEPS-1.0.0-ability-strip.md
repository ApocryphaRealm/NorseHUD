# NorseHUD 1.0.0 - ability strip (controller-menus build list item 4) - test steps

Builds: NorseHUD 1.0.0 (ability strip) + NorseRealtimeNative 1.0.0 (NRN_GetAbilitySlots / NRN_GetStamina / NRN_LtHeld exports).
Map: /Game/Norse/Maps/Missions/Side/SeasideSkirmish/SideMission_SeasideSkirmish. Pad: vpad, plugged in after the game starts.

1. Launch, mission-drive.sh title, open. EXPECTED: NorseHUD.log has "STRIP: NorseRealtimeNative's NRN_GetAbilitySlots found";
   in free roam no "STRIP: shown" line and no strip on screen (no real-time fight).
2. mission-drive.sh fight. EXPECTED: "STRIP: shown at (x, y), 5 slots", a "STRIP: LB ...; X ...; Y ...; A ...; B ..." state line
   naming the hero's unlocked abilities (an empty control shows "-"); a frame shows five boxes at the bottom centre with LT+LB /
   LT+X / LT+Y / LT+A / LT+B labels, icons and names. "STRIP: N WBP_AbilityBar_C widget(s)" line: any visible one collapsed;
   no turn-based ability bar on the frame.
3. vpad: hold l2 (LT) 1.5 s with a frame in the middle. EXPECTED: gold frames on the boxes, "[LT held]" state line.
4. nrn.ability tofoe, nrn.ability reset, vpad "press l2+square 150" (LT + X). EXPECTED: NorseRealtimeNative.log "ABILITY: <name>
   (LT+X, ...)" fired; NorseHUD.log "STRIP: ... X <name> 12 s" then the seconds counting down about once a second to "ready";
   frames at +1 s, +4 s, +8 s show the dark shutter shrinking and the seconds falling on the X box.
5. nrn.killhostiles. EXPECTED: "STRIP: hidden (no real-time fight)" and any collapsed bar given back; no strip on the frame.
