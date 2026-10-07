// ps5-homebrew-ui - Behaviour tests for the API client's session recovery.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NAS expires a `music-token` on its own schedule. What the player should
// not do then is put a keyboard back in the player's hands: the credentials it
// logged in with are kept, so the refused read is signed in again and asked
// once more. The preview host in src/fnos/http.cpp refuses the token value
// "preview-stale" with a 401 to make that state reachable without a server.

#include "fnos/api.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using hui::fnos::Api;
using hui::fnos::TrackPage;

// A kept account means an expired cookie is a retry, not a message.
TEST(FnosApi, ExpiredSessionIsRenewedAndTheShelfReadAgain)
{
    Api api;
    api.set_server("http://preview.local");
    std::string error;
    ASSERT_TRUE(api.login("admin", "hunter2", "0123456789abcdef0123456789abcdef", &error)) << error;
    const std::string issued = api.token();
    ASSERT_FALSE(issued.empty());

    api.set_token("preview-stale"); // the NAS dropped this console's session
    TrackPage page;
    ASSERT_TRUE(api.tracks(1, &page, &error)) << error;
    EXPECT_FALSE(page.items.empty());
    EXPECT_EQ(api.token(), issued) << "the renewed cookie is the one on offer";
}

// A client that never logged in has no account to renew with: the refusal is
// the message, and no request is signed back in behind the player's back.
TEST(FnosApi, RefusalWithoutASessionIsSaidNotRenewed)
{
    Api cold;
    cold.set_server("http://preview.local");
    cold.set_token("preview-stale");
    TrackPage page;
    std::string error;
    EXPECT_FALSE(cold.tracks(1, &page, &error));
    EXPECT_NE(error.find("登录已失效"), std::string::npos) << error;
}

} // namespace
