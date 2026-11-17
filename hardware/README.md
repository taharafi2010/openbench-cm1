# OpenBench CM1 colorimeter: print files

The printable parts for OpenBench CM1, an open-source double-beam colorimeter. It is two stacked modules joined by two short legs:

- **Bottom module (control):** OLED screen, rotary encoder, and the electronics on a perfboard. Its roof is sealed against spills.
- **Top module (optics):** white LED, red gel filter, cuvette and two photodiodes. The sample photodiode sits behind the cuvette; the reference photodiode views the LED directly.

Every file is a single part, **already oriented for printing** (flat on the bed, correct side down) and centred. Load it as is. The files carry no colour, so pick filament in your slicer.

## Parts

| Folder | File | Qty | Print orientation | Notes |
|---|---|---|---|---|
| `1_bottom_module` | `bottom_shell.3mf` | 1 | roof down | USB-C opening in the right side |
| | `bottom_base_plate.3mf` | 1 | flat | perfboard standoffs, countersunk screw holes, foot pockets |
| | `screen_bezel.3mf` | 1 | face down | sits flush in the front pocket |
| | `oled_clamp.3mf` | 1 | pads up | holds the OLED module from behind |
| | `leg_boss_ring_x2.3mf` | **2** | top down | glued into the roof seats around each leg |
| `2_legs` | `leg_x2.3mf` | **2** | standing | use 4 walls; the wires run inside |
| `3_top_module` | `top_shell.3mf` | 1 | top face down | |
| | `top_base_plate.3mf` | 1 | flat | leg sockets, wire channels, core seat, drip nozzle |
| | `port_collar.3mf` | 1 | top face down | glued into the inner-lid opening |
| | `port_cap.3mf` | 1 | top face down | hinged light-trap cap over the cuvette |
| | `cap_hinge_block.3mf` | 1 | base down | glued into the pocket behind the collar |
| `4_optical_core` | `optical_core.3mf` | 1 | upright | LED, gel, cuvette and photodiode pockets |
| | `inner_lid.3mf` | 1 | roof down | LED hold-down post points up |

No supports are needed. Every part fits a 256 × 256 mm bed. All parts were designed in PLA.

## Filament: which parts must be opaque

The photodiodes measure nanoamps, and white or light PLA lets light through. **Print these in opaque black (or another dark, opaque filament):**

- `optical_core`, `inner_lid`, `port_collar`, `port_cap`, `cap_hinge_block`
- `top_shell` is recommended dark too; it helps keep stray light out.

The rest can be any colour. The reference build is two-tone:

- **White:** bottom shell, both base plates, OLED clamp, boss rings.
- **Black:** top shell, optics parts, screen bezel, legs.

## Other parts you need

| Item | Qty |
|---|---|
| M3 × 8 self-tapping countersunk screws | 12 (4 bottom plate, 4 top plate, 2 core, 2 OLED clamp) |
| Rubber feet, Ø10 mm | 4 |
| 1.75 mm filament, 14.4 mm long (cap hinge pin) | 1 |
| Clear PET sheet, 0.2–0.3 mm (2 spill windows) | small offcut |
| Red gel filter, about 12 × 16 mm | 1 |
| Black Blu-Tack (plugs the wire exits) | a little |
| Glue (CA or epoxy) | — |
| Arduino Nano, ADS1115, MCP6002 on a 50 × 70 mm perfboard | 1 |
| 1.3" SH1106 OLED module (glass 33 × 18 mm) | 1 |
| Rotary encoder with knob (7.2 mm panel hole) | 1 |
| White 5 mm LED + 150 Ω resistor | 1 |
| Photodiodes (face 7 × 5 mm, body 2 mm thick) | 2 |
| 28 AWG wire | 6 runs |
| Standard cuvette, 12.5 × 12.5 × 45 mm | — |

## Assembly (short version)

1. **Wire** the LED (with its resistor) and both photodiodes, and twist each photodiode pair.
2. **Thread the wires** up through the legs:
   - left leg: sample photodiode pair + LED+;
   - right leg: reference photodiode pair + LED−.
3. **Glue the boss rings** into the bottom-module roof and let them cure. Then glue the legs into the rings and into the top base plate's sockets.
4. **Fit the optical core** in its seat on the top base plate (2 × M3 from below). Then fit:
   - the LED, against its shoulder;
   - the gel (trimmed to ≤ 16 mm tall);
   - a PET window in each of the two window slots;
   - each photodiode, face first against its hole, with its pins and glue in the narrow slot behind.
5. **Plug the wire exits** on the core's sides with black Blu-Tack.
6. **Fit the inner lid**: its post presses the LED down. Glue the port collar into the lid opening.
7. **Fit the top shell** and screw the top base plate on (4 × M3 from below). Then fit the cap:
   1. Glue the hinge block in behind the collar.
   2. Seat the cap with its knuckle between the lugs.
   3. Push the filament pin through.
   4. Glue the pin at the knuckle only.

   To open the cap, lift it straight up, then flip it back.
8. **Fit the OLED** behind the bezel and screw on the clamp. Fit the encoder, the perfboard and the bezel.
9. **Screw on the bottom base plate** (4 × M3) and add the rubber feet.

## Licence

SPDX-License-Identifier: CERN-OHL-S-2.0

Copyright 2026 Taha Rafi.

This source describes Open Hardware and is licensed under the CERN-OHL-S v2.

You may redistribute and modify this source and make products using it under the terms of the CERN-OHL-S v2 (https://ohwr.org/cern_ohl_s_v2.txt).

This source is distributed WITHOUT ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING OF MERCHANTABILITY, SATISFACTORY QUALITY AND FITNESS FOR A PARTICULAR PURPOSE. Please see the CERN-OHL-S v2 for applicable conditions.

Source location: https://github.com/taharafi2010/openbench-cm1 (project site: https://openbench.oesimracing.com)

As per CERN-OHL-S v2 section 4, should You produce hardware based on this source, You must, where practicable, maintain the Source Location visible on the external case of the colorimeter or other products you make using this source.

The full licence text is in [`LICENSES/CERN-OHL-S-2.0.txt`](../LICENSES/CERN-OHL-S-2.0.txt).
