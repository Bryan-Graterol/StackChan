/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
// Infrared remote support for the StackChan body (IR_SEND = GPIO5, IR_REC = GPIO10).
// Sends NEC / raw codes through the RMT peripheral, learns codes from a real remote
// and stores them by name in NVS so the AI agent can use them through MCP tools.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ir_remote {

// A raw IR code: alternating mark/space durations in microseconds, starting with a mark.
struct IrCode {
    uint32_t carrierHz = 38000;
    std::vector<uint16_t> durations;
};

struct NecFrame {
    uint16_t address = 0;  // 8-bit address (0-255) or 16-bit extended address
    uint8_t command  = 0;
};

enum class LearnState { Idle, Listening, Success, Failed };

struct LearnStatus {
    LearnState state = LearnState::Idle;
    std::string name;
    std::string detail;  // Decoded protocol summary or failure reason
};

// Lazily initialises RMT TX/RX channels. Safe to call multiple times.
bool init();
// Human-readable reason for the last init failure (empty when OK).
std::string last_error();

// Transmission
bool send_raw(const IrCode& code, int repeat = 1);
bool send_nec(const NecFrame& frame, int repeat = 1);
std::optional<NecFrame> decode_nec(const std::vector<uint16_t>& durations);

// Blocking capture of a single IR frame from the receiver.
bool capture(IrCode& out, uint32_t timeoutMs);

// Called from the learner task (PSRAM stack: must NOT write flash) once a capture
// finishes. Hand the result to a thread with an internal stack and call
// complete_learning() there to save it.
using CaptureCallback = std::function<void(const std::string& name, bool captured, const IrCode& code)>;

// Non-blocking learning: a background task waits for a code from a real remote.
// Returns false if a learn is already in progress, the name is invalid or the
// hardware failed (see last_error()).
bool start_learning(const std::string& name, uint32_t timeoutMs, CaptureCallback onCaptured = nullptr);
// Saves a captured code and updates the learn status. Writes NVS: call it from a
// task whose stack is in internal RAM. Returns true when the code was saved.
bool complete_learning(const std::string& name, bool captured, const IrCode& code);
LearnStatus get_learn_status();

// Storage (NVS namespace "ir_codes"). Names: 1-15 chars, [a-z0-9_].
std::optional<std::string> normalize_name(const std::string& name);
bool save_code(const std::string& name, const IrCode& code);
bool load_code(const std::string& name, IrCode& out);
bool delete_code(const std::string& name);
std::vector<std::string> list_codes();

}  // namespace ir_remote
