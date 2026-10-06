// ps5-homebrew-ui - The list of UI designs, in switcher order.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shipped app is the fnOS music player and nothing else: the kit's own
// settings page used to ride beside it, and L1/R1 had somewhere to go. The
// player now opens on its launch sequence and stays on its own pages, so the
// switcher has a single design. The other designs in src/concepts/ are the
// component demos the kit ships with; they stay out of this list. See
// docs/DESIGNS.md for them.

#include "concepts/concepts.hpp"

namespace hui::app
{

std::span<const ConceptFactory> concept_registry()
{
    static constexpr ConceptFactory kFactories[] = {
        concepts::make_fnmusic,
    };
    return kFactories;
}

} // namespace hui::app
