/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include "ir/ir_remote.h"
#include <application.h>

using namespace stackchan;

static const std::string_view _tag = "HAL-MCP";

static void set_neon_color(uint8_t r, uint8_t g, uint8_t b)
{
    LvglLockGuard lock;
    GetStackChan().leftNeonLight().setColor(r, g, b);
    GetStackChan().rightNeonLight().setColor(r, g, b);
}

static std::string json_escape(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

static void ir_mcp_init(McpServer& mcp_server)
{
    // System Prompt hint:
    // You can control TVs, air conditioners and other appliances with infrared. Use self.ir.list_codes to see which
    // buttons are saved, self.ir.send_code to press one, and self.ir.learn_code to teach a new one.

    mclog::tagInfo(_tag, "add ir.list_codes tool");
    mcp_server.AddTool("self.ir.list_codes",
                       "List the infrared (IR) remote buttons the robot has learned, e.g. [\"tv_power\",\"ac_on\"]. "
                       "Call this first to know which appliances (TV, air conditioner, fan, lights...) you can control.",
                       std::vector<Property>{}, [](const PropertyList& properties) -> ReturnValue {
                           auto names       = ir_remote::list_codes();
                           std::string json = "[";
                           for (size_t i = 0; i < names.size(); ++i) {
                               json += fmt::format(R"("{}")", json_escape(names[i]));
                               if (i + 1 < names.size()) {
                                   json += ",";
                               }
                           }
                           json += "]";
                           mclog::tagInfo(_tag, "ir.list_codes: {}", json);
                           return json;
                       });

    mclog::tagInfo(_tag, "add ir.send_code tool");
    mcp_server.AddTool("self.ir.send_code",
                       "Press a learned IR remote button to control an appliance (TV, air conditioner, fan...). "
                       "`name` must be one returned by self.ir.list_codes. `repeat` sends the button several times "
                       "(e.g. volume up 3 steps = repeat 3). The appliance must be in front of the robot.",
                       PropertyList({Property("name", kPropertyTypeString),
                                     Property("repeat", kPropertyTypeInteger, 1, 1, 10)}),
                       [](const PropertyList& properties) -> ToolResult {
                           auto name  = properties["name"].value<std::string>();
                           int repeat = properties["repeat"].value<int>();
                           mclog::tagInfo(_tag, "ir.send_code: {} x{}", name, repeat);

                           ir_remote::IrCode code;
                           if (!ir_remote::load_code(name, code)) {
                               return std::unexpected(fmt::format(
                                   "IR code '{}' not found. Use self.ir.list_codes or learn it first.", name));
                           }
                           if (!ir_remote::send_raw(code, repeat)) {
                               return std::unexpected(fmt::format("Failed to transmit IR signal. {}", ir_remote::last_error()));
                           }
                           return true;
                       });

    mclog::tagInfo(_tag, "add ir.learn_code tool");
    mcp_server.AddTool(
        "self.ir.learn_code",
        "Learn (record) a button from the user's real IR remote and save it under `name` (short, lowercase, "
        "e.g. tv_power, tv_vol_up, ac_on). This returns immediately and the robot listens in the background for "
        "`timeout_seconds` while its LEDs glow blue. After calling it, tell the user to point their remote at the "
        "robot and press the button. LEDs turn green when saved or red on failure. Then call "
        "self.ir.get_learn_status to confirm the result. Learning an existing name overwrites it.",
        PropertyList({Property("name", kPropertyTypeString),
                      Property("timeout_seconds", kPropertyTypeInteger, 15, 5, 30)}),
        [](const PropertyList& properties) -> ToolResult {
            auto name   = properties["name"].value<std::string>();
            int timeout = properties["timeout_seconds"].value<int>();

            auto key = ir_remote::normalize_name(name);
            if (!key) {
                return std::unexpected(std::string("Invalid name. Use letters, digits and underscores."));
            }
            mclog::tagInfo(_tag, "ir.learn_code: {} ({}s)", *key, timeout);

            auto current = ir_remote::get_learn_status();
            if (current.state == ir_remote::LearnState::Listening) {
                return fmt::format(
                    R"({{"status": "already_listening", "name": "{}", "hint": "The robot is still waiting for the user to press the remote button. Do not call learn_code again; call self.ir.get_learn_status later."}})",
                    json_escape(current.name));
            }

            if (!ir_remote::init()) {
                return std::unexpected(fmt::format("IR hardware init failed: {}", ir_remote::last_error()));
            }

            set_neon_color(0, 0, 168);
            // The callback runs on the learner task (PSRAM stack, no flash access):
            // hand the result to the main application thread to save it to NVS.
            bool started = ir_remote::start_learning(
                *key, timeout * 1000, [](const std::string& name, bool captured, const ir_remote::IrCode& code) {
                    Application::GetInstance().Schedule([name, captured, code]() {
                        bool ok = ir_remote::complete_learning(name, captured, code);

                        LvglLockGuard lock;
                        GetStackChan().leftNeonLight().setColor(ok ? 0 : 168, ok ? 168 : 0, 0);
                        GetStackChan().rightNeonLight().setColor(ok ? 0 : 168, ok ? 168 : 0, 0);

                        // Turn the LEDs off after 2 s (runs in the LVGL task, lock already held)
                        lv_timer_t* timer = lv_timer_create(
                            [](lv_timer_t*) {
                                GetStackChan().leftNeonLight().setColor(0, 0, 0);
                                GetStackChan().rightNeonLight().setColor(0, 0, 0);
                            },
                            2000, nullptr);
                        lv_timer_set_repeat_count(timer, 1);
                    });
                });

            if (!started) {
                set_neon_color(0, 0, 0);
                auto reason = ir_remote::last_error();
                return std::unexpected(fmt::format("Could not start IR learning. {}",
                                                   reason.empty() ? "Another learn is in progress." : reason));
            }
            return fmt::format(R"({{"status": "listening", "name": "{}", "timeout_seconds": {}}})",
                               json_escape(*key), timeout);
        });

    mclog::tagInfo(_tag, "add ir.get_learn_status tool");
    mcp_server.AddTool("self.ir.get_learn_status",
                       "Get the result of the last self.ir.learn_code call. state is one of: idle, listening, "
                       "success, failed.",
                       std::vector<Property>{}, [](const PropertyList& properties) -> ReturnValue {
                           auto status            = ir_remote::get_learn_status();
                           const char* state_name = "idle";
                           switch (status.state) {
                               case ir_remote::LearnState::Listening:
                                   state_name = "listening";
                                   break;
                               case ir_remote::LearnState::Success:
                                   state_name = "success";
                                   break;
                               case ir_remote::LearnState::Failed:
                                   state_name = "failed";
                                   break;
                               default:
                                   break;
                           }
                           return fmt::format(R"({{"state": "{}", "name": "{}", "detail": "{}"}})", state_name,
                                              json_escape(status.name), json_escape(status.detail));
                       });

    mclog::tagInfo(_tag, "add ir.delete_code tool");
    mcp_server.AddTool("self.ir.delete_code", "Delete a learned IR remote button by name.",
                       PropertyList({Property("name", kPropertyTypeString)}),
                       [](const PropertyList& properties) -> ToolResult {
                           auto name = properties["name"].value<std::string>();
                           mclog::tagInfo(_tag, "ir.delete_code: {}", name);
                           if (!ir_remote::delete_code(name)) {
                               return std::unexpected(fmt::format("IR code '{}' not found", name));
                           }
                           return true;
                       });

    mclog::tagInfo(_tag, "add ir.send_nec tool");
    mcp_server.AddTool("self.ir.send_nec",
                       "Advanced: send a raw NEC protocol IR command without learning it first. Only use when the "
                       "user gives an explicit address/command code.",
                       PropertyList({Property("address", kPropertyTypeInteger, 0, 0, 65535),
                                     Property("command", kPropertyTypeInteger, 0, 0, 255),
                                     Property("repeat", kPropertyTypeInteger, 1, 1, 10)}),
                       [](const PropertyList& properties) -> ToolResult {
                           ir_remote::NecFrame frame;
                           frame.address = static_cast<uint16_t>(properties["address"].value<int>());
                           frame.command = static_cast<uint8_t>(properties["command"].value<int>());
                           int repeat    = properties["repeat"].value<int>();
                           mclog::tagInfo(_tag, "ir.send_nec: addr=0x{:X} cmd=0x{:X} x{}", frame.address,
                                          frame.command, repeat);
                           if (!ir_remote::send_nec(frame, repeat)) {
                               return std::unexpected(fmt::format("Failed to transmit IR signal. {}", ir_remote::last_error()));
                           }
                           return true;
                       });
}

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    // System Prompt：
    // You can control the robot's head. Use get_yaw and get_pitch to sense current position. Use set_yaw for horizontal
    // movement and set_pitch for vertical movement. All angles are in degrees.

    mclog::tagInfo(_tag, "add robot.get_head_angles tool");
    mcp_server.AddTool("self.robot.get_head_angles",
                       "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           LvglLockGuard lock;  // StackChan motion update is under the lvgl lock

                           auto& motion      = GetStackChan().motion();
                           int current_yaw   = motion.yawServo().getCurrentAngle() / 10;
                           int current_pitch = motion.pitchServo().getCurrentAngle() / 10;

                           auto result = fmt::format(R"({{"yaw": {}, "pitch": {}}})", current_yaw, current_pitch);
                           mclog::tagInfo(_tag, "get_head_angles: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.set_head_angles tool");
    mcp_server.AddTool("self.robot.set_head_angles",
                       "Adjust head position. GUIDELINES: "
                       "1. For natural interaction, stay within +/- 45 degrees. "
                       "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
                       "3. Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up). "
                       "Speed(100-1000, 150 is natural).",
                       PropertyList({Property("yaw", kPropertyTypeInteger, -9999, -9999, 128),
                                     Property("pitch", kPropertyTypeInteger, -9999, -9999, 90),
                                     Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int speed = properties["speed"].value<int>();
                           int yaw   = properties["yaw"].value<int>();
                           int pitch = properties["pitch"].value<int>();

                           mclog::tagInfo(_tag, "motion set_angles: yaw: {}, pitch: {}, speed: {}", yaw, pitch, speed);

                           LvglLockGuard lock;

                           auto& motion = GetStackChan().motion();
                           if (pitch != -9999) {
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed);
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed);
                           }

                           return true;
                       });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
        "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color: r={}, g={}, b={}", r, g, b);

            LvglLockGuard lock;

            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add robot.create_reminder tool");
    mcp_server.AddTool("self.robot.create_reminder",
                       "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat "
                       "to true to repeat the reminder.",
                       PropertyList({Property("duration_seconds", kPropertyTypeInteger, 60, 1, 86400),
                                     Property("message", kPropertyTypeString, std::string("Time's up!")),
                                     Property("repeat", kPropertyTypeBoolean, false)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int duration_seconds = properties["duration_seconds"].value<int>();
                           std::string message  = properties["message"].value<std::string>();
                           bool repeat          = properties["repeat"].value<bool>();

                           // Default message
                           if (message.empty()) {
                               message = "Time's up!";
                           }

                           mclog::tagInfo(_tag, "create_reminder: duration={}s, message={}, repeat={}",
                                          duration_seconds, message, repeat);

                           int id = tools::create_reminder(duration_seconds * 1000, message, repeat);

                           return id;
                       });

    mclog::tagInfo(_tag, "add robot.get_reminders tool");
    mcp_server.AddTool("self.robot.get_reminders", "Get list of active reminders.", std::vector<Property>{},
                       [this](const PropertyList& properties) -> ReturnValue {
                           mclog::tagInfo(_tag, "get_reminders");
                           auto reminders          = tools::get_active_reminders();
                           std::string result_json = "[";
                           for (size_t i = 0; i < reminders.size(); ++i) {
                               const auto& r = reminders[i];
                               result_json +=
                                   fmt::format(R"({{"id": {}, "duration_ms": {}, "message": "{}", "repeat": {}}})",
                                               r.id, r.durationMs, r.message, r.repeat ? "true" : "false");
                               if (i < reminders.size() - 1) {
                                   result_json += ", ";
                               }
                           }
                           result_json += "]";
                           mclog::tagInfo(_tag, "get_reminders result: {}", result_json);
                           return result_json;
                       });

    mclog::tagInfo(_tag, "add robot.stop_reminder tool");
    mcp_server.AddTool("self.robot.stop_reminder", "Stop a reminder by ID.",
                       PropertyList({Property("id", kPropertyTypeInteger, -1)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id = properties["id"].value<int>();
                           mclog::tagInfo(_tag, "stop_reminder: id={}", id);
                           tools::stop_reminder(id);
                           return true;
                       });

    ir_mcp_init(mcp_server);
}
