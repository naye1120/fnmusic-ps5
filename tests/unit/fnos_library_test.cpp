// ps5-homebrew-ui - Behaviour tests for the library worker's background reads.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// While a song is playing out, the queue asks for the address of the next one so
// the switch does not begin with a redirect round trip. The worker is a real
// thread, so each case below waits for its queue to drain. The preview host in
// src/fnos/http.cpp answers an address marked "preview-redirect" as if it had
// hopped to another host, which is the state being cached here.

#include "fnos/library.hpp"

#include "fnos/art.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unistd.h>

namespace
{

using hui::fnos::Library;

// Lets the worker take and finish what is queued, then reports whether it is
// idle again. The sleep between jobs is 40 ms, so this waits, it does not race.
bool drained(Library &library)
{
    for (int pass = 0; pass < 200; ++pass)
    {
        library.poll();
        if (!library.busy())
            return true;
        ::usleep(10 * 1000);
    }
    return false;
}

TEST(FnosLibrary, PrefetchedAddressIsSpentOnceAndSaysNothing)
{
    Library library;
    ASSERT_TRUE(library.start(nullptr));
    library.request_prefetch("preview-redirect");
    ASSERT_TRUE(drained(library));

    EXPECT_EQ(library.stream_url("preview-redirect"), "http://preview.resolved/stream");
    // Spent on the way out: a second play of the same song, a lap of repeat-one
    // or a seek back to its start asks the NAS again rather than trusting an
    // address that may be minutes old.
    EXPECT_NE(library.stream_url("preview-redirect").find("guid=preview-redirect"),
              std::string::npos);

    // Bookkeeping stays out of the banner, in either direction.
    library.poll();
    EXPECT_FALSE(library.failed());
    EXPECT_TRUE(library.message().empty());
}

TEST(FnosLibrary, ATrackNobodyPrefetchedHasItsOwnAddress)
{
    Library library;
    ASSERT_TRUE(library.start(nullptr));
    library.request_prefetch("preview-redirect");
    ASSERT_TRUE(drained(library));

    // Another song's turn must not pick up the cached hop.
    EXPECT_NE(library.stream_url("another-song").find("guid=another-song"), std::string::npos);
}

// A shelf read that finishes under a wall of artwork still has to reach the
// screen. The ring holds eight answers; a cover the page never got is asked
// again on the next frame, while a shelf nobody answers stays empty until the
// player turns the page again.
TEST(FnosLibrary, AShelfAnswerSurvivesABurstOfCovers)
{
    hui::fnos::ArtCache cache;
    Library library;
    ASSERT_TRUE(library.start(&cache));
    library.sign_in("http://preview.local", "admin", "hunter2");
    ASSERT_TRUE(drained(library));

    for (int index = 0; index < 40; ++index)
        library.request_cover("no-such-cover-" + std::to_string(index));
    // Front of the queue, so its answer is the oldest one the burst produces.
    library.show(hui::fnos::Shelf::songs);
    // Nothing is drained while the wall comes down: overflowing the ring is the
    // state being tested.
    for (int pass = 0; pass < 400 && library.busy(); ++pass)
        ::usleep(10 * 1000);
    library.poll();

    EXPECT_FALSE(library.tracks().empty()) << "the shelf answer was dropped by a cover";
}

} // namespace
