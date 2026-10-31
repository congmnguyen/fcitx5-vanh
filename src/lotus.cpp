/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
/**
 * @file lotus.cpp
 * @brief Addon entry point of the Lotus input method.
 */
#include "lotus-engine.h"

namespace fcitx {

    /**
     * @brief Factory class for creating LotusEngine instances.
     */
    class LotusFactory : public AddonFactory {
      public:
        /**
         * @brief Creates a new LotusEngine instance.
         * @param manager Pointer to addon manager.
         * @return New engine instance.
         */
        AddonInstance* create(AddonManager* manager) override {
            registerDomain("fcitx5-vanh", FCITX_INSTALL_LOCALEDIR);
            return new LotusEngine(manager->instance()); // NOLINT
        }
    };

} // namespace fcitx

FCITX_ADDON_FACTORY(fcitx::LotusFactory)
