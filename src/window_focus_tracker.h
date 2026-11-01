/*
 * SPDX-FileCopyrightText: 2026 kimxuanhong <kimxuanhong@outlook.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file window_focus_tracker.h
 * @brief Tracks the AT-SPI focus of Chromium based browsers to recognise their address bar.
 */

#ifndef _SRC_WINDOW_FOCUS_TRACKER_H_
#define _SRC_WINDOW_FOCUS_TRACKER_H_

#include <atomic>
#include <thread>

namespace fcitx {

    /**
     * @brief Watches AT-SPI focus events and reports whether a Chromium address bar is focused.
     */
    class WindowFocusTracker {
      public:
        WindowFocusTracker() = default;

        /**
         * @brief Stops the tracker thread.
         */
        ~WindowFocusTracker();

        WindowFocusTracker(const WindowFocusTracker&)            = delete;
        WindowFocusTracker& operator=(const WindowFocusTracker&) = delete;
        WindowFocusTracker(WindowFocusTracker&&)                 = delete;
        WindowFocusTracker& operator=(WindowFocusTracker&&)      = delete;

        /**
         * @brief Starts the tracker thread; does nothing when it already runs.
         */
        void start();

        /**
         * @brief Stops the tracker thread and clears the reported focus state.
         */
        void stop();

        /**
         * @brief Reports whether the focused accessibility node is a Chromium address bar.
         * @return True while the Omnibox of a Chromium browser holds the accessibility focus.
         */
        [[nodiscard]] bool isLikeAddressBarChromium() const {
            return browserUIFocused_.load(std::memory_order_relaxed);
        }

      private:
        /** @brief Thread body: connects to the AT-SPI bus and follows focus events. */
        void              threadFunc();

        std::thread       thread_;                  ///< Thread following the accessibility focus
        std::atomic<bool> stopRequested_{false};    ///< Set by stop() to end the thread
        std::atomic<bool> browserUIFocused_{false}; ///< True while an Omnibox holds the focus
    };

} // namespace fcitx

#endif // _SRC_WINDOW_FOCUS_TRACKER_H_
