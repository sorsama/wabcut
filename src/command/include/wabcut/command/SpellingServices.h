#pragma once

// What the spell-check commands need from the host (0.2 graphics, model/Spelling.h): its spell checker with the user's
// dictionary. The app and wabcut-cli give the operating system's checker (editor/SystemSpellChecker.h) under the user's
// PersonalSpelling; without these services captions.checkSpelling, graphic.checkSpelling and spelling.check are refused.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Spelling.h>

#include <memory>

namespace wabcut::command {

class SpellingServices : public CommandServices {
public:
    explicit SpellingServices(std::shared_ptr<const SpellChecker> checker) : m_checker(std::move(checker)) {}
    const SpellChecker& checker() const { return m_checker ? *m_checker : noSpellChecker(); }

private:
    std::shared_ptr<const SpellChecker> m_checker;
};

} // namespace wabcut::command
