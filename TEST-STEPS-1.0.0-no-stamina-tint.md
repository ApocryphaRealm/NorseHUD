# NorseHUD 1.0.0 - the ability strip's red "no stamina" tint - test steps

Builds: NorseHUD 1.0.0 as recorded working (f86c, unchanged); NorseRealtimeNative 1.0.0 repo main.dll with the test-kit command
"nrn.ability stamina <value> [hold s]" (sets the hero's stamina, no refill for hold + 1 s). Profile needs: none.
Map: /Game/Norse/Maps/Missions/Side/SeasideSkirmish/SideMission_SeasideSkirmish; vpad plugged in after the game starts.

1. Launch, title, open, fight (mission-drive.sh). EXPECTED NorseHUD.log "STRIP: ... X Hammer ready".
2. nrn.ability stamina 10 6. EXPECTED NRN "nrn.ability stamina: 10 of 100, no refill for 7.0 s"; NorseHUD "STRIP: ... X Hammer no
   stamina"; frame: the LT+X box red-tinted, the Hammer icon greyed.
3. vpad LT + X (press l2+square 150). EXPECTED NRN "STAMINA: 10 - not enough (30 needed)", no ABILITY line; tint stays.
4. Wait 10 s. EXPECTED NorseHUD "X Hammer ready" once stamina is back over 30; frame: no tint, bright icon.
Killing condition: falsified if the strip shows no red tint while NRN reports stamina below the cost, or the tint stays once it is ready.
