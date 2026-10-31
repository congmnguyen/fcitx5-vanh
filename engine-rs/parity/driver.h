/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file driver.h
 * @brief Neutral interface of the parity harness: the former C++ engine (old_side.cpp, compiled
 *        with -Dlotus=lotus_old) and the Rust engine behind its C++ wrappers (new_side.cpp) are
 *        driven through the same calls, so neither side's headers meet the other's.
 */

#ifndef LOTUS_PARITY_DRIVER_H
#define LOTUS_PARITY_DRIVER_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace parity {

    /** Options of a processor, as plain values (w2u / bracket: -1 = follow the engine flags). */
    struct Opts {
        bool        autoNonVnRestore    = true;
        bool        ddFreeStyle         = true;
        bool        englishRestore      = false;
        bool        macroEnabled        = false;
        bool        autoCapitalizeMacro = false;
        bool        spellCheckWithDicts = true;
        bool        modernStyle         = true;
        bool        freeMarking         = true;
        int         charset             = 0;
        int         w2u                 = -1;
        int         bracket             = -1;
        std::string timeFormat          = "%H:%M";
        std::string dateFormat          = "%d/%m/%Y";
    };

    /** How a processor is built and what it shares. */
    struct Setup {
        int                                              inputMethod = 0; ///< Built-in id; 7 = Custom
        bool                                             parseCustom = false;
        std::map<std::string, std::string>               customDefinition;
        bool                                             spellDefault = true;
        bool                                             setOptions   = true; ///< false = keep the constructor defaults
        bool                                             rewriteAllowed = false; ///< setRewriteAllowed(true) right after construction
        bool                                             withMacros   = false;
        std::vector<std::pair<std::string, std::string>> macros;
        int                                              dictionary = 0; ///< 0 none, 1 loadFromWords, 2 loadFromText
        std::vector<std::string>                         dictWords;
        std::string                                      dictText;
    };

    /** One key processor of one side. */
    class Proc {
      public:
        virtual ~Proc()                                             = default;
        virtual void           setOptions(const Opts& opts)         = 0;
        virtual void           setMacroEnabled(bool enabled)        = 0;
        virtual void           setShouldRestoreKeyStrokes(bool on)  = 0;
        virtual bool           shouldRestoreKeyStrokes()            = 0;
        virtual bool           processKey(uint32_t key, uint32_t st) = 0;
        virtual bool           canProcessKey(uint32_t key)          = 0;
        virtual std::u32string preedit()                            = 0;
        virtual std::u32string takeCommit()                         = 0;
        virtual void           commitPreedit()                      = 0;
        virtual void           reset()                              = 0;
        virtual void           rebuildFromText(const std::u32string& text) = 0;
        virtual void           setRewriteAllowed(bool allowed)      = 0;
        virtual std::u32string takeRewrittenBefore()                = 0;
        virtual void           forgetContext()                      = 0;
        /** Re-shares the macro table / dictionary of the setup (or drops them). */
        virtual void           reshare(bool macros, bool dictionary) = 0;
    };

    /** Entry points of one side. */
    struct Side {
        std::unique_ptr<Proc> (*make)(const Setup& setup, const Opts& opts);
        std::string (*encode)(int charset, const std::u32string& text);
        std::string (*formatTime)(const std::string& format);
        bool (*isValidState)(uint32_t states);
        /** For each word: bit 0 = hasWord, bit 1 = hasWordOrPrefix; the last entry is the size. */
        std::vector<std::size_t> (*dictionaryProbe)(const Setup& setup, const std::vector<std::u32string>& words);
        std::vector<std::optional<std::u32string>> (*macroProbe)(const Setup& setup, const std::vector<std::u32string>& keys);
    };

    const Side& oldSide();
    const Side& newSide();

    /** Definitions of the built-in input methods (from the former C++ data). */
    std::vector<std::map<std::string, std::string>> builtinDefinitions();

} // namespace parity

#endif // LOTUS_PARITY_DRIVER_H
