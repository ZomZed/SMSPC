# Episodes left for a human playtest (w12)

These were not finished by scripted input (see BOOTLOG "Episode objectives (w12)").
Start each with `python port/tools/build_aurora.py --run --pads --warp <area>,<scenario>`.
The warp starts on a fresh save in the test user folder (`SMS_PORT_USER_DIR`), so the real memory card is not touched.
For each one: get the Shine, answer the save prompt, and check that you are back in Delfino Plaza.

| Episode | Warp | What to watch for |
|---|---|---|
| Ricco Harbor 6: Red Coins on the Water | `3,5` | Ground-pound the red switch on the dock. The 2:00 timer and the 0/8 counter should appear (M4 fix). Ride the red Blooper next to it. One coin floats at height 400 near (3700, 6600) and needs a surf jump (A). Crashing head-on into a wall, boat or buoy kills Mario; that is the original game. The Shine appears near the switch. |
| Pianta Village 2: Il Piantissimo's Crazy Climb | `8,1` | Stand still within about 2 Mario-lengths of Il Piantissimo to start (Ready, Go). A scripted run reached the flag in 45.9 s and still lost, so the shortcuts matter. Losing plays "You are nothing", costs a life and returns to the plaza; that is the original script. |
| Bianco Hills 3: The Hillside Cave Secret | `2,2` | Enter the secret course, then play it to the Shine (no FLUDD). |
| Bianco Hills 4: Red Coins of Windmill Village | `2,3` | 8 red coins on the rooftops. The Shine appears on top of the windmill. |
| Ricco Harbor 3: The Caged Shine Sprite | `3,2` | The caged Shine is on the central tower at (-5000, 2550, 7650). |
| Ricco Harbor 4: The Secret of Ricco Tower | `3,3` | Secret course entrance at the tower top, then the course to the Shine. |
| Gelato Beach 1: Dune Bud Sand Castle Secret | `4,0` | The sand castle entrance, then the secret course. |
| Gelato Beach 4: The Sand Bird is Born | `4,3` | Sand bird flight to the Shine. |
| Pinna Park 3: Red Coins of the Pirate Ships | `5,2` | Beach cannon to the park, then the red coins. |
| Sirena Beach 1: Manta Storm | `6,0` | Spray every split Manta. The Shine raises the hotel. |
| Sirena Beach 2: The Hotel Lobby's Secret | `6,1` | Secret course from the hotel lobby. |
| Noki Bay 1: Uncork the Waterfall | `9,0` | Clean the goop at the waterfall. |
| Noki Bay 2: The Boss of Tricky Ruins | `9,1` | Climb to the event Shine at (4902, 1300, 9241). |
| Pianta Village 1: Chain Chomplets Unchained | `8,0` | Cool the Chomplets with FLUDD and push them into the bath. |

Already checked by script (no need to replay): the Delfino Plaza boathouse blue coins to Shine trade (warp `1,8`, 10 blue coins), and opening the guide map (Z) in 11 stages.
