// The mode changes of SME: smstart, smstop, msr svcr. Only the host runs these (Unicorn has no SME).
// The runner of the host reads z / p (and za) back in the streaming mode, so a line that leaves the
// mode is a sequence that enters it again: a change of the streaming mode zeroes z, p, ffr and sets
// fpsr to 0x0800009f, a change of the za enable zeroes za and zt0; setting a bit which is set
// already changes nothing.
smstart                                  // @mode: za
smstart sm                               // @mode: za
smstart za                               // @mode: za
smstop sm ; smstart sm                   // @mode: za
smstop za ; smstart za                   // @mode: za
smstop ; smstart                         // @mode: za
smstop za ; smstart                      // @mode: za
smstop sm ; smstart za ; smstart sm      // @mode: za
smstop sm ; smstop za ; smstart sm ; smstart za  // @mode: za
smstop sm ; smstart sm ; smstop za ; smstart za  // @mode: za
smstart sm ; smstart za                  // @mode: sm
mrs x0, svcr                             // @mode: za
mrs x1, svcr                             // @mode: sm
msr svcr, x0                             // @mode: za @state: x0=3
msr svcr, x0 ; msr svcr, x1              // @mode: za @state: x0=1 x1=3
msr svcr, x0 ; msr svcr, x1              // @mode: za @state: x0=2 x1=3
msr svcr, x0 ; msr svcr, x1              // @mode: za @state: x0=3 x1=3
msr svcr, x0 ; msr svcr, x1 ; mrs x2, svcr  // @mode: za @state: x0=0 x1=3
rdsvl x0, #1                             // @mode: sm
rdsvl x3, #-4                            // @mode: za
addsvl x0, x1, #2                        // @mode: sm
addsvl sp, sp, #-2                       // @mode: za
addspl x4, x5, #3                        // @mode: sm
addspl sp, x2, #-32                      // @mode: za
rdsvl x1, #31                            // @mode: plain
addsvl x2, x3, #-32                      // @mode: plain
