#include "brookesia/agent_xiaozhi/detail/response_flow.hpp"

using esp_brookesia::agent::acos::ResponseFlow;

// Exercise the actual queue policy at compile time, without an ESP32 device.
static_assert([] {
    ResponseFlow flow;
    const auto first = flow.audio();
    if (flow.audio() != first) return false;
    auto cancel = flow.interrupt();
    if (!cancel.send || flow.accepts(first)) return false;
    // Late packets before the cancellation boundary belong to the old answer.
    if (flow.audio() != first || flow.interrupt().send) return false;
    flow.done();
    const auto second = flow.audio();
    return second > first && flow.accepts(second) && !flow.accepts(first);
}(), "Interrupt must discard late old audio and allow the next answer");

static_assert([] {
    ResponseFlow flow;
    const auto buffered = flow.audio();
    flow.done();
    flow.done(); // Some servers send both audio.done and response.done.
    const auto cancel = flow.interrupt();
    return !cancel.send && !flow.accepts(buffered) && flow.accepts(flow.audio());
}(), "Locally buffered completed answers must not send response.cancel");

static_assert([] {
    ResponseFlow flow;
    if (flow.interrupt().send || flow.accepts(0)) return false;
    const auto first = flow.audio();
    flow.done();
    const auto second = flow.audio();
    flow.interrupt();
    return !flow.accepts(first) && !flow.accepts(second);
}(), "New user speech invalidates every older buffered answer");
