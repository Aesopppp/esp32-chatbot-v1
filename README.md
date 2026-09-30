# ESP32 Chatbot v1.0

Voice assistant firmware for ESP-VoCat v1.2, based on ESP-Brookesia release/v0.7.
It uses the local "你好小益" wake word, ACOS real-time conversation, voice interruption
during playback, and a 60-second idle timeout before wake word is required again.

The ESP-IDF project is in `Robot_Assistant/agent/chatbot`. The `agent`, `service`, `hal`,
`expression`, and `utils` directories contain local components required by that
project. Keep this directory layout when building. See
[`ACOS_BARGE_IN.md`](Robot_Assistant/agent/chatbot/ACOS_BARGE_IN.md) for behavior and
hardware test steps.

## Build

Use ESP-IDF v5.5.5 and select ESP-VoCat v1.2. In an ESP-IDF PowerShell terminal:

```powershell
cd D:\esp32_chatbot_v1.0\Robot_Assistant\agent\chatbot
idf.py set-target esp32s3
idf.py gen-bmgr-config -b esp_vocat_board_v1_2
idf.py menuconfig
idf.py build
idf.py -p COM6 flash monitor
```

The board-generation command creates local source files under `components/gen_bmgr_codes`
and may move aside `sdkconfig`; configure the ACOS token afterward. The ESP32-S3
defaults select the VoCat v1.2 board, 16 MB flash, octal PSRAM,
and the 你好小益 wake model. In `menuconfig`, set the ACOS token under
`ESP-Brookesia: Agent Xiaozhi Configurations`. The generated `sdkconfig` is
local and ignored by Git; never commit or share it. Wi-Fi credentials are
provisioned on the device and stored in NVS.

This project incorporates ESP-Brookesia components by Espressif. See
[`license.txt`](license.txt) for the upstream license.
