# Wyze Sense v1 Contact Board Test Pads

The photo circles each pad in its signal's colour; the wiring diagrams under `docs/img/` use the same colours.

The pads are on the battery side of the board (battery out). Labels are printed next to most of them.

![The v1 contact's pad side, each pad circled in its signal's colour](../img/contact_v1.jpg)

| Pad | Colour | Signal | Used for |
|---|---|---|---|
| `V+` | pink | 3.3 V supply | power from the LaunchPad or the ESP (battery out) |
| `V−` / `VBAT−` | dark red | ground | common ground |
| `M` | purple | cJTAG TMSC | 2-pin debug port (`cjtag`) |
| `C` | red | cJTAG TCKC | 2-pin debug port (`cjtag`) |
| `S` | green | reset (`RESET_N`) | hardware reset (`optional` if `V+` is on GPIO) |
| `R` | grey-blue | serial input & bootloader backdoor (`DIO1`) | ESP UART TX & ROM bootloader entry |
| `T` | yellow-green | serial output (`DIO0`) | ESP UART RX |
| `W` | blue | `DIO10` (factory test input, pulled up) | unused on the relay |

A second green pad (bottom left) and a second dark-red pad (`VBAT−`, top right) carry the same signals as `S` and `V−`; use whichever is easier to reach.

The reed switch (the glass tube along the bottom edge, under the board) connects `DIO7` to ground when a magnet is near. The button is on `DIO11` and the LED on `DIO9`; neither has a test pad.
