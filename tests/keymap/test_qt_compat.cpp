// Compile-time proof that the keymap's Qt-free key constants equal Qt's, so the app can pass QKeyEvent::key()
// and modifiers() straight through. Only this test includes Qt; the keymap module itself never does.

#include <wabcut/keymap/Key.h>

#include <QtCore/qnamespace.h>

#include <catch2/catch_test_macros.hpp>

namespace km = wabcut::keymap;

#define WABCUT_SAME_KEY(name) static_assert(km::name == static_cast<int>(Qt::name), #name " differs from Qt")
WABCUT_SAME_KEY(Key_Space);
WABCUT_SAME_KEY(Key_Exclam);
WABCUT_SAME_KEY(Key_QuoteDbl);
WABCUT_SAME_KEY(Key_NumberSign);
WABCUT_SAME_KEY(Key_Dollar);
WABCUT_SAME_KEY(Key_Percent);
WABCUT_SAME_KEY(Key_Ampersand);
WABCUT_SAME_KEY(Key_Apostrophe);
WABCUT_SAME_KEY(Key_ParenLeft);
WABCUT_SAME_KEY(Key_ParenRight);
WABCUT_SAME_KEY(Key_Asterisk);
WABCUT_SAME_KEY(Key_Plus);
WABCUT_SAME_KEY(Key_Comma);
WABCUT_SAME_KEY(Key_Minus);
WABCUT_SAME_KEY(Key_Period);
WABCUT_SAME_KEY(Key_Slash);
WABCUT_SAME_KEY(Key_0);
WABCUT_SAME_KEY(Key_1);
WABCUT_SAME_KEY(Key_5);
WABCUT_SAME_KEY(Key_9);
WABCUT_SAME_KEY(Key_Colon);
WABCUT_SAME_KEY(Key_Semicolon);
WABCUT_SAME_KEY(Key_Less);
WABCUT_SAME_KEY(Key_Equal);
WABCUT_SAME_KEY(Key_Greater);
WABCUT_SAME_KEY(Key_Question);
WABCUT_SAME_KEY(Key_At);
WABCUT_SAME_KEY(Key_A);
WABCUT_SAME_KEY(Key_K);
WABCUT_SAME_KEY(Key_Z);
WABCUT_SAME_KEY(Key_BracketLeft);
WABCUT_SAME_KEY(Key_Backslash);
WABCUT_SAME_KEY(Key_BracketRight);
WABCUT_SAME_KEY(Key_AsciiCircum);
WABCUT_SAME_KEY(Key_Underscore);
WABCUT_SAME_KEY(Key_QuoteLeft);
WABCUT_SAME_KEY(Key_BraceLeft);
WABCUT_SAME_KEY(Key_Bar);
WABCUT_SAME_KEY(Key_BraceRight);
WABCUT_SAME_KEY(Key_AsciiTilde);
WABCUT_SAME_KEY(Key_Escape);
WABCUT_SAME_KEY(Key_Tab);
WABCUT_SAME_KEY(Key_Backtab);
WABCUT_SAME_KEY(Key_Backspace);
WABCUT_SAME_KEY(Key_Return);
WABCUT_SAME_KEY(Key_Enter);
WABCUT_SAME_KEY(Key_Insert);
WABCUT_SAME_KEY(Key_Delete);
WABCUT_SAME_KEY(Key_Pause);
WABCUT_SAME_KEY(Key_Print);
WABCUT_SAME_KEY(Key_Clear);
WABCUT_SAME_KEY(Key_Home);
WABCUT_SAME_KEY(Key_End);
WABCUT_SAME_KEY(Key_Left);
WABCUT_SAME_KEY(Key_Up);
WABCUT_SAME_KEY(Key_Right);
WABCUT_SAME_KEY(Key_Down);
WABCUT_SAME_KEY(Key_PageUp);
WABCUT_SAME_KEY(Key_PageDown);
WABCUT_SAME_KEY(Key_Shift);
WABCUT_SAME_KEY(Key_Control);
WABCUT_SAME_KEY(Key_Meta);
WABCUT_SAME_KEY(Key_Alt);
WABCUT_SAME_KEY(Key_AltGr);
WABCUT_SAME_KEY(Key_CapsLock);
WABCUT_SAME_KEY(Key_NumLock);
WABCUT_SAME_KEY(Key_ScrollLock);
WABCUT_SAME_KEY(Key_F1);
WABCUT_SAME_KEY(Key_F12);
WABCUT_SAME_KEY(Key_F35);
WABCUT_SAME_KEY(Key_unknown);
#undef WABCUT_SAME_KEY

static_assert(km::functionKey(24) == static_cast<int>(Qt::Key_F24));

#define WABCUT_SAME_MODIFIER(name, qt)                                                                       \
    static_assert(static_cast<std::uint32_t>(km::Modifier::name) == static_cast<std::uint32_t>(Qt::qt),      \
                  #name " differs from Qt")
WABCUT_SAME_MODIFIER(None, NoModifier);
WABCUT_SAME_MODIFIER(Shift, ShiftModifier);
WABCUT_SAME_MODIFIER(Control, ControlModifier);
WABCUT_SAME_MODIFIER(Alt, AltModifier);
WABCUT_SAME_MODIFIER(Meta, MetaModifier);
WABCUT_SAME_MODIFIER(Keypad, KeypadModifier);
WABCUT_SAME_MODIFIER(GroupSwitch, GroupSwitchModifier);
#undef WABCUT_SAME_MODIFIER

static_assert(km::Modifiers::kMask == (static_cast<std::uint32_t>(Qt::KeyboardModifierMask) & ~0x80000000u));

TEST_CASE("A Qt key event converts losslessly", "[keymap][qt]") {
    const Qt::KeyboardModifiers qtMods = Qt::ControlModifier | Qt::ShiftModifier | Qt::KeypadModifier;
    const km::KeyChord chord = km::KeyChord::fromQt(Qt::Key_K, static_cast<std::uint32_t>(qtMods.toInt()));
    CHECK(chord.key == Qt::Key_K);
    CHECK(chord.mods.bits() == static_cast<std::uint32_t>(qtMods.toInt()));
    CHECK(chord.mods.has(km::Modifier::Keypad));
}
