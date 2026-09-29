# ESP32 Chatbot v1.0

Voice assistant firmware for ESP-VoCat v1.2, based on ESP-Brookesia release/v0.7.
It uses the local "Hi ESP" wake word, ACOS real-time conversation, voice interruption
during playback, and a 60-second idle timeout before wake word is required again.

The ESP-IDF project is in `examples/agent/chatbot`. The `agent`, `service`, `hal`,
`expression`, and `utils` directories contain local components required by that
project. Keep this directory layout when building. See
[`ACOS_BARGE_IN.md`](examples/agent/chatbot/ACOS_BARGE_IN.md) for behavior and
hardware test steps.

## Build

Use ESP-IDF v5.5.5 and select ESP-VoCat v1.2. In an ESP-IDF PowerShell terminal:

```powershell
cd D:\esp32_chatbot_v1.0\examples\agent\chatbot
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM6 flash monitor
```

The ESP32-S3 defaults select the VoCat v1.2 board, 16 MB flash, octal PSRAM,
and the Hi ESP wake model. In `menuconfig`, set the ACOS token under
`ESP-Brookesia: Agent Xiaozhi Configurations`. The generated `sdkconfig` is
local and ignored by Git; never commit or share it. Wi-Fi credentials are
provisioned on the device and stored in NVS.

This project incorporates ESP-Brookesia components by Espressif. See
[`license.txt`](license.txt) for the upstream license.
