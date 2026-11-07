/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ir_remote.h"
#include <driver/rmt_tx.h>
#include <driver/rmt_rx.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace ir_remote {

static const std::string_view _tag = "HAL-IR";

// StackChan body pinout (docs.m5stack.com/en/StackChan): IR_SEND = G5, IR_REC = G10
static constexpr gpio_num_t TX_GPIO = GPIO_NUM_5;
static constexpr gpio_num_t RX_GPIO = GPIO_NUM_10;

static constexpr uint32_t RESOLUTION_HZ   = 1000000;  // 1 tick = 1 us
static constexpr int RX_MARK_LEVEL        = 0;        // Demodulating receivers output LOW while carrier is present
static constexpr size_t RX_BUF_SYMBOLS    = 1024;
static constexpr size_t MAX_DURATIONS     = 1000;     // Upper bound for a stored code (2 KB blob)
static constexpr size_t MIN_DURATIONS     = 7;        // Shorter captures are treated as noise
static constexpr uint32_t MAX_SYMBOL_TICK = 32767;    // RMT duration field is 15 bits
static constexpr uint32_t REPEAT_GAP_MS   = 40;

static constexpr const char* NVS_NAMESPACE = "ir_codes";
static constexpr uint16_t BLOB_MAGIC       = 0x4952;  // "IR"
static constexpr uint8_t BLOB_VERSION      = 1;

struct BlobHeader {
    uint16_t magic;
    uint8_t version;
    uint8_t reserved;
    uint32_t carrierHz;
};

static std::recursive_mutex s_hw_mutex;
static bool s_inited                     = false;
static rmt_channel_handle_t s_tx         = nullptr;
static rmt_channel_handle_t s_rx         = nullptr;
static rmt_encoder_handle_t s_copy_enc   = nullptr;
static QueueHandle_t s_rx_queue          = nullptr;
static rmt_symbol_word_t* s_rx_buf       = nullptr;
static size_t s_rx_buf_symbols           = 0;
static uint32_t s_current_carrier_hz     = 0;

static std::mutex s_learn_mutex;
static LearnStatus s_learn_status;

/* -------------------------------------------------------------------------- */
/*                                   Hardware                                 */
/* -------------------------------------------------------------------------- */

static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata, void* user_ctx)
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(static_cast<QueueHandle_t>(user_ctx), edata, &woken);
    return woken == pdTRUE;
}

static std::string s_last_error;

static bool fail_init(const char* step, esp_err_t err)
{
    s_last_error = fmt::format("{} failed: {}", step, esp_err_to_name(err));
    mclog::tagError(_tag, "{}", s_last_error);
    return false;
}

static bool create_rx_channel()
{
    struct Option {
        bool dma;
        size_t memSymbols;
    };
    // DMA is skipped: no GDMA interrupt is free while the AI agent runs.
    const Option options[] = {{false, 192}, {false, 96}, {false, 48}};
    esp_err_t last_err = ESP_FAIL;

    for (const auto& opt : options) {
        rmt_rx_channel_config_t rx_cfg = {};
        rx_cfg.gpio_num                = RX_GPIO;
        rx_cfg.clk_src                 = RMT_CLK_SRC_DEFAULT;
        rx_cfg.resolution_hz           = RESOLUTION_HZ;
        rx_cfg.mem_block_symbols       = opt.memSymbols;
        rx_cfg.flags.with_dma          = opt.dma;

        last_err = rmt_new_rx_channel(&rx_cfg, &s_rx);
        if (last_err != ESP_OK) {
            mclog::tagWarn(_tag, "rx channel (dma: {}, symbols: {}) failed: {}", opt.dma, opt.memSymbols,
                           esp_err_to_name(last_err));
            s_rx = nullptr;
            continue;
        }

        s_rx_buf_symbols = opt.dma ? RX_BUF_SYMBOLS : opt.memSymbols;
        uint32_t caps    = MALLOC_CAP_INTERNAL | (opt.dma ? MALLOC_CAP_DMA : MALLOC_CAP_8BIT);
        s_rx_buf         = static_cast<rmt_symbol_word_t*>(
            heap_caps_aligned_calloc(64, s_rx_buf_symbols, sizeof(rmt_symbol_word_t), caps));
        if (!s_rx_buf) {
            last_err = ESP_ERR_NO_MEM;
            rmt_del_channel(s_rx);
            s_rx = nullptr;
            continue;
        }

        mclog::tagInfo(_tag, "rx channel ready (dma: {}, symbols: {})", opt.dma, s_rx_buf_symbols);
        return true;
    }
    return fail_init("create rx channel", last_err);
}

static esp_err_t apply_carrier(uint32_t carrierHz)
{
    if (carrierHz == s_current_carrier_hz) {
        return ESP_OK;
    }
    rmt_carrier_config_t carrier = {};
    carrier.frequency_hz         = carrierHz;
    carrier.duty_cycle           = 0.33f;
    esp_err_t err                = rmt_apply_carrier(s_tx, &carrier);
    if (err != ESP_OK) {
        mclog::tagError(_tag, "apply carrier {} Hz failed: {}", carrierHz, esp_err_to_name(err));
        return err;
    }
    s_current_carrier_hz = carrierHz;
    return ESP_OK;
}

// Release whatever a failed init created so the next attempt starts clean.
static void deinit_partial()
{
    if (s_rx) {
        rmt_disable(s_rx);
        rmt_del_channel(s_rx);
        s_rx = nullptr;
    }
    if (s_rx_buf) {
        heap_caps_free(s_rx_buf);
        s_rx_buf = nullptr;
    }
    if (s_rx_queue) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = nullptr;
    }
    if (s_copy_enc) {
        rmt_del_encoder(s_copy_enc);
        s_copy_enc = nullptr;
    }
    if (s_tx) {
        rmt_disable(s_tx);
        rmt_del_channel(s_tx);
        s_tx = nullptr;
    }
    s_current_carrier_hz = 0;
}

static bool init_hw()
{
    esp_err_t err;

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num                = TX_GPIO;
    tx_cfg.clk_src                 = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz           = RESOLUTION_HZ;
    tx_cfg.mem_block_symbols       = 48;
    tx_cfg.trans_queue_depth       = 4;
    if ((err = rmt_new_tx_channel(&tx_cfg, &s_tx)) != ESP_OK) {
        s_tx = nullptr;
        return fail_init("create tx channel", err);
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    if ((err = rmt_new_copy_encoder(&copy_cfg, &s_copy_enc)) != ESP_OK) {
        s_copy_enc = nullptr;
        return fail_init("create copy encoder", err);
    }

    s_current_carrier_hz = 0;
    if ((err = apply_carrier(38000)) != ESP_OK) {
        return fail_init("apply tx carrier", err);
    }
    if ((err = rmt_enable(s_tx)) != ESP_OK) {
        return fail_init("enable tx", err);
    }

    if (!create_rx_channel()) {
        return false;
    }

    s_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!s_rx_queue) {
        return fail_init("create rx queue", ESP_ERR_NO_MEM);
    }
    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done             = rx_done_cb;
    if ((err = rmt_rx_register_event_callbacks(s_rx, &cbs, s_rx_queue)) != ESP_OK) {
        return fail_init("register rx callback", err);
    }
    if ((err = rmt_enable(s_rx)) != ESP_OK) {
        return fail_init("enable rx", err);
    }
    return true;
}

bool init()
{
    std::lock_guard<std::recursive_mutex> lock(s_hw_mutex);
    if (s_inited) {
        return true;
    }

    mclog::tagInfo(_tag, "init (tx: GPIO{}, rx: GPIO{})", static_cast<int>(TX_GPIO), static_cast<int>(RX_GPIO));
    if (!init_hw()) {
        deinit_partial();
        return false;
    }

    s_inited = true;
    s_last_error.clear();
    mclog::tagInfo(_tag, "init done");
    return true;
}

std::string last_error()
{
    std::lock_guard<std::recursive_mutex> lock(s_hw_mutex);
    return s_last_error;
}

/* -------------------------------------------------------------------------- */
/*                                  Transmit                                  */
/* -------------------------------------------------------------------------- */

// Pack alternating mark/space durations into RMT symbols, splitting values that
// exceed the 15-bit duration field.
static std::vector<rmt_symbol_word_t> build_symbols(const std::vector<uint16_t>& durations)
{
    struct Half {
        uint16_t level;
        uint16_t duration;
    };
    std::vector<Half> halves;
    halves.reserve(durations.size() + 2);

    for (size_t i = 0; i < durations.size(); ++i) {
        uint16_t level = (i % 2 == 0) ? 1 : 0;
        uint32_t d     = durations[i];
        while (d > 0) {
            uint32_t chunk = std::min(d, MAX_SYMBOL_TICK);
            halves.push_back({level, static_cast<uint16_t>(chunk)});
            d -= chunk;
        }
    }
    if (halves.size() % 2 != 0) {
        halves.push_back({0, 1000});  // Trailing space after the final mark
    }

    std::vector<rmt_symbol_word_t> symbols(halves.size() / 2);
    for (size_t i = 0; i < symbols.size(); ++i) {
        symbols[i].level0    = halves[i * 2].level;
        symbols[i].duration0 = halves[i * 2].duration;
        symbols[i].level1    = halves[i * 2 + 1].level;
        symbols[i].duration1 = halves[i * 2 + 1].duration;
    }
    return symbols;
}

bool send_raw(const IrCode& code, int repeat)
{
    if (code.durations.empty()) {
        return false;
    }
    if (!init()) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(s_hw_mutex);

    uint32_t carrier = code.carrierHz ? code.carrierHz : 38000;
    if (apply_carrier(carrier) != ESP_OK) {
        return false;
    }

    auto symbols = build_symbols(code.durations);
    repeat       = std::clamp(repeat, 1, 20);

    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count            = 0;
    tx_cfg.flags.eot_level       = 0;

    for (int i = 0; i < repeat; ++i) {
        if (rmt_transmit(s_tx, s_copy_enc, symbols.data(), symbols.size() * sizeof(rmt_symbol_word_t), &tx_cfg) !=
            ESP_OK) {
            mclog::tagError(_tag, "transmit failed");
            return false;
        }
        if (rmt_tx_wait_all_done(s_tx, 2000) != ESP_OK) {
            mclog::tagError(_tag, "transmit timeout");
            return false;
        }
        if (i + 1 < repeat) {
            vTaskDelay(pdMS_TO_TICKS(REPEAT_GAP_MS));
        }
    }

    mclog::tagInfo(_tag, "sent {} durations x{} @ {} Hz", code.durations.size(), repeat, carrier);
    return true;
}

bool send_nec(const NecFrame& frame, int repeat)
{
    uint8_t bytes[4];
    if (frame.address <= 0xFF) {
        bytes[0] = static_cast<uint8_t>(frame.address);
        bytes[1] = static_cast<uint8_t>(~frame.address);
    } else {
        bytes[0] = static_cast<uint8_t>(frame.address & 0xFF);
        bytes[1] = static_cast<uint8_t>(frame.address >> 8);
    }
    bytes[2] = frame.command;
    bytes[3] = static_cast<uint8_t>(~frame.command);

    IrCode code;
    code.carrierHz = 38000;
    code.durations = {9000, 4500};
    for (uint8_t b : bytes) {
        for (int bit = 0; bit < 8; ++bit) {
            code.durations.push_back(560);
            code.durations.push_back((b >> bit) & 1 ? 1690 : 560);
        }
    }
    code.durations.push_back(560);

    return send_raw(code, repeat);
}

static bool near(uint32_t value, uint32_t target, uint32_t tolerancePercent = 30)
{
    uint32_t delta = target * tolerancePercent / 100;
    return value + delta >= target && value <= target + delta;
}

std::optional<NecFrame> decode_nec(const std::vector<uint16_t>& d)
{
    if (d.size() < 67 || !near(d[0], 9000) || !near(d[1], 4500)) {
        return std::nullopt;
    }

    uint32_t data = 0;
    for (int bit = 0; bit < 32; ++bit) {
        uint16_t mark  = d[2 + bit * 2];
        uint16_t space = d[3 + bit * 2];
        if (!near(mark, 560, 50)) {
            return std::nullopt;
        }
        if (near(space, 1690, 35)) {
            data |= (1u << bit);
        } else if (!near(space, 560, 50)) {
            return std::nullopt;
        }
    }

    uint8_t addr_lo = data & 0xFF;
    uint8_t addr_hi = (data >> 8) & 0xFF;
    uint8_t cmd     = (data >> 16) & 0xFF;
    uint8_t cmd_inv = (data >> 24) & 0xFF;
    if (static_cast<uint8_t>(~cmd) != cmd_inv) {
        return std::nullopt;
    }

    NecFrame frame;
    frame.address = (static_cast<uint8_t>(~addr_lo) == addr_hi) ? addr_lo : (addr_lo | (addr_hi << 8));
    frame.command = cmd;
    return frame;
}

/* -------------------------------------------------------------------------- */
/*                                   Receive                                  */
/* -------------------------------------------------------------------------- */

static std::vector<uint16_t> symbols_to_durations(const rmt_symbol_word_t* symbols, size_t count)
{
    std::vector<uint32_t> segs;
    int last_kind = -1;  // 1 = mark, 0 = space

    auto add = [&](int level, uint32_t duration) {
        if (duration == 0) {
            return;
        }
        int kind = (level == RX_MARK_LEVEL) ? 1 : 0;
        if (segs.empty() && kind == 0) {
            return;  // Skip leading idle
        }
        if (kind == last_kind) {
            segs.back() += duration;
        } else {
            segs.push_back(duration);
            last_kind = kind;
        }
    };

    for (size_t i = 0; i < count; ++i) {
        add(symbols[i].level0, symbols[i].duration0);
        add(symbols[i].level1, symbols[i].duration1);
    }
    if (!segs.empty() && segs.size() % 2 == 0) {
        segs.pop_back();  // Drop trailing idle space
    }
    if (segs.size() > MAX_DURATIONS) {
        segs.resize(MAX_DURATIONS % 2 == 0 ? MAX_DURATIONS - 1 : MAX_DURATIONS);
    }

    std::vector<uint16_t> out;
    out.reserve(segs.size());
    for (auto s : segs) {
        out.push_back(static_cast<uint16_t>(std::min<uint32_t>(s, 0xFFFF)));
    }
    return out;
}

bool capture(IrCode& out, uint32_t timeoutMs)
{
    if (!init()) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(s_hw_mutex);
    xQueueReset(s_rx_queue);

    rmt_receive_config_t rx_cfg = {};
    rx_cfg.signal_range_min_ns  = 1000;              // Glitch filter
    rx_cfg.signal_range_max_ns  = 30 * 1000 * 1000;  // 30 ms of silence ends a frame

    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;

    while (true) {
        int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) {
            break;
        }

        if (rmt_receive(s_rx, s_rx_buf, s_rx_buf_symbols * sizeof(rmt_symbol_word_t), &rx_cfg) != ESP_OK) {
            mclog::tagError(_tag, "start receive failed");
            return false;
        }

        rmt_rx_done_event_data_t event = {};
        if (xQueueReceive(s_rx_queue, &event, pdMS_TO_TICKS(remaining_ms)) != pdTRUE) {
            // Abort the pending receive so the channel is ready for the next call
            rmt_disable(s_rx);
            rmt_enable(s_rx);
            break;
        }

        auto durations = symbols_to_durations(event.received_symbols, event.num_symbols);
        if (durations.size() >= MIN_DURATIONS) {
            out.carrierHz = 38000;  // Receiver is demodulating; 38 kHz covers most remotes
            out.durations = std::move(durations);
            mclog::tagInfo(_tag, "captured {} durations", out.durations.size());
            return true;
        }
        mclog::tagInfo(_tag, "ignored short burst ({} durations)", durations.size());
    }

    mclog::tagWarn(_tag, "capture timeout");
    return false;
}

/* -------------------------------------------------------------------------- */
/*                                   Storage                                  */
/* -------------------------------------------------------------------------- */

std::optional<std::string> normalize_name(const std::string& name)
{
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == ' ' || c == '-' || c == '_' || c == '.') {
            if (!out.empty() && out.back() != '_') {
                out.push_back('_');
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.size() > 15) {  // NVS key limit
        out.resize(15);
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

bool save_code(const std::string& name, const IrCode& code)
{
    auto key = normalize_name(name);
    if (!key || code.durations.empty() || code.durations.size() > MAX_DURATIONS) {
        return false;
    }

    std::vector<uint8_t> blob(sizeof(BlobHeader) + code.durations.size() * sizeof(uint16_t));
    BlobHeader header = {BLOB_MAGIC, BLOB_VERSION, 0, code.carrierHz};
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), code.durations.data(), code.durations.size() * sizeof(uint16_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key->c_str(), blob.data(), blob.size());
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        mclog::tagError(_tag, "save '{}' failed: {}", *key, esp_err_to_name(err));
        return false;
    }
    mclog::tagInfo(_tag, "saved '{}' ({} bytes)", *key, blob.size());
    return true;
}

bool load_code(const std::string& name, IrCode& out)
{
    auto key = normalize_name(name);
    if (!key) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t size   = 0;
    esp_err_t err = nvs_get_blob(handle, key->c_str(), nullptr, &size);
    std::vector<uint8_t> blob;
    if (err == ESP_OK && size > sizeof(BlobHeader)) {
        blob.resize(size);
        err = nvs_get_blob(handle, key->c_str(), blob.data(), &size);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }

    BlobHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    size_t payload = size - sizeof(header);
    if (header.magic != BLOB_MAGIC || header.version != BLOB_VERSION || payload % sizeof(uint16_t) != 0) {
        mclog::tagError(_tag, "'{}' has an invalid format", *key);
        return false;
    }

    out.carrierHz = header.carrierHz;
    out.durations.resize(payload / sizeof(uint16_t));
    std::memcpy(out.durations.data(), blob.data() + sizeof(header), payload);
    return true;
}

bool delete_code(const std::string& name)
{
    auto key = normalize_name(name);
    if (!key) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_erase_key(handle, key->c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

std::vector<std::string> list_codes()
{
    std::vector<std::string> names;
    nvs_iterator_t it = nullptr;
    esp_err_t err     = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        names.emplace_back(info.key);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    std::sort(names.begin(), names.end());
    return names;
}

/* -------------------------------------------------------------------------- */
/*                                  Learning                                  */
/* -------------------------------------------------------------------------- */

static void set_learn_status(LearnState state, const std::string& name, const std::string& detail)
{
    std::lock_guard<std::mutex> lock(s_learn_mutex);
    s_learn_status = {state, name, detail};
}

// One persistent learner task with its stack in PSRAM: while the AI agent runs,
// internal SRAM is nearly exhausted (~7 KB free), so a new internal-stack task
// cannot be created. A PSRAM-stack task must not touch flash, so it only captures;
// saving to NVS is done by the caller's callback on another thread.
struct LearnRequest {
    std::string name;
    uint32_t timeoutMs = 0;
    CaptureCallback onCaptured;
};

static QueueHandle_t s_learn_queue = nullptr;
static TaskHandle_t s_learn_task   = nullptr;

static void learn_task(void*)
{
    while (true) {
        LearnRequest* req = nullptr;
        if (xQueueReceive(s_learn_queue, &req, portMAX_DELAY) != pdTRUE || !req) {
            continue;
        }

        IrCode code;
        bool captured = capture(code, req->timeoutMs);
        mclog::tagInfo(_tag, "learn '{}': captured={}", req->name, captured);

        if (req->onCaptured) {
            req->onCaptured(req->name, captured, code);
        } else {
            complete_learning(req->name, captured, code);
        }
        delete req;
    }
}

static bool ensure_learn_task()
{
    if (s_learn_task) {
        return true;
    }
    if (!s_learn_queue) {
        s_learn_queue = xQueueCreate(1, sizeof(LearnRequest*));
        if (!s_learn_queue) {
            s_last_error = "create learn queue failed: ESP_ERR_NO_MEM";
            return false;
        }
    }
    if (xTaskCreatePinnedToCoreWithCaps(learn_task, "ir_learn", 6144, nullptr, 4, &s_learn_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_learn_task = nullptr;
        s_last_error = "create learn task (PSRAM stack) failed";
        mclog::tagError(_tag, "{}", s_last_error);
        return false;
    }
    return true;
}

bool complete_learning(const std::string& name, bool captured, const IrCode& code)
{
    bool ok = captured;
    std::string detail;

    if (!captured) {
        detail = "No IR signal received before timeout";
    } else if (!save_code(name, code)) {
        ok     = false;
        detail = "Signal received but saving failed (storage may be full)";
    } else if (auto nec = decode_nec(code.durations)) {
        detail = fmt::format("Saved NEC code (address 0x{:X}, command 0x{:X})", nec->address, nec->command);
    } else {
        detail = fmt::format("Saved raw code ({} pulses)", code.durations.size());
    }

    set_learn_status(ok ? LearnState::Success : LearnState::Failed, name, detail);
    mclog::tagInfo(_tag, "learn '{}': {}", name, detail);
    return ok;
}

bool start_learning(const std::string& name, uint32_t timeoutMs, CaptureCallback onCaptured)
{
    auto key = normalize_name(name);
    if (!key || !init()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(s_learn_mutex);
        if (s_learn_status.state == LearnState::Listening) {
            return false;
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(s_hw_mutex);
        if (!ensure_learn_task()) {
            return false;
        }
    }

    set_learn_status(LearnState::Listening, *key, "Waiting for IR signal");
    auto* req = new LearnRequest{*key, timeoutMs, std::move(onCaptured)};
    if (xQueueSend(s_learn_queue, &req, 0) != pdTRUE) {
        delete req;
        set_learn_status(LearnState::Failed, *key, "Learner busy");
        return false;
    }
    return true;
}

LearnStatus get_learn_status()
{
    std::lock_guard<std::mutex> lock(s_learn_mutex);
    return s_learn_status;
}

}  // namespace ir_remote
