
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```

## Infrared remote (AI controllable)

The firmware drives the body's IR transmitter (GPIO5) and receiver (GPIO10) through the RMT peripheral
(`main/hal/ir/`). Learned buttons are stored by name in NVS (namespace `ir_codes`) and exposed to the AI
agent as MCP tools:

| Tool | Purpose |
| --- | --- |
| `self.ir.list_codes` | List learned buttons |
| `self.ir.learn_code` | Record a button from a real remote (LEDs: blue = listening, green = saved, red = failed) |
| `self.ir.get_learn_status` | Result of the last learn |
| `self.ir.send_code` | Send a learned button (`repeat` 1-10) |
| `self.ir.delete_code` | Delete a learned button |
| `self.ir.send_nec` | Send a NEC address/command directly |

Example: "learn my TV power button as tv_power", then "turn off the TV".
Codes are captured raw (up to ~1000 pulses, replayed at 38 kHz), so long air-conditioner frames work as a fixed
snapshot of the state that was recorded. The NVS partition is 16 KB and shared, so keep the number of
long codes moderate.
