/* ACOS-backed agent. The class name is kept for compatibility with the Brookesia chatbot example. */
#pragma once
#include <atomic>
#include <functional>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "boost/thread.hpp"
#include "esp_websocket_client.h"
#include "brookesia/agent_manager/base.hpp"
#include "brookesia/agent_helper/xiaozhi.hpp"
#include "brookesia/mcp_utils/mcp_utils.hpp"
#include "brookesia/agent_xiaozhi/macro_configs.h"
#include "brookesia/agent_xiaozhi/detail/response_flow.hpp"
namespace esp_brookesia::agent {
class XiaoZhi: public Base {
public:
    inline static const AgentAttributes DEFAULT_AGENT_ATTRIBUTES{
        .name = helper::XiaoZhi::get_name().data(),
        .operation_timeout = {.activate = 10000, .start = 15000, .sleep = 5000, .wake_up = 5000},
        .support_general_functions = {helper::Manager::AgentGeneralFunction::InterruptSpeaking},
        .support_general_events = {helper::Manager::AgentGeneralEvent::SpeakingStatusChanged,
            helper::Manager::AgentGeneralEvent::ListeningStatusChanged,
            helper::Manager::AgentGeneralEvent::AgentSpeakingTextGot,
            helper::Manager::AgentGeneralEvent::UserSpeakingTextGot,
            helper::Manager::AgentGeneralEvent::EmoteGot},
    };
    static constexpr AudioConfig DEFAULT_AUDIO_CONFIG{
        .encoder = {.type = service::helper::Audio::CodecFormat::OPUS,
            .general = {.channels = 1, .sample_bits = 16, .sample_rate = 16000, .frame_duration = 20},
            .extra = service::helper::Audio::EncoderExtraConfigOpus{.enable_vbr = false, .bitrate = 24000},
            .fetch_data_size = 2048},
        .decoder = {.type = service::helper::Audio::CodecFormat::OPUS,
            // Keep a 60 ms output capacity for variable-duration server packets.
            .general = {.channels = 1, .sample_bits = 16, .sample_rate = 16000, .frame_duration = 60}}
    };
    static XiaoZhi &get_instance() { static XiaoZhi instance; return instance; }
    struct ChatInfo { bool has_mqtt_config = false; bool has_websocket_config = true; };
private:
    using Helper = helper::XiaoZhi;
    XiaoZhi(): Base(DEFAULT_AGENT_ATTRIBUTES, DEFAULT_AUDIO_CONFIG) {}
    ~XiaoZhi() = default;
    bool on_init() override;
    void on_deinit() override;
    std::expected<boost::json::array, std::string> function_add_mcp_tools_with_service_function(const std::string &, const boost::json::array &);
    std::expected<boost::json::array, std::string> function_add_mcp_tools_with_custom_function(const boost::json::array &);
    std::expected<void, std::string> function_remove_mcp_tools(const boost::json::array &);
    std::expected<std::string, std::string> function_explain_image(const service::RawBuffer &, const std::string &);
    std::vector<service::FunctionSchema> get_function_schemas() override { auto s=Helper::get_function_schemas(); return {s.begin(),s.end()}; }
    std::vector<service::EventSchema> get_event_schemas() override { auto s=Helper::get_event_schemas(); return {s.begin(),s.end()}; }
    service::ServiceBase::FunctionHandlerMap get_function_handlers() override {
        return {
            BROOKESIA_SERVICE_HELPER_FUNC_HANDLER_2(Helper, Helper::FunctionId::AddMCP_ToolsWithServiceFunction, std::string, boost::json::array, function_add_mcp_tools_with_service_function(PARAM1, PARAM2)),
            BROOKESIA_SERVICE_HELPER_FUNC_HANDLER_1(Helper, Helper::FunctionId::AddMCP_ToolsWithCustomFunction, boost::json::array, function_add_mcp_tools_with_custom_function(PARAM)),
            BROOKESIA_SERVICE_HELPER_FUNC_HANDLER_1(Helper, Helper::FunctionId::RemoveMCP_Tools, boost::json::array, function_remove_mcp_tools(PARAM)),
            BROOKESIA_SERVICE_HELPER_FUNC_HANDLER_2(Helper, Helper::FunctionId::ExplainImage, service::RawBuffer, std::string, function_explain_image(PARAM1, PARAM2)),
        };
    }
    bool on_activate() override;
    bool on_startup() override;
    void on_shutdown() override;
    bool on_sleep() override;
    bool on_wakeup() override;
    bool on_interrupt_speaking() override;
    bool on_manual_start_listening() override;
    bool on_manual_stop_listening() override;
    bool on_encoder_data_ready(const uint8_t *, size_t) override;
    static void ws_event_handler(void *, esp_event_base_t, int32_t, void *);
    void handle_ws_event(int32_t, esp_websocket_event_data_t *);
    void handle_ws_frame(const uint8_t *, size_t, uint8_t, uint32_t turn = 0);
    bool send_json(const std::string &, unsigned generation = 0, unsigned audio_epoch = 0);
    void uplink_worker();
    void clear_uplink();
    bool send_auth();
    bool post_ws_task(std::function<void()> task, bool control = false);
    bool dispatch_receive_frame(const uint8_t *data, size_t size, uint8_t opcode);
    bool enqueue_receive_frame(const uint8_t *data, size_t size, uint8_t opcode, uint32_t turn);
    void drain_receive_queue(unsigned epoch);
    void clear_receive_queue();
    void request_reconnect();
    void finish_response();
    bool set_playback_stopped();
    bool interrupt_response(const char *reason, acos::ResponseFlow::Cancellation cancellation);
    bool queue_cancel(uint32_t turn);
    esp_websocket_client_handle_t ws_client_ = nullptr;
    std::atomic<bool> ws_ready_{false};
    std::atomic<bool> ws_started_{false};
    std::atomic<bool> ws_stop_pending_{false};
    std::atomic<unsigned> ws_generation_{0};
    std::atomic<bool> conversation_awake_{false};
    std::atomic<unsigned> audio_epoch_{0};
    unsigned response_epoch_ = 0;
    uint32_t playback_turn_ = 0;
    std::mutex ws_send_mutex_;
    struct UplinkPacket {
        std::vector<uint8_t> data;
        unsigned generation;
        unsigned audio_epoch;
        int64_t queued_at_us;
    };
    boost::thread uplink_thread_;
    std::mutex uplink_mutex_;
    std::condition_variable uplink_cv_;
    std::deque<UplinkPacket> uplink_queue_;
    struct CancelCommand {
        uint32_t turn;
        unsigned generation;
        unsigned audio_epoch;
    };
    std::deque<CancelCommand> cancel_queue_;
    bool uplink_running_ = false;
    unsigned uplink_sequence_ = 0;
    // Payloads and queue nodes are allocated together in PSRAM. Only one
    // scheduler task drains this queue, regardless of the server burst size.
    struct ReceiveFrame {
        ReceiveFrame *next;
        size_t size;
        uint8_t opcode;
        uint32_t turn;
    };
    std::mutex receive_mutex_;
    ReceiveFrame *receive_head_ = nullptr;
    ReceiveFrame *receive_tail_ = nullptr;
    size_t receive_bytes_ = 0;
    bool receive_drain_pending_ = false;
    unsigned receive_epoch_ = 0;
    acos::ResponseFlow response_flow_;
    std::vector<uint8_t> rx_frame_;
    uint8_t rx_opcode_ = 0;
    size_t rx_fragment_base_ = 0;
    mcp_utils::ToolRegistry mcp_tool_registry_;
};
BROOKESIA_DESCRIBE_STRUCT(XiaoZhi::ChatInfo, (), (has_mqtt_config, has_websocket_config));
} // namespace esp_brookesia::agent
