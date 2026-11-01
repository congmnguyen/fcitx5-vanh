/*
 * SPDX-FileCopyrightText: 2026 kimxuanhong <kimxuanhong@outlook.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "window_focus_tracker.h"
#include "lotus-utils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>

#include <dbus/dbus.h>
#include <unistd.h>

namespace fcitx {
    namespace {
        constexpr int kMaxAccessibleNodes = 256;  ///< Children walked while priming the tree
        constexpr int kShortTimeoutMs     = 150;  ///< Ping / GetAddress round trip
        constexpr int kDefaultTimeoutMs   = 500;  ///< GetAttributes / GetRelationSet
        constexpr int kPokeTimeoutMs      = 2000; ///< Registry GetChildren
        constexpr int kReadWriteTimeoutMs = 50;   ///< Signal poll interval
        constexpr int kReconnectDelayMs   = 100;  ///< Delay between reconnect attempts
        constexpr int kReconnectAttempts  = 20;   ///< Attempts before the bus is retried
        constexpr int kRedialAttempts     = 10;   ///< Attempts after a bus loss
        constexpr std::chrono::seconds kPokeInterval{15}; ///< Accessibility tree priming period

        /// Releases a libdbus connection; private connections must be closed before unref.
        struct ConnectionDeleter {
            bool isPrivate = false;

            void operator()(DBusConnection* connection) const noexcept {
                if (connection == nullptr) {
                    return;
                }
                if (isPrivate) {
                    dbus_connection_close(connection);
                }
                dbus_connection_unref(connection);
            }
        };

        struct MessageDeleter {
            void operator()(DBusMessage* message) const noexcept {
                if (message != nullptr) {
                    dbus_message_unref(message);
                }
            }
        };

        using ConnectionPtr = std::unique_ptr<DBusConnection, ConnectionDeleter>;
        using MessagePtr    = std::unique_ptr<DBusMessage, MessageDeleter>;

        /// Calls a D-Bus method and waits for its reply; null when the call failed.
        MessagePtr callMethod(DBusConnection* bus, const char* destination, const char* path,
            const char* interface, const char* method, std::string_view argument, int timeoutMs) {
            MessagePtr request(dbus_message_new_method_call(destination, path, interface, method));
            if (!request) {
                return nullptr;
            }
            if (!argument.empty()) {
                const std::string value{argument};
                const char*       raw = value.c_str();
                if (dbus_message_append_args(
                        request.get(), DBUS_TYPE_STRING, &raw, DBUS_TYPE_INVALID) == FALSE) {
                    return nullptr;
                }
            }
            DBusError error;
            dbus_error_init(&error);
            MessagePtr reply(
                dbus_connection_send_with_reply_and_block(bus, request.get(), timeoutMs, &error));
            const bool failed = dbus_error_is_set(&error) != FALSE;
            dbus_error_free(&error);
            return failed ? nullptr : std::move(reply);
        }

        bool pingRegistry(DBusConnection* bus) {
            return callMethod(bus, "org.a11y.atspi.Registry", "/org/a11y/atspi/registry",
                       "org.freedesktop.DBus.Peer", "Ping", {}, kShortTimeoutMs) != nullptr;
        }

        /// Opens the AT-SPI bus, falling back to a private session bus connection.
        ConnectionPtr connectAtspiBus() {
            // Address: the environment, the runtime dir, then the a11y service.
            std::string       address    = getEnv("AT_SPI_BUS_ADDRESS");
            const std::string runtimeDir = getEnv("XDG_RUNTIME_DIR");
            const std::string socketPath = runtimeDir + "/at-spi/bus_0";
            if (address.empty() && !runtimeDir.empty() && access(socketPath.c_str(), F_OK) == 0) {
                address = "unix:path=" + socketPath;
            }
            if (address.empty()) {
                DBusError error;
                dbus_error_init(&error);
                ConnectionPtr session(dbus_bus_get(DBUS_BUS_SESSION, &error), ConnectionDeleter{});
                if (session && dbus_error_is_set(&error) == FALSE) {
                    MessagePtr reply = callMethod(session.get(), "org.a11y.Bus", "/org/a11y/bus",
                        "org.a11y.Bus", "GetAddress", {}, kShortTimeoutMs);
                    char*      value = nullptr;
                    DBusError  argsError;
                    dbus_error_init(&argsError);
                    if (reply &&
                        dbus_message_get_args(reply.get(), &argsError, DBUS_TYPE_STRING, &value,
                            DBUS_TYPE_INVALID) != FALSE &&
                        value != nullptr) {
                        address = value;
                    }
                    dbus_error_free(&argsError);
                }
                dbus_error_free(&error);
            }

            DBusError error;
            if (!address.empty()) {
                dbus_error_init(&error);
                ConnectionPtr bus(
                    dbus_connection_open(address.c_str(), &error), ConnectionDeleter{});
                if (bus && dbus_error_is_set(&error) == FALSE &&
                    dbus_bus_register(bus.get(), &error) != FALSE &&
                    dbus_error_is_set(&error) == FALSE && pingRegistry(bus.get())) {
                    dbus_error_free(&error);
                    dbus_connection_set_exit_on_disconnect(bus.get(), FALSE);
                    return bus;
                }
                dbus_error_free(&error);
            }

            dbus_error_init(&error);
            ConnectionPtr bus(dbus_bus_get_private(DBUS_BUS_SESSION, &error),
                ConnectionDeleter{.isPrivate = true});
            if (bus && dbus_error_is_set(&error) == FALSE && pingRegistry(bus.get())) {
                dbus_error_free(&error);
                dbus_connection_set_exit_on_disconnect(bus.get(), FALSE);
                return bus;
            }
            dbus_error_free(&error);
            return nullptr;
        }

        /// Walks the accessibility tree once so Chromium exposes its address bar node.
        void pokeAccessibleApps(DBusConnection* bus) {
            MessagePtr reply =
                callMethod(bus, "org.a11y.atspi.Registry", "/org/a11y/atspi/accessible/root",
                    "org.a11y.atspi.Accessible", "GetChildren", {}, kPokeTimeoutMs);
            if (!reply) {
                return;
            }
            DBusMessageIter iter{};
            if (dbus_message_iter_init(reply.get(), &iter) == FALSE ||
                dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
                return;
            }
            DBusMessageIter children{};
            dbus_message_iter_recurse(&iter, &children);
            for (int count = 0; count < kMaxAccessibleNodes &&
                dbus_message_iter_get_arg_type(&children) == DBUS_TYPE_STRUCT;
                ++count) {
                DBusMessageIter child{};
                dbus_message_iter_recurse(&children, &child);
                char* appBus  = nullptr;
                char* appPath = nullptr;
                if (dbus_message_iter_get_arg_type(&child) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&child, reinterpret_cast<void*>(&appBus));
                    dbus_message_iter_next(&child);
                    if (dbus_message_iter_get_arg_type(&child) == DBUS_TYPE_OBJECT_PATH) {
                        dbus_message_iter_get_basic(&child, reinterpret_cast<void*>(&appPath));
                    }
                }
                if (appBus != nullptr && appPath != nullptr &&
                    std::string_view(appPath).starts_with('/')) {
                    callMethod(bus, appBus, appPath, "org.a11y.atspi.Accessible", "GetRelationSet",
                        {}, kDefaultTimeoutMs);
                }
                dbus_message_iter_next(&children);
            }
        }

        /// Registers one AT-SPI event name.
        bool registerEvent(DBusConnection* bus, const char* eventName) {
            return callMethod(bus, "org.a11y.atspi.Registry", "/org/a11y/atspi/registry",
                       "org.a11y.atspi.Registry", "RegisterEvent", eventName,
                       kPokeTimeoutMs) != nullptr;
        }

        /// Adds one signal match rule.
        bool addMatch(DBusConnection* bus, const char* rule) {
            DBusError error;
            dbus_error_init(&error);
            dbus_bus_add_match(bus, rule, &error);
            const bool ok = dbus_error_is_set(&error) == FALSE;
            dbus_error_free(&error);
            return ok;
        }
    } // namespace

    WindowFocusTracker::~WindowFocusTracker() {
        stop();
    }

    void WindowFocusTracker::start() {
        if (thread_.joinable()) {
            return;
        }
        stopRequested_.store(false, std::memory_order_release);
        thread_ = std::thread(&WindowFocusTracker::threadFunc, this);
    }

    void WindowFocusTracker::stop() {
        stopRequested_.store(true, std::memory_order_release);
        browserUIFocused_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void WindowFocusTracker::threadFunc() {
        // libdbus threading is required because fcitx also uses dbus.
        static const bool initialised = dbus_threads_init_default() != FALSE;
        (void)initialised;
        const auto waitBeforeRetry = [this](int attempts) {
            for (int retry = 0; retry < attempts && !stopRequested_.load(std::memory_order_acquire);
                ++retry) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
            }
        };
        while (!stopRequested_.load(std::memory_order_acquire)) {
            ConnectionPtr bus = connectAtspiBus();
            if (!bus ||
                !(registerEvent(bus.get(), "object:state-changed:focused") &&
                    registerEvent(bus.get(), "focus:") &&
                    addMatch(bus.get(),
                        "type='signal',interface='org.a11y.atspi.Event.Object',"
                        "member='StateChanged'") &&
                    addMatch(bus.get(), "type='signal',interface='org.a11y.atspi.Event.Focus'"))) {
                browserUIFocused_.store(false, std::memory_order_relaxed);
                waitBeforeRetry(kReconnectAttempts);
                continue;
            }
            std::string focusBus;
            std::string focusPath;
            pokeAccessibleApps(bus.get());
            auto nextAccessibilityPoke = std::chrono::steady_clock::now() + kPokeInterval;
            while (!stopRequested_.load(std::memory_order_acquire) &&
                dbus_connection_read_write(bus.get(), kReadWriteTimeoutMs) != FALSE) {
                while (MessagePtr message{dbus_connection_pop_message(bus.get())}) {
                    // Classify the signal as a focus event; skip everything else.
                    const char* interface = dbus_message_get_interface(message.get());
                    const char* member    = dbus_message_get_member(message.get());
                    if (interface == nullptr || member == nullptr) {
                        continue;
                    }
                    bool focused = true;
                    if (std::string_view(interface) != "org.a11y.atspi.Event.Focus") {
                        if (std::string_view(interface) != "org.a11y.atspi.Event.Object" ||
                            std::string_view(member) != "StateChanged") {
                            continue;
                        }
                        DBusMessageIter iter{};
                        if (dbus_message_iter_init(message.get(), &iter) == FALSE ||
                            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
                            continue;
                        }
                        char* state = nullptr;
                        dbus_message_iter_get_basic(&iter, reinterpret_cast<void*>(&state));
                        if (state == nullptr || std::string_view(state) != "focused") {
                            continue;
                        }
                        dbus_message_iter_next(&iter);
                        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INT32) {
                            continue;
                        }
                        dbus_int32_t focusState = 0;
                        dbus_message_iter_get_basic(&iter, &focusState);
                        focused = focusState == 1;
                    }

                    const char* sender = dbus_message_get_sender(message.get());
                    const char* path   = dbus_message_get_path(message.get());
                    if (!focused) {
                        if (sender != nullptr && focusBus == sender &&
                            (path == nullptr || focusPath == path)) {
                            browserUIFocused_.store(false, std::memory_order_relaxed);
                            focusBus.clear();
                            focusPath.clear();
                        }
                        continue;
                    }
                    if (sender == nullptr || path == nullptr) {
                        continue;
                    }
                    focusBus  = sender;
                    focusPath = path;

                    // Executable name of the process owning the focused node.
                    std::string   program;
                    MessagePtr    pidReply = callMethod(bus.get(), "org.freedesktop.DBus",
                           "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "GetConnectionUnixProcessID", sender, kDefaultTimeoutMs);
                    dbus_uint32_t pid      = 0;
                    DBusError     error;
                    dbus_error_init(&error);
                    if (pidReply &&
                        dbus_message_get_args(pidReply.get(), &error, DBUS_TYPE_UINT32, &pid,
                            DBUS_TYPE_INVALID) != FALSE &&
                        static_cast<int>(pid) > 0) {
                        std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
                        std::getline(comm, program);
                        program.erase(std::min(program.find('\r'), program.size()));
                    }
                    dbus_error_free(&error);

                    // The "class" attribute of the focused node.
                    std::string     className;
                    MessagePtr      attributes = isChromiumProgram(program) ?
                             callMethod(bus.get(), sender, path, "org.a11y.atspi.Accessible",
                                 "GetAttributes", {}, kDefaultTimeoutMs) :
                             nullptr;
                    DBusMessageIter iter{};
                    DBusMessageIter entries{};
                    if (attributes &&
                        dbus_message_has_signature(attributes.get(), "a{ss}") != FALSE &&
                        dbus_message_iter_init(attributes.get(), &iter) != FALSE) {
                        dbus_message_iter_recurse(&iter, &entries);
                        while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
                            DBusMessageIter entry{};
                            dbus_message_iter_recurse(&entries, &entry);
                            char* key   = nullptr;
                            char* value = nullptr;
                            dbus_message_iter_get_basic(&entry, reinterpret_cast<void*>(&key));
                            dbus_message_iter_next(&entry);
                            dbus_message_iter_get_basic(&entry, reinterpret_cast<void*>(&value));
                            if (key != nullptr && value != nullptr &&
                                std::string_view(key) == "class") {
                                className = value;
                                break;
                            }
                            dbus_message_iter_next(&entries);
                        }
                    }
                    browserUIFocused_.store(
                        className.find("Omnibox") != std::string::npos, std::memory_order_relaxed);
                }
                if (std::chrono::steady_clock::now() >= nextAccessibilityPoke) {
                    pokeAccessibleApps(bus.get());
                    nextAccessibilityPoke = std::chrono::steady_clock::now() + kPokeInterval;
                }
            }
            browserUIFocused_.store(false, std::memory_order_relaxed);
            waitBeforeRetry(kRedialAttempts);
        }
    }
} // namespace fcitx
