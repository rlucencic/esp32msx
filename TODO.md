# esp32msx roadmap

Proposed work in priority order. See [Development](README.md#development) for the current project.

1. **Exercise real MegaROM and SCC workloads.** Use a banked non-SCC title and
   a Konami SCC title with `components/msx/cart.c` as the mapper reference. Log
   selected mapper and bank transitions, capture a repeatable gameplay point
   and check joystick response and speaker sound on both boards.

2. **Verify V9938 commands in a scrolling workload.** Replay an MSX2 title or
   diagnostic that combines bitmap scrolling, sprite overflow and logical
   blits. Compare pictures and command completion points with the host. Profile
   frame-time tails on the MaTouch before changing VDP kernels.

3. **Test the RP5C01 around network time sync.** Use `components/msx/rtc.c` to
   test BCD reads while board time predates 1980, a guest date set before sync,
   and the first valid network time. Check date, weekday and 12/24-hour
   behavior with a host fixture and a board boot.
