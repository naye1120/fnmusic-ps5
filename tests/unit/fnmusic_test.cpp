// ps5-homebrew-ui - Behaviour tests for the 飞牛音乐 design.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The setup page is the only place a player can type, so its rules are read
// from what the design asks to hear: a `type` cue is a character that reached
// the field, and the refusal on 登录 says the sign-in check ran.

#include "concept_fixture.hpp"
#include "concepts/concepts.hpp"

#include <memory>

namespace
{

using hui::Action;
using hui::Direction;
using hui::audio::Cue;

class Fnmusic : public hui::testing::ConceptFixture
{
  protected:
    Fnmusic() : design_(hui::concepts::make_fnmusic(context_))
    {
        design_->enter();
        // Long enough for the splash to lift on its own: these tests are about
        // the fields, not about the launch sequence.
        send(*design_, idle(), 3.0f);
    }

    // Types one character into the field the focus is on, then walks to the
    // on-screen 完成 key and presses it: the way a player closes a field.
    void type_and_leave_with_the_done_key()
    {
        send(*design_, press(Action::confirm), 0.2f); // the row opens the keyboard
        send(*design_, press(Action::confirm), 0.2f); // the focused key types
        EXPECT_TRUE(played(Cue::type)) << "the keyboard must type, not close";
        for (int row = 0; row < 4; ++row)
            send(*design_, nav(Direction::down), 0.1f);
        for (int key = 0; key < 4; ++key)
            send(*design_, nav(Direction::right), 0.1f);
        send(*design_, press(Action::confirm), 0.2f); // 完成
    }

    std::unique_ptr<hui::app::Concept> design_;
};

// Every field starts on a letter key. The keyboard kept the focus of the last
// edit - the 完成 key - so the next field answered ✕ with "closed", and a
// player could never type into the second, third or fourth field.
TEST_F(Fnmusic, EachFieldTypesIntoItsOwnBuffer)
{
    type_and_leave_with_the_done_key(); // 服务器
    send(*design_, nav(Direction::down), 0.2f);
    send(*design_, nav(Direction::down), 0.2f); // 账号, 密码
    type_and_leave_with_the_done_key();
    expect_drawable(*design_);
}

// Options is the shortcut for 完成: no player should have to reach the key.
TEST_F(Fnmusic, OptionsClosesTheKeyboard)
{
    send(*design_, press(Action::confirm), 0.2f); // 服务器 opens the keyboard
    send(*design_, press(Action::menu), 0.2f);    // closes it again
    // Square is the keyboard's delete. Back on the list it changes the page
    // instead, and that is the cue which proves the keyboard really closed.
    send(*design_, press(Action::west), 0.2f);
    EXPECT_TRUE(played(Cue::tab));
    expect_drawable(*design_);
}

// A field left empty is not a sign-in: 登录 refuses while server, account or
// password is missing.
TEST_F(Fnmusic, AnEmptyFieldIsNotASignIn)
{
    for (int row = 0; row < 3; ++row)
        send(*design_, nav(Direction::down), 0.2f); // 账号, 密码, 登录
    send(*design_, press(Action::confirm), 0.2f);
    EXPECT_TRUE(played(Cue::error));
    expect_drawable(*design_);
}

// 登录 is answered by what the row is, not by which line it sits on. Filling
// the three fields and pressing ✕ must reach the library - the refusal above
// and this one together mean the button works both ways.
TEST_F(Fnmusic, AFilledFormSignInForReal)
{
    type_and_leave_with_the_done_key(); // 服务器
    send(*design_, nav(Direction::down), 0.2f);
    type_and_leave_with_the_done_key(); // 账号
    send(*design_, nav(Direction::down), 0.2f);
    type_and_leave_with_the_done_key(); // 密码
    send(*design_, nav(Direction::down), 0.2f);
    send(*design_, press(Action::confirm), 0.2f); // 登录
    EXPECT_TRUE(played(Cue::launch)) << "a filled form has to reach the library";
    expect_drawable(*design_);
}

} // namespace
