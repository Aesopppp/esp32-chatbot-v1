# Chatbot 工程中文代码说明

本文档说明本工程从上电到语音对话的执行链路，并标出日常开发最常修改的文件。`agent`、`service`、`hal` 和 `utils` 目录来自 ESP-Brookesia 框架；应用业务主要位于本目录的 `main`。

## 1. 上电执行顺序

`main/main.cpp` 中的 `app_main()` 是 ESP-IDF 的入口。它依次完成：

1. 降低底层音频和网络组件的日志等级。
2. 通过 HAL 初始化屏幕、音频编解码器、存储和其他板级设备。
3. 启动 `ServiceManager` 和后台任务调度器。
4. 初始化 NVS、SNTP、设备服务和音频服务。
5. 配置 ESP-SR AFE 和唤醒词，并注册 ACOS Agent。
6. 启动 LVGL 表情界面、触摸手势和 WiFi 配网流程。

初始化任务被投递到后台调度器，因此 `app_main()` 不会阻塞在配网或网络连接上。

## 2. 语音数据链路

```text
麦克风 → ESP-SR AFE（降噪、VAD、唤醒词）
       → Opus 编码 → ACOS WebSocket（上行语音）
       → ACOS 返回 JSON 事件和 Opus 音频
       → 接收队列 → 解码器 → 功放/扬声器
```

`main/modules/ai_agents.cpp` 负责把应用事件和 Agent 状态连接起来；`agent/brookesia_agent_xiaozhi/src/agent_xiaozhi.cpp` 负责 ACOS WebSocket、音频上行、响应播放和打断。

## 3. 唤醒词

唤醒词模型由 ESP-SR 在编译时打包到 `model` 分区。ESP32-S3 当前默认配置位于 `sdkconfig.defaults.esp32s3`，目前选择的是 `wn9_nihaoxiaoyi_tts2`（“你好小益”）。

更换内置唤醒词时修改 `sdkconfig.defaults.esp32s3`，然后重新执行 `idf.py build` 和 `idf.py flash`。只修改日志中的文字不会改变识别模型；自定义词语需要单独生成 ESP-SR 模型。

## 4. WiFi 配网

`main/modules/wifi_provisioning.cpp` 先读取 NVS 中保存的 AP：

- 找到已保存 AP：启动 STA 自动连接。
- 没有保存 AP：启动 SoftAP 配网网页。
- STA 连接失败：回退到 SoftAP，让用户重新输入 WiFi。
- SoftAP 配网成功：停止热点，之后由 STA 连接。

WiFi 凭据由 WiFi 服务保存到 NVS，不写入源码。

## 5. ACOS 连续对话和打断

`agent_xiaozhi.cpp` 中的 `conversation_awake_` 表示当前是否处于可对话状态：

- `on_wakeup()` 清除睡眠状态并开始接收用户语音。
- `on_encoder_data_ready()` 将编码后的麦克风数据放入上行队列。
- `handle_ws_frame()` 处理服务器事件、文本和音频数据。
- `interrupt_response()` 先停止本地播放，再发送取消事件，清理旧响应队列，然后保留新的语音输入。
- `on_sleep()` 结束当前会话，但保留本地唤醒词检测。

响应播放和 WebSocket 接收分别使用队列及工作任务，避免在音频回调中直接执行网络操作。

## 6. 显示和设置界面

`main/modules/display/display.cpp` 启动 LVGL、表情动画和触摸手势；`display/screens/settings.cpp` 实现 WiFi 状态、亮度、音量、Agent 切换和恢复出厂设置。SquareLine 生成的 C 文件位于 `display/squareline_ui`，修改界面业务逻辑时优先修改 C++ 封装层。

## 7. 常见修改位置

| 需求 | 文件 |
| --- | --- |
| 修改启动流程 | `main/main.cpp` |
| 修改音频、唤醒词和 AFE 超时 | `main/modules/ai_agents.cpp`、`sdkconfig.defaults.esp32s3` |
| 修改 ACOS URL、认证、连续对话和打断 | `agent/brookesia_agent_xiaozhi` |
| 修改配网逻辑 | `main/modules/wifi_provisioning.cpp` |
| 修改音量、亮度和设置页 | `main/modules/display/screens/settings.cpp` |
| 修改表情和状态映射 | `main/modules/ai_agents.cpp`、`main/modules/display` |
| 修改板级引脚和设备 | `hal/brookesia_hal_boards` |

## 8. 编译烧录

必须使用 ESP-IDF 5.5.5。进入本目录后执行：

```powershell
idf.py build
idf.py -p COM6 flash monitor
```

唤醒词、分区表或 `sdkconfig.defaults.esp32s3` 改动后必须重新编译并烧录；只修改注释和文档不需要重新烧录。
