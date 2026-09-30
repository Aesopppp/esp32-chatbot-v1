#include "brookesia/agent_xiaozhi/macro_configs.h"
#if !BROOKESIA_AGENT_XIAOZHI_ENABLE_DEBUG_LOG
#define BROOKESIA_LOG_DISABLE_DEBUG_TRACE 1
#endif
#include "private/utils.hpp"
#include "brookesia/agent_xiaozhi/agent_xiaozhi.hpp"
#include "brookesia/service_helper/audio.hpp"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "boost/json.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <algorithm>

namespace esp_brookesia::agent {
using AudioHelper = service::helper::Audio;

// ACOS 实时 Agent 的传输层：负责 WebSocket 生命周期、认证、麦克风
// Opus 上行、服务器音频下行，以及回答播放期间的语音打断。
// 音频采集由 brookesia_service_audio 完成，本类只消费编码后的数据，
// 避免网络操作阻塞 AFE 音频任务。

namespace {
const boost::json::object &error_details(const boost::json::object &message)
{
    auto error = message.if_contains("error");
    return error && error->is_object() ? error->as_object() : message;
}
bool is_inactive_cancel(const boost::json::object &message)
{
    const auto &details = error_details(message);
    auto code = details.if_contains("code");
    auto text = details.if_contains("message");
    return (code && code->is_string() && code->as_string() == "response_cancel_not_active") ||
           (text && text->is_string() && text->as_string() == "Conversation has none active response");
}
}

bool XiaoZhi::on_init()
{
    // 初始化阶段创建上行工作线程，但不假定 WiFi 已经连接；网络连接
    // 在后续 startup/activate 流程中建立，并允许失败后重试。
    uplink_running_ = true;
    try {
        BROOKESIA_THREAD_CONFIG_GUARD({
            .name = "ACOS_Uplink", .core_id = 1, .priority = 5,
            .stack_size = 8192, .stack_in_ext = true,
        });
        uplink_thread_ = boost::thread([this]() { uplink_worker(); });
    } catch (const std::exception &e) {
        uplink_running_ = false;
        BROOKESIA_LOGE("Failed to create ACOS uplink thread: %1%", e.what());
        return false;
    }
    BROOKESIA_LOGI("ACOS agent initialized (local ESP-SR wake word is retained)");
    return true;
}
void XiaoZhi::on_deinit()
{
    {
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        uplink_running_ = false;
        uplink_queue_.clear();
    }
    uplink_cv_.notify_all();
    if (uplink_thread_.joinable()) uplink_thread_.join();
    clear_receive_queue();
    mcp_tool_registry_.remove_all_tools();
}
std::expected<boost::json::array, std::string> XiaoZhi::function_add_mcp_tools_with_service_function(
    const std::string &name, const boost::json::array &functions)
{
    std::vector<std::string> names;
    if (!BROOKESIA_DESCRIBE_FROM_JSON(functions, names)) return std::unexpected("Invalid function list");
    return BROOKESIA_DESCRIBE_TO_JSON(mcp_tool_registry_.add_service_tools(name, std::move(names))).as_array();
}
std::expected<boost::json::array, std::string> XiaoZhi::function_add_mcp_tools_with_custom_function(
    const boost::json::array &tools)
{
    std::vector<mcp_utils::CustomTool> custom;
    if (!BROOKESIA_DESCRIBE_FROM_JSON(tools, custom)) return std::unexpected("Invalid tools");
    std::vector<std::string> added;
    for (auto &tool : custom) added.push_back(mcp_tool_registry_.add_custom_tool(std::move(tool)));
    return BROOKESIA_DESCRIBE_TO_JSON(added).as_array();
}
std::expected<void, std::string> XiaoZhi::function_remove_mcp_tools(const boost::json::array &tools)
{
    std::vector<std::string> names;
    if (!BROOKESIA_DESCRIBE_FROM_JSON(tools, names)) return std::unexpected("Invalid tool list");
    for (const auto &name : names) mcp_tool_registry_.remove_tool(name);
    return {};
}
std::expected<std::string, std::string> XiaoZhi::function_explain_image(
    const service::RawBuffer &, const std::string &)
{
    return std::unexpected("ACOS image explanation is not supported by this realtime protocol");
}

bool XiaoZhi::on_activate()
{
    // 激活 Agent 后启动 WebSocket。连接成功并收到 authenticated/ready
    // 事件后，设备才允许上传用户语音。
    // ACOS uses a bearer-like token in the websocket auth message and has no
    // separate activation-code flow.  Activation is therefore local and fast.
    trigger_general_event(GeneralEvent::Activated);
    return true;
}

void XiaoZhi::ws_event_handler(void *arg, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto *self = static_cast<XiaoZhi *>(arg);
    if (self) self->handle_ws_event(event_id, static_cast<esp_websocket_event_data_t *>(event_data));
}

bool XiaoZhi::send_json(const std::string &json, unsigned generation, unsigned audio_epoch)
{
    // 所有 WebSocket 写操作都要检查连接代次和音频代次，防止打断或重连
    // 后旧回答继续发送到新会话。
    std::lock_guard<std::mutex> lock(ws_send_mutex_);
    if (!ws_client_ || !ws_started_.load() || ws_stop_pending_.load()) return false;
    if (generation && generation != ws_generation_.load()) return false;
    if (generation && (!conversation_awake_.load() || audio_epoch != audio_epoch_.load())) return false;
    int sent = esp_websocket_client_send_text(ws_client_, json.data(), json.size(), pdMS_TO_TICKS(5000));
    return sent == static_cast<int>(json.size());
}
bool XiaoZhi::post_ws_task(std::function<void()> task, bool)
{
    // The WS callback runs under the client's lock. Never call audio RPCs or
    // stop/destroy the client there. Preserve frame order on the state strand.
    auto scheduler = get_task_scheduler();
    if (!scheduler) return false;
    const auto generation = ws_generation_.load();
    return scheduler->post([this, generation, task = std::move(task)]() {
        if (ws_started_.load() && generation == ws_generation_.load()) task();
    }, nullptr, get_state_task_group());
}
bool XiaoZhi::dispatch_receive_frame(const uint8_t *data, size_t size, uint8_t opcode)
{
    uint32_t turn = 0;
    if (opcode == 2) {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        turn = response_flow_.audio();
        if (!conversation_awake_.load() || !response_flow_.accepts(turn)) return true;
    } else if (opcode == 1) {
        boost::system::error_code ec;
        auto value = boost::json::parse(
            boost::json::string_view(reinterpret_cast<const char *>(data), size), ec);
        if (ec || !value.is_object()) return true;
        auto field = value.as_object().if_contains("type");
        if (!field || !field->is_string()) return true;
        const auto &type = field->as_string();
        if (type == "input_audio_buffer.speech_started") {
            if (!conversation_awake_.load()) return true;
            acos::ResponseFlow::Cancellation cancellation;
            {
                std::lock_guard<std::mutex> lock(receive_mutex_);
                cancellation = response_flow_.interrupt();
            }
            // Do not enqueue behind seconds of TTS. The callback only marks
            // old packets; audio RPCs run on the serial state task group.
            return post_ws_task([this, cancellation]() {
                if (!conversation_awake_.load()) return;
                BROOKESIA_LOGI("ACOS server detected speech (AEC microphone stream)");
                if (interrupt_response("user speech", cancellation)) {
                    // Server VAD also owns the idle pause. This keeps a long
                    // utterance awake even if the local AFE wake window ended.
                    AudioHelper::call_function_async(AudioHelper::FunctionId::PauseAFE_WakeupEnd);
                }
            });
        }
        if (type == "input_audio_buffer.speech_stopped" || type == "error") {
            if (type == "error" && is_inactive_cancel(value.as_object())) {
                // Treat the cancel-not-active reply as a wire boundary before
                // the next binary frame arrives, not after playback drains.
                std::lock_guard<std::mutex> lock(receive_mutex_);
                if (response_flow_.cancel_requested == response_flow_.current &&
                        !response_flow_.accepts(response_flow_.current)) response_flow_.done();
            }
            std::string message(reinterpret_cast<const char *>(data), size);
            return post_ws_task([this, message = std::move(message)]() {
                handle_ws_frame(reinterpret_cast<const uint8_t *>(message.data()), message.size(), 1);
            });
        }
        const bool done = type == "response.audio.done" || type == "response.done" ||
                          type == "response.cancelled" || type == "response.canceled";
        if (done || type == "response.text.delta" || type == "response.audio_transcript.delta") {
            std::lock_guard<std::mutex> lock(receive_mutex_);
            turn = done ? response_flow_.done() : response_flow_.current;
        }
    }
    return enqueue_receive_frame(data, size, opcode, turn);
}

bool XiaoZhi::enqueue_receive_frame(const uint8_t *data, size_t size, uint8_t opcode, uint32_t turn)
{
    constexpr size_t QUEUE_BYTES = 512 * 1024;
    const auto allocation_size = sizeof(ReceiveFrame) + size;
    bool schedule = false;
    unsigned epoch = 0;
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        if (allocation_size > QUEUE_BYTES - receive_bytes_) return false;
        auto *frame = static_cast<ReceiveFrame *>(heap_caps_malloc(allocation_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!frame) return false;
        frame->next = nullptr;
        frame->size = size;
        frame->opcode = opcode;
        frame->turn = turn;
        std::memcpy(frame + 1, data, size);
        if (receive_tail_) receive_tail_->next = frame;
        else receive_head_ = frame;
        receive_tail_ = frame;
        receive_bytes_ += allocation_size;
        if (!receive_drain_pending_) {
            receive_drain_pending_ = true;
            schedule = true;
        }
        epoch = receive_epoch_;
    }
    return !schedule || post_ws_task([this, epoch]() { drain_receive_queue(epoch); });
}

void XiaoZhi::drain_receive_queue(unsigned epoch)
{
    ReceiveFrame *frame = nullptr;
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        if (epoch != receive_epoch_) return;
        // Drop the entire obsolete tail in one pass, not one scheduled task
        // per packet. Preserve untagged authentication/control messages.
        while (receive_head_ && receive_head_->turn && !response_flow_.accepts(receive_head_->turn)) {
            auto *old = receive_head_;
            receive_head_ = old->next;
            receive_bytes_ -= sizeof(ReceiveFrame) + old->size;
            heap_caps_free(old);
        }
        if (!receive_head_) receive_tail_ = nullptr;
        frame = receive_head_;
        if (!frame) { receive_drain_pending_ = false; return; }
        receive_head_ = frame->next;
        if (!receive_head_) receive_tail_ = nullptr;
        receive_bytes_ -= sizeof(ReceiveFrame) + frame->size;
    }
    std::unique_ptr<ReceiveFrame, decltype(&heap_caps_free)> owner(frame, heap_caps_free);
    handle_ws_frame(reinterpret_cast<const uint8_t *>(frame + 1), frame->size, frame->opcode, frame->turn);
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        if (epoch != receive_epoch_) return;
        if (!receive_head_) { receive_drain_pending_ = false; return; }
    }
    // Yield between packets so lifecycle and wake/sleep actions remain responsive.
    if (!post_ws_task([this, epoch]() { drain_receive_queue(epoch); })) request_reconnect();
}

void XiaoZhi::clear_receive_queue()
{
    std::lock_guard<std::mutex> lock(receive_mutex_);
    while (receive_head_) {
        auto *next = receive_head_->next;
        heap_caps_free(receive_head_);
        receive_head_ = next;
    }
    receive_tail_ = nullptr;
    receive_bytes_ = 0;
    receive_drain_pending_ = false;
    ++receive_epoch_;
}

void XiaoZhi::request_reconnect()
{
    ws_ready_.store(false);
    conversation_awake_.store(false);
    if (!ws_stop_pending_.exchange(true)) {
        ++ws_generation_;
        clear_receive_queue();
        post_ws_task([this]() { trigger_general_event(GeneralEvent::Stopped); }, true);
    }
}

void XiaoZhi::finish_response()
{
    if (!is_speaking()) return;
    const auto epoch = response_epoch_;
    const auto generation = ws_generation_.load();
    auto scheduler = get_task_scheduler();
    if (!scheduler) return;
    // The ordered queue has fed all audio. Allow the feeder's five packet
    // FIFO and decoder/mixer to drain before reopening half-duplex recording.
    scheduler->post_delayed([this, epoch, generation]() {
        if (generation != ws_generation_.load() || epoch != response_epoch_ || !conversation_awake_.load()) return;
        set_speaking(false);
        set_listening(true);
        BROOKESIA_LOGI("ACOS response drained; continuous listening (60s idle timeout)");
    }, 1000, nullptr, get_state_task_group());
}
bool XiaoZhi::send_auth()
{
    std::string token = BROOKESIA_AGENT_XIAOZHI_ACOS_TOKEN;
    if (token.empty()) {
        BROOKESIA_LOGE("ACOS token is empty; set it in menuconfig");
        return false;
    }
    boost::json::object msg;
    msg["type"] = "auth";
    msg["token"] = token;
    return send_json(boost::json::serialize(msg));
}
bool XiaoZhi::set_playback_stopped()
{
    // Called on the same serial group as FeedDecoderData. Wait until both
    // RPCs complete so an old feed cannot race destruction of the feeder FIFO.
    auto stopped = AudioHelper::call_function_sync(AudioHelper::FunctionId::StopDecoder);
    if (!stopped) {
        BROOKESIA_LOGE("ACOS decoder stop failed: %1%", stopped.error());
        return false;
    }
    if (ws_started_.load() && !ws_stop_pending_.load()) {
        auto started = AudioHelper::call_function_sync(AudioHelper::FunctionId::StartDecoder,
            BROOKESIA_DESCRIBE_TO_JSON(get_audio_config().decoder).as_object());
        if (!started) {
            BROOKESIA_LOGE("ACOS decoder restart failed: %1%", started.error());
            return false;
        }
    }
    return true;
}

bool XiaoZhi::queue_cancel(uint32_t turn)
{
    {
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        if (!uplink_running_ || !ws_ready_.load()) return false;
        if (cancel_queue_.size() >= 4) return false;
        cancel_queue_.push_back({turn, ws_generation_.load(), audio_epoch_.load()});
    }
    uplink_cv_.notify_one();
    return true;
}

bool XiaoZhi::interrupt_response(const char *reason, acos::ResponseFlow::Cancellation cancellation)
{
    // 打断顺序很关键：先停止扬声器，再发送服务器取消事件，最后清理
    // 旧响应状态；上行队列保留句首数据，保证短句不会被截掉。
    // A newer answer may have arrived while this control task was queued.
    if (is_speaking() && playback_turn_ > cancellation.through) return true;
    ++response_epoch_;
    const bool was_speaking = is_speaking();
    if (was_speaking && !set_playback_stopped()) {
        request_reconnect();
        return false;
    }
    reset_interrupted_speaking();
    playback_turn_ = 0;
    set_speaking(false);
    set_listening(true);
    if (was_speaking) {
        BROOKESIA_LOGI("ACOS barge-in: %1%; old audio discarded, microphone upload continues", reason);
    }
    // Do not clear uplink: it contains the beginning of the interrupting
    // utterance. Cancel writes use the network worker, never the AFE callback.
    if (cancellation.send && !queue_cancel(cancellation.through)) {
        request_reconnect();
        return false;
    }
    return true;
}

void XiaoZhi::handle_ws_frame(const uint8_t *data, size_t len, uint8_t opcode, uint32_t turn)
{
    // 文本帧包含状态和文本事件，二进制帧通常是服务器返回的 Opus 音频。
    // 无效或过期 turn 会被丢弃，避免旧回答污染当前会话。
    if (turn) {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        if (!response_flow_.accepts(turn)) return;
    }
    if (opcode == 2) {
        if (ws_ready_.load() && conversation_awake_.load() && len) {
            ++response_epoch_;
            playback_turn_ = turn;
            // HalfDuplex drops decoder data while listening is true.
            if (!is_speaking()) {
                set_listening(false);
                set_speaking(true);
                if (get_chat_mode() == ChatMode::HalfDuplex) clear_uplink();
                if (is_speaking()) BROOKESIA_LOGI("ACOS response audio started");
            }
            if (is_speaking() && !feed_audio_decoder_data(data, len)) request_reconnect();
        }
        return;
    }
    if (opcode != 1 || !data || !len) return;
    try {
        auto value = boost::json::parse(std::string(reinterpret_cast<const char *>(data), len));
        if (!value.is_object()) return;
        auto &obj = value.as_object();
        auto it = obj.if_contains("type");
        if (!it || !it->is_string()) return;
        std::string type = it->as_string().c_str();
        if (type == "__proxy_connected__") {
            if (ws_ready_.exchange(true)) return;
            BROOKESIA_LOGI("ACOS websocket authenticated");
            conversation_awake_.store(false);
            trigger_general_event(GeneralEvent::Started);
            trigger_general_event(GeneralEvent::Slept);
            BROOKESIA_LOGI("ACOS ready; waiting for local wake word (你好小益)");
        } else if (type == "error") {
            const auto &details = error_details(obj);
            std::string code;
            if (auto field = details.if_contains("code"); field && field->is_string()) code = field->as_string().c_str();
            if (is_inactive_cancel(obj)) {
                BROOKESIA_LOGI("ACOS cancel: server response already finished; microphone remains active");
            } else {
                // Print a bounded identifier, not an entire server payload
                // which could echo authentication or audio data.
                if (code.size() > 64 || code.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:") != std::string::npos) code = "invalid";
                BROOKESIA_LOGE("ACOS server error, code=%1%", code.empty() ? "unspecified" : code);
            }
        } else if (!conversation_awake_.load()) {
            // Authentication never opens the microphone upload gate.
            return;
        } else if (type == "input_audio_buffer.speech_started") {
            // Handled with priority in dispatch_receive_frame().
            return;
        } else if (type == "input_audio_buffer.speech_stopped") {
            BROOKESIA_LOGI("ACOS server detected speech end; waiting for response");
            if (!is_speaking()) {
                set_listening(true);
                AudioHelper::call_function_async(AudioHelper::FunctionId::ResumeAFE_WakeupEnd);
            }
        } else if (type == "response.audio.done" || type == "response.done" ||
                   type == "response.cancelled" || type == "response.canceled") {
            finish_response();
        } else if (type == "response.text.delta" || type == "response.audio_transcript.delta") {
            if (auto t = obj.if_contains("delta"); t && t->is_string()) set_agent_speaking_text(t->as_string().c_str());
        }
    } catch (const std::exception &e) {
        BROOKESIA_LOGW("Invalid ACOS JSON frame: %s", e.what());
    }
}
void XiaoZhi::handle_ws_event(int32_t event_id, esp_websocket_event_data_t *event)
{
    // 连接、断开和错误事件统一在这里转换为 Agent 状态；断线后由
    // request_reconnect 安排重连，不让音频任务永久等待网络。
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ws_ready_.store(false);
        rx_frame_.clear();
        BROOKESIA_LOGI("ACOS websocket connected, authenticating");
        post_ws_task([this]() {
            if (!send_auth()) BROOKESIA_LOGE("Failed to send ACOS auth");
        }, true);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
        rx_frame_.clear();
        BROOKESIA_LOGW("ACOS websocket disconnected/error");
        request_reconnect();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ws_stop_pending_.load() || !event || event->op_code >= 8 || event->data_len < 0 ||
                event->payload_len < 0 || event->payload_offset < 0) break;
        if (event->payload_offset == 0) {
            if (event->op_code == 1 || event->op_code == 2) {
                rx_frame_.clear();
                rx_opcode_ = event->op_code;
            }
            rx_fragment_base_ = rx_frame_.size();
        }
        if ((rx_opcode_ != 1 && rx_opcode_ != 2) || rx_frame_.size() + event->data_len > 65536 ||
                rx_fragment_base_ + static_cast<size_t>(event->payload_offset) != rx_frame_.size()) {
            BROOKESIA_LOGE("ACOS invalid or oversized fragmented message");
            request_reconnect();
            break;
        }
        if (event->data_len) rx_frame_.insert(rx_frame_.end(), event->data_ptr, event->data_ptr + event->data_len);
        if (event->fin && event->payload_offset + event->data_len == event->payload_len) {
            if (!rx_frame_.empty() && !dispatch_receive_frame(rx_frame_.data(), rx_frame_.size(), rx_opcode_)) {
                BROOKESIA_LOGE("ACOS receive PSRAM buffer exhausted; restarting session");
                request_reconnect();
            }
            rx_frame_.clear();
            rx_opcode_ = 0;
        }
        break;
    default: break;
    }
}

bool XiaoZhi::on_startup()
{
    if (ws_client_) return ws_started_.load();
    esp_websocket_client_config_t config{};
    config.uri = BROOKESIA_AGENT_XIAOZHI_ACOS_WS_URL;
    config.buffer_size = 8192;
    config.network_timeout_ms = 10000;
    config.task_stack = 6144;
    config.keep_alive_enable = true;
    config.ping_interval_sec = 20;
    config.pingpong_timeout_sec = 60;
    config.reconnect_timeout_ms = 5000;
    // AgentManager owns restart after Stopped; do not race a second reconnect.
    config.disable_auto_reconnect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    ws_client_ = esp_websocket_client_init(&config);
    if (!ws_client_) { BROOKESIA_LOGE("Failed to create ACOS websocket"); return false; }
    if (esp_websocket_register_events(ws_client_, WEBSOCKET_EVENT_ANY, ws_event_handler, this) != ESP_OK) {
        esp_websocket_client_destroy(ws_client_); ws_client_ = nullptr; return false;
    }
    ++ws_generation_;
    conversation_awake_.store(false);
    ++audio_epoch_;
    clear_receive_queue();
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        response_flow_ = {};
    }
    clear_uplink();
    ws_stop_pending_.store(false);
    ws_started_.store(true);
    if (esp_websocket_client_start(ws_client_) != ESP_OK) {
        ws_started_.store(false);
        esp_websocket_client_destroy(ws_client_); ws_client_ = nullptr; return false;
    }
    return true;
}
void XiaoZhi::on_shutdown()
{
    conversation_awake_.store(false);
    ++audio_epoch_;
    ++response_epoch_;
    clear_receive_queue();
    ws_ready_.store(false);
    ws_started_.store(false);
    ws_stop_pending_.store(true);
    ++ws_generation_;
    clear_uplink();
    std::lock_guard<std::mutex> lock(ws_send_mutex_);
    if (ws_client_) {
        BROOKESIA_LOGI("ACOS stopping transport");
        esp_websocket_client_stop(ws_client_);
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
        BROOKESIA_LOGI("ACOS transport stopped");
    }
    rx_frame_.clear();
    set_playback_stopped();
    trigger_general_event(GeneralEvent::Stopped);
}
bool XiaoZhi::on_sleep()
{
    // 睡眠只结束当前 ACOS 会话，不关闭本地 AFE 唤醒词检测，因此设备
    // 空闲后仍能通过“你好小益”重新进入对话。
    conversation_awake_.store(false);
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        response_flow_.interrupt();
    }
    ++audio_epoch_;
    ++response_epoch_;
    clear_receive_queue();
    clear_uplink();
    set_listening(false);
    if (is_speaking()) set_playback_stopped();
    set_speaking(false);
    trigger_general_event(GeneralEvent::Slept);
    BROOKESIA_LOGI("ACOS sleeping; local wake word detection remains active");
    return true;
}
bool XiaoZhi::on_wakeup()
{
    // 唤醒后清除睡眠状态并打开语音上行窗口；连续对话期间的短句无需
    // 重复唤醒，直到 AFE 空闲超时或 Agent 主动进入睡眠。
    if (!ws_ready_.load()) return false;
    // Awake must clear the Slept bit before listening can be enabled.
    trigger_general_event(GeneralEvent::Awake);
    ++audio_epoch_;
    conversation_awake_.store(true);
    set_listening(true);
    BROOKESIA_LOGI("ACOS awake; ready for speech");
    return true;
}
bool XiaoZhi::on_interrupt_speaking()
{
    // 本地唤醒词或连续对话逻辑最终都从这里进入统一打断流程，不能只
    // 停止扬声器，否则服务器仍会继续推送上一轮回答。
    acos::ResponseFlow::Cancellation cancellation;
    {
        std::lock_guard<std::mutex> lock(receive_mutex_);
        cancellation = response_flow_.interrupt();
    }
    return post_ws_task([this, cancellation]() {
        if (conversation_awake_.load()) interrupt_response("wake word", cancellation);
        else reset_interrupted_speaking();
    });
}
bool XiaoZhi::on_manual_start_listening() { set_listening(true); return true; }
bool XiaoZhi::on_manual_stop_listening() { set_listening(false); return true; }
bool XiaoZhi::on_encoder_data_ready(const uint8_t *data, size_t data_size)
{
    // 音频服务每产生一帧 Opus 数据就调用一次。这里只快速入队，实际
    // WebSocket 发送由 uplink_worker 完成，避免阻塞音频回调。
    if (!ws_ready_.load() || !conversation_awake_.load() || !data || !data_size) return true;
    if (data_size > 2048) return false;
    {
        // Never wait for the network in Audio's event callback. Bound backlog
        // to about one second; a slow connection must not replay stale speech.
        std::lock_guard<std::mutex> lock(uplink_mutex_);
        if (!uplink_running_ || !ws_ready_.load() || !conversation_awake_.load()) return true;
        if (uplink_queue_.size() >= 50) {
            uplink_queue_.pop_front();
        }
        uplink_queue_.push_back({{data, data + data_size}, ws_generation_.load(), audio_epoch_.load(), esp_timer_get_time()});
    }
    uplink_cv_.notify_one();
    return true;
}

void XiaoZhi::clear_uplink()
{
    std::lock_guard<std::mutex> lock(uplink_mutex_);
    uplink_queue_.clear();
    cancel_queue_.clear();
}

void XiaoZhi::uplink_worker()
{
    // 上行线程从队列取出 Opus 帧并发送。连接断开、会话睡眠或代次变化
    // 时立即丢弃旧帧，避免重连后把上一轮语音发给服务器。
    unsigned logged_generation = 0;
    for (;;) {
        UplinkPacket packet;
        CancelCommand cancel{};
        bool has_cancel = false;
        {
            std::unique_lock<std::mutex> lock(uplink_mutex_);
            uplink_cv_.wait(lock, [this]() { return !uplink_running_ || !cancel_queue_.empty() || !uplink_queue_.empty(); });
            if (!uplink_running_) return;
            has_cancel = !cancel_queue_.empty();
            if (has_cancel) {
                cancel = cancel_queue_.front();
                cancel_queue_.pop_front();
            } else {
                packet = std::move(uplink_queue_.front());
                uplink_queue_.pop_front();
            }
        }
        if (has_cancel) {
            if (cancel.generation != ws_generation_.load() || cancel.audio_epoch != audio_epoch_.load() ||
                    !ws_ready_.load() || !conversation_awake_.load()) continue;
            {
                std::lock_guard<std::mutex> lock(receive_mutex_);
                // Generation may have completed while cancellation was queued.
                if (response_flow_.current != cancel.turn || !response_flow_.open) continue;
            }
            boost::json::object message;
            message["event_id"] = "vocat_cancel_" + std::to_string(cancel.generation) + "_" + std::to_string(cancel.turn);
            message["type"] = "response.cancel";
            if (!send_json(boost::json::serialize(message), cancel.generation, cancel.audio_epoch) &&
                    cancel.generation == ws_generation_.load() && cancel.audio_epoch == audio_epoch_.load() &&
                    conversation_awake_.load()) request_reconnect();
            continue;
        }
        if (!ws_ready_.load() || !conversation_awake_.load() || packet.audio_epoch != audio_epoch_.load() ||
                packet.generation != ws_generation_.load() ||
                esp_timer_get_time() - packet.queued_at_us > 1000000) continue;
        std::vector<unsigned char> b64(4 * ((packet.data.size() + 2) / 3) + 1);
        size_t out = 0;
        if (mbedtls_base64_encode(b64.data(), b64.size(), &out,
                                packet.data.data(), packet.data.size()) != 0) continue;
        boost::json::object msg;
        msg["event_id"] = "vocat_" + std::to_string(packet.generation) + "_" + std::to_string(++uplink_sequence_);
        msg["type"] = "input_audio_buffer.append";
        msg["audio"] = std::string(reinterpret_cast<char *>(b64.data()), out);
        msg["encoding"] = "opus";
        if (send_json(boost::json::serialize(msg), packet.generation, packet.audio_epoch)) {
            if (logged_generation != packet.generation) {
                logged_generation = packet.generation;
                BROOKESIA_LOGI("ACOS first microphone packet uploaded (Opus 16kHz, 20ms)");
            }
        } else if (packet.generation == ws_generation_.load() && packet.audio_epoch == audio_epoch_.load() &&
                   conversation_awake_.load() && !ws_stop_pending_.load()) {
            clear_uplink();
            BROOKESIA_LOGW("ACOS audio upload failed; requesting reconnect");
            request_reconnect();
        }
    }
}

#if BROOKESIA_AGENT_XIAOZHI_ENABLE_AUTO_REGISTER
BROOKESIA_PLUGIN_REGISTER_SINGLETON(Base, XiaoZhi, XiaoZhi::get_instance().get_attributes().get_name(), XiaoZhi::get_instance());
// Register with the service manager too, and provide the symbol retained by
// CMake's -u option so the linker includes this plugin from its static library.
BROOKESIA_PLUGIN_REGISTER_SINGLETON_WITH_SYMBOL(
    service::ServiceBase, XiaoZhi, XiaoZhi::get_instance().get_attributes().get_name(),
    XiaoZhi::get_instance(), BROOKESIA_AGENT_XIAOZHI_PLUGIN_SYMBOL
);
#endif
} // namespace esp_brookesia::agent

