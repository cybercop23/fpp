; Set the per-GPIO-bank LED mask registers from the generated pin
; configuration.  Included by both FalconMatrix.asm and (in split output
; mode) FalconMatrixPRUCpy.asm; the including file must define the
; gpio0..3_led_mask registers and the CONFIGURE_PIN/GPIO_MASK macros.
#ifndef NO_OUTPUT_1
    CONFIGURE_PIN(r11)
    CONFIGURE_PIN(g11)
    CONFIGURE_PIN(b11)
    CONFIGURE_PIN(r12)
    CONFIGURE_PIN(g12)
    CONFIGURE_PIN(b12)
#endif
#ifndef NO_OUTPUT_2
    CONFIGURE_PIN(r21)
    CONFIGURE_PIN(g21)
    CONFIGURE_PIN(b21)
    CONFIGURE_PIN(r22)
    CONFIGURE_PIN(g22)
    CONFIGURE_PIN(b22)
#endif
#ifndef NO_OUTPUT_3
    CONFIGURE_PIN(r31)
    CONFIGURE_PIN(g31)
    CONFIGURE_PIN(b31)
    CONFIGURE_PIN(r32)
    CONFIGURE_PIN(g32)
    CONFIGURE_PIN(b32)
#endif
#ifndef NO_OUTPUT_4
    CONFIGURE_PIN(r41)
    CONFIGURE_PIN(g41)
    CONFIGURE_PIN(b41)
    CONFIGURE_PIN(r42)
    CONFIGURE_PIN(g42)
    CONFIGURE_PIN(b42)
#endif
#ifndef NO_OUTPUT_5
    CONFIGURE_PIN(r51)
    CONFIGURE_PIN(g51)
    CONFIGURE_PIN(b51)
    CONFIGURE_PIN(r52)
    CONFIGURE_PIN(g52)
    CONFIGURE_PIN(b52)
#endif
#ifndef NO_OUTPUT_6
    CONFIGURE_PIN(r61)
    CONFIGURE_PIN(g61)
    CONFIGURE_PIN(b61)
    CONFIGURE_PIN(r62)
    CONFIGURE_PIN(g62)
    CONFIGURE_PIN(b62)
#endif
#ifndef NO_OUTPUT_7
    CONFIGURE_PIN(r71)
    CONFIGURE_PIN(g71)
    CONFIGURE_PIN(b71)
    CONFIGURE_PIN(r72)
    CONFIGURE_PIN(g72)
    CONFIGURE_PIN(b72)
#endif
#ifndef NO_OUTPUT_8
    CONFIGURE_PIN(r81)
    CONFIGURE_PIN(g81)
    CONFIGURE_PIN(b81)
    CONFIGURE_PIN(r82)
    CONFIGURE_PIN(g82)
    CONFIGURE_PIN(b82)
#endif
