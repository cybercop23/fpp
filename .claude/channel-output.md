# Channel Output System (`src/channeloutput/`)

## Class Hierarchy

- **`ChannelOutput`** — Base class. Pure virtuals: `SendData()`, `GetRequiredChannelRanges()`. Virtuals: `Init()`, `Close()`, `PrepData()`, `StartingOutput()`, `StoppingOutput()`.
- **`ThreadedChannelOutput`** — Extends ChannelOutput with double-buffered async sending via pthread. Subclasses implement `RawSendData()`.
- **`SerialChannelOutput`** — Mixin for serial port management.
- **`UDPOutput`** — Singleton managing all UDP-based protocols (E1.31, DDP, ArtNet, KiNet, Twinkly). Socket pooling, `sendmmsg()` batching, background worker threads.

## Channel Output Plugins (41 `.so` plugins)

Plugins are loaded at runtime via `dlopen()` in `Plugins.cpp`. Each plugin exports a `createPlugin()` factory function. Configured via JSON in `config/channeloutputs.json`.

### Network Protocol Plugins

| Plugin | Protocol | Notes |
| --- | --- | --- |
| `libfpp-co-UDPOutput` | E1.31/DDP/ArtNet/KiNet/Twinkly | Unified UDP output, multicast, frame dedup |
| `libfpp-co-GenericUDP` | Custom UDP | Configurable packet format |
| `libfpp-co-MQTTOutput` | MQTT | RGB/RGBW publish, threaded |
| `libfpp-co-HTTPVirtualDisplay` | HTTP | Remote rendering |
| `libfpp-co-HTTPVirtualDisplay3D` | HTTP | 3D remote rendering |

### Pixel String Drivers

| Plugin | Hardware | Platform |
| --- | --- | --- |
| `libfpp-co-spixels` | SPiWare SPI pixels | RPi |
| `libfpp-co-SPIws2801` | WS2801 SPI | RPi/Linux |
| `libfpp-co-SPInRF24L01` | nRF24L01 wireless SPI | RPi/Linux |
| `libfpp-co-BBB48String` | 48-channel string cape | BBB |
| `libfpp-co-BBShiftString` | Shift register strings | BBB |
| `libfpp-co-DPIPixels` | DPI parallel RGB pixels | BBB |
| `libfpp-co-ModelPixelStrings` | Virtual pixel string models | All |

### Matrix/Panel Drivers

| Plugin | Hardware | Platform |
| --- | --- | --- |
| `libfpp-co-RGBMatrix` | rpi-rgb-led-matrix GPIO panels | RPi |
| `libfpp-co-BBBMatrix` | PRU-driven RGB matrix | BBB |
| `libfpp-co-FBMatrix` | Framebuffer (X11/KMS) | Linux/Mac |
| `libfpp-co-MAX7219Matrix` | MAX7219 SPI LED driver | SPI |
| `libfpp-co-ColorLight5a75` | ColorLight 5A-75 receiver | Multi |
| `libfpp-co-ILI9488` | ILI9488 TFT LCD (SPI) | RPi |
| `libfpp-co-X11Matrix` | X11 window (dev/demo) | Linux/Mac |
| `libfpp-co-X11PanelMatrix` | X11 panel layout viz | Linux/Mac |
| `libfpp-co-VirtualDisplay` | Abstract virtual display | All |

### Serial/DMX Protocols

| Plugin | Protocol |
| --- | --- |
| `libfpp-co-GenericSerial` | Generic serial (configurable baud, headers/footers) |
| `libfpp-co-Renard` | Renard serial |
| `libfpp-co-LOR` | Light-O-Rama serial |
| `libfpp-co-LOREnhanced` | Enhanced LOR |
| `libfpp-co-BBBSerial` | BeagleBone serial |
| `libfpp-co-USBPixelnet` | Pixelnet via USB |
| `libfpp-co-USBDMX` | USB DMX |
| `libfpp-co-UDMX` | uDMX (FTDI-based) |

### GPIO/I2C Expander Plugins

| Plugin | Hardware |
| --- | --- | --- |
| `libfpp-co-GPIO` | Direct GPIO (binary on/off, PWM) |
| `libfpp-co-GPIO-595` | 74HC595 shift register |
| `libfpp-co-MCP23017` | MCP23017 I2C 16-bit GPIO |
| `libfpp-co-PCF8574` | PCF8574 I2C 8-bit GPIO |
| `libfpp-co-PCA9685` | PCA9685 I2C 16-ch PWM servo |
| `libfpp-co-USBRelay` | USB relay module |

### Special

| Plugin | Purpose |
| --- | --- | --- |
| `libfpp-co-FalconV5Support` | Falcon V5 controller hardware |
| `libfpp-co-ControlChannel` | Control channel preset triggers |
| `libfpp-co-Debug` | Debug/test output (logs channel data) |

## Output Processor Pipeline (`src/channeloutput/processors/`)

Chain-of-responsibility pattern applied sequentially to channel data each frame. Configured per-output in JSON. Thread-safe via mutex.

| Processor | Purpose |
| --- | --- | --- |
| `RemapOutputProcessor` | Sparse channel remapping |
| `BrightnessOutputProcessor` | Global/per-model brightness + gamma curve |
| `ColorOrderOutputProcessor` | RGB byte reordering (RGB->BGR, etc.) |
| `ScaleValueOutputProcessor` | Linear brightness scaling |
| `ClampValueOutputProcessor` | Min/max value clamping |
| `SetValueOutputProcessor` | Force channels to fixed values |
| `HoldValueOutputProcessor` | Hold last value (interpolation) |
| `ThreeToFourOutputProcessor` | RGB->RGBW expansion |
| `OverrideZeroOutputProcessor` | Force zero channels to non-zero |
| `FoldOutputProcessor` | Bit depth reduction |

## String Testers (`src/channeloutput/stringtesters/`)

Test pattern generators for pixel string verification: `PixelCountStringTester`, `PixelFadeStringTester`, `PortNumberStringTester`.
