// ps5-homebrew-ui - Design "fnmusic": the fnOS (飞牛) music player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One screen for a whole NAS library:
//
//   - a rail on the left (歌曲/歌单/专辑/艺人/风格/收藏/搜索/设置) and a content
//     zone on the right that is a card grid for the groups and a list for the
//     songs inside one of them;
//   - a transport row that is never more than a press away: L1 and R1 step the
//     song back and on, L2 and R2 step the rail as the shop steps its filter,
//     the right stick fast-forwards, ▼ on the last row raises the bar over
//     the grid and lowers it again, and □ turns the whole page into Now Playing;
//   - every network call goes through fnos::Service, which answers between
//     frames: a slow server costs an animation, never a frame;
//   - covers are fetched on the same worker, decoded to RGBA on it, and kept in
//     a small texture cache. A tile without one shows a coloured plate and a
//     note, so an empty library still looks finished.
//
// Han glyphs: the faces the kit ships have none, so the font set this screen
// hands its components swaps its three text roles for the CJK atlas, which
// carries a Latin face of its own. The digits of the clock stay on the mono.

#include "concepts/concepts.hpp"

#include "core/tween.hpp"
#include "fnos/service.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/list.hpp"
#include "ui/components/media_controls.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hui::concepts
{
namespace
{

using fnos::Library;
using fnos::Service;
using fnos::Shelf;
using gfx::Color;
using gfx::Rect;

// Where the focus is. The splash is a page of its own, the keyboard a layer
// over the content zone.
enum class Zone : int
{
    boot,
    chips,
    content,
    transport,
    keyboard,
};

// Which text the on-screen keyboard is typing.
enum class Field : int
{
    none = -1,
    server = 0,
    username = 1,
    password = 2,
    query = 3,
    // The setup page's buttons. They ride in the same row list as the fields,
    // so each needs its own tag: telling them apart by row position broke as
    // soon as a row was added, and 登录 stopped answering.
    login = 4,
    sign_out = 5,
    device = 6,
    about = 7, // the credits row: read, never typed into
};

constexpr float kWidth = gfx::kVirtualWidth;
constexpr float kHeight = gfx::kVirtualHeight;
constexpr float kMargin = 96.0f;
constexpr float kRight = kWidth - kMargin;
constexpr float kTransportHeight = 140.0f;

// ---- the three pages, after the kit's Launch Sequence, Storefront and Now Playing
// Palette: the Storefront's coal-and-warm-ink sheet, with the 飞牛 orange as the
// single accent so the app keeps its own identity.
const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kInk = Color::rgb(0xf4efe6);      // warm white: all text
const Color kAccent = Color::rgb(0xff6a13);   // calls to action
const Color kOnAccent = Color::rgb(0x201407); // text on the accent
const Color kCoal = Color::rgb(0x0e0f12);     // backdrop, top
const Color kCoalLow = Color::rgb(0x17181d);  // backdrop, bottom
const Color kPanel = Color::rgb(0x202228);    // plates and panels

// The splash (Launch Sequence, first screen).
constexpr float kCentreX = kWidth * 0.5f;
constexpr float kEmblemY = 452.0f;
constexpr float kEmblemRadius = 92.0f;
constexpr float kSplashSeconds = 2.4f;

// The home page. Every number here is design 13's own, taken from the
// Storefront sheet as drawn in src/concepts/store.cpp: the plate is 412 tall
// with 820 of artwork at its right, the chips sit under it, and the grid runs
// from 628 to 968 in 326x272 capsules. Inventing a proportion here is what
// made this page stop resembling the sheet.
constexpr float kTopBarY = 78.0f;
constexpr float kBannerY = 116.0f;
constexpr float kBannerH = 412.0f;
constexpr float kBannerArtW = 820.0f;
constexpr float kBannerRadius = 28.0f;
constexpr float kChipsY = 552.0f;
constexpr float kChipH = 48.0f;
constexpr float kGridY = 628.0f;
constexpr float kCardGap = 24.0f;   // between capsules, left to right
constexpr float kCardH = 272.0f;    // 184 of cover, the words under it
constexpr float kCardRadius = 14.0f;
constexpr float kRowPitch = 300.0f; // one row to the next
constexpr float kViewBottom = 968.0f;
// 326 wide over the sheet's 184 tall cover.
constexpr float kCardArtAspect = 1.78f;
constexpr float kCardW = (kWidth - 2.0f * kMargin - 4.0f * kCardGap) / 5.0f;
constexpr float kCoverH = 184.0f; // the picture inside a capsule
constexpr float kPageTop = 104.0f;
constexpr float kStickY = 116.0f;      // where the chips stop when the page scrolls
constexpr float kFadeFoot = 72.0f;     // the grid fades out over this distance
constexpr float kFadeHead = 24.0f;     // ... and in again under the chips
constexpr float kStuckTop = kStickY + kChipH + 28.0f; // the focused row's top when scrolled
constexpr float kLift = 0.05f; // how much the focused card grows

// Drawn layer by layer across the cards, not card by card, so the whole grid
// costs a handful of calls instead of one per element (design 13's own trick).
enum : unsigned
{
    kImages = 1,
    kShapes = 2,
    kSemibold = 4,
    kMono = 8,
    kAllLayers = 15,
};

// The playing page (Now Playing).
constexpr float kPlayArt = 460.0f;
constexpr float kPlayArtY = 150.0f;
constexpr float kPlayArtRadius = 40.0f;
constexpr float kPlayPausedScale = 0.94f;
constexpr float kPlayColumnGap = 72.0f;
constexpr int kBars = 24;
constexpr float kBarsBase = 604.0f;
constexpr float kBarsHeight = 150.0f;
constexpr float kQueueRow = 66.0f;
constexpr float kQueueW = 420.0f;
constexpr float kQueueY = 96.0f;
constexpr float kQueueRadius = 28.0f;
const Color kGold = Color::rgb(0xffd166); // the favourite star
constexpr float kTau = 6.2831853f;

// Plate colours for artwork that has not arrived: vivid, never grey-blue.
constexpr std::uint32_t kPlates[] = {0xff6a13, 0xf5a524, 0x12b886, 0xe64980,
                                     0x7048e8, 0x1c7ed6, 0xfa5252, 0x82c91e};
constexpr int kPlateCount = static_cast<int>(sizeof(kPlates) / sizeof(kPlates[0]));

const char *shelf_title(Shelf shelf)
{
    switch (shelf)
    {
    case Shelf::songs:
        return "全部歌曲";
    case Shelf::playlists:
        return "歌单";
    case Shelf::albums:
        return "专辑";
    case Shelf::artists:
        return "艺人";
    case Shelf::genres:
        return "风格";
    case Shelf::favorites:
        return "我的收藏";
    case Shelf::search:
        return "搜索结果";
    case Shelf::setup:
        return "连接设置";
    }
    return "飞牛音乐";
}

struct RailEntry
{
    const char *label;
    Shelf shelf;
};

constexpr std::array<RailEntry, 8> kRail = {
    RailEntry{"歌曲", Shelf::songs},  RailEntry{"歌单", Shelf::playlists},
    RailEntry{"专辑", Shelf::albums}, RailEntry{"艺人", Shelf::artists},
    RailEntry{"风格", Shelf::genres}, RailEntry{"收藏", Shelf::favorites},
    RailEntry{"搜索", Shelf::search}, RailEntry{"设置", Shelf::setup},
};

constexpr int kRailCount = 8;

// The right stick has to be pushed past this before the position moves, and it
// jumps the song in blocks of this many seconds.
constexpr float kScrubEdge = 0.3f;
constexpr float kScrubStep = 5.0f;

std::string clock_text(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    char text[24];
    if (seconds >= 3600)
    {
        std::snprintf(text, sizeof(text), "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60,
                      seconds % 60);
        return text;
    }
    std::snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
    return text;
}

std::uint32_t plate_hash(const std::string &key)
{
    std::uint32_t hash = 2166136261u;
    for (const char c : key)
    {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

Color plate_color(const std::string &key)
{
    return Color::rgb(kPlates[plate_hash(key) % kPlateCount]);
}

// Centred square crop of a picture of that size, as UVs.
Rect square_uv(int width, int height)
{
    if (width <= 0 || height <= 0)
        return gfx::kFullUv;
    if (width > height)
    {
        const float share = static_cast<float>(height) / static_cast<float>(width);
        return {(1.0f - share) * 0.5f, 0.0f, share, 1.0f};
    }
    const float share = static_cast<float>(width) / static_cast<float>(height);
    return {0.0f, (1.0f - share) * 0.5f, 1.0f, share};
}

// The wide crop of the same picture, for the sheet's capsules: a square cover
// loses its top and bottom, never its middle, which is where the art is.
Rect banner_uv(int width, int height)
{
    if (width <= 0 || height <= 0)
        return gfx::kFullUv;
    const float source = static_cast<float>(width) / static_cast<float>(height);
    if (source > kCardArtAspect)
    {
        const float share = kCardArtAspect / source;
        return {(1.0f - share) * 0.5f, 0.0f, share, 1.0f};
    }
    const float share = source / kCardArtAspect;
    return {0.0f, (1.0f - share) * 0.5f, 1.0f, share};
}

// The note on a plate whose artwork has not arrived.
void draw_note(gfx::DrawList &list, const Rect &art, Color ink)
{
    const float s = std::min(art.w, art.h) * 0.36f;
    const float x = art.cx() - s * 0.5f;
    const float y = art.cy() - s * 0.5f;
    list.circle(x + s * 0.28f, y + s * 0.76f, s * 0.17f, ink);
    list.rounded_rect({x + s * 0.41f, y + s * 0.14f, s * 0.10f, s * 0.64f}, s * 0.05f, ink);
    list.rotated_rect({x + s * 0.41f, y + s * 0.12f, s * 0.38f, s * 0.10f}, s * 0.05f, 0.35f, ink);
}

// Baseline that centres a line of the given size on cy.
float centred(float cy, float size)
{
    return cy + size * 0.35f;
}

// A card's place on the unscrolled page, exactly where design 13 puts it.
Rect card_rect(int k)
{
    return {kMargin + static_cast<float>(k % 5) * (kCardW + kCardGap),
            kGridY + static_cast<float>(k / 5) * kRowPitch, kCardW, kCardH};
}

const char *const kTechniques[] = {
    "NAS library on one worker thread, answered between frames",
    "the storefront page over the live library: sticky chips, one gliding ring",
    "cover art decoded on the console by FFmpeg and cached as textures",
    "CJK atlas swapped into the components' font roles",
};

} // namespace

class Fnmusic final : public app::Concept
{
  public:
    explicit Fnmusic(app::Context &context) : fn_(Service::instance()), version_(context.version)
    {
        build_theme();
        glyphs_ = context.fonts;
        if (glyphs_.cjk.font != nullptr)
        {
            glyphs_.regular = glyphs_.cjk;
            glyphs_.semibold = glyphs_.cjk;
            glyphs_.display = glyphs_.cjk;
        }

        // The chips row carries what the rail used to: one entry per shelf. The
        // pill simply sits on the focused chip, no spring needed.
        chip_ = 0;

        tracks_.style.leading_width = 78.0f;
        tracks_.style.row_height = 74.0f;
        tracks_.style.dividers = true;
        tracks_.style.highlight.kind = ui::HighlightKind::bar;
        tracks_.style.focus_shift = 6.0f;
        tracks_.style.title_size = 26.0f;
        tracks_.style.subtitle_size = 19.0f;
        tracks_.style.value_size = 21.0f;
        tracks_.leading = [this](ui::Canvas &canvas, const Rect &row, const ui::ListItem &item,
                                 int index, float focus)
        {
            draw_row_cover(canvas, item, Rect{row.x + 10.0f, row.cy() - 28.0f, 56.0f, 56.0f}, index,
                           focus);
        };

        // The page is drawn by this design itself, layer by layer, exactly as
        // design 13 draws it; only its row list and its fields are components.
        controls_.style.layout = ui::MediaLayout::compact;
        controls_.style.panel = true;
        controls_.style.show_art = false;
        controls_.style.exits.up = true;
        // The bar is a read-out, not a control: the focus walks its buttons and
        // never sits on the scrubber, so the thumb cannot freeze where it was
        // left, and the right stick is what drags the position.
        controls_.style.focus_scrubber = false;
        controls_.style.title_size = 25.0f;
        controls_.style.artist_size = 19.0f;
        controls_.art = [this](ui::Canvas &canvas, const Rect &box, float radius)
        { draw_square(canvas.list, box, radius, transport_gl_, transport_uv_); };

        fields_.style.row_height = 92.0f;
        fields_.style.highlight.kind = ui::HighlightKind::bar;
        fields_.style.dividers = true;
        fields_.style.title_size = 27.0f;
        fields_.style.subtitle_size = 19.0f;
        fields_.style.value_size = 23.0f;

        keys_.style.bindings = ui::KeyboardBindings::standard();
        // Options confirms the field: reaching 完成 with the d-pad alone is
        // what made players think the keyboard ignored them.
        keys_.style.bindings.done = Action::menu;
        keys_.style.max_length = 64;
        keys_.on_text = [this](std::string_view text)
        {
            std::string &target = edit_buffer();
            target.append(text);
            keys_.set_length(static_cast<int>(target.size()));
            rebuild_ = true;
        };
        keys_.on_backspace = [this]
        {
            std::string &target = edit_buffer();
            if (!target.empty())
                target.pop_back();
            keys_.set_length(static_cast<int>(target.size()));
            rebuild_ = true;
        };

        Library &lib = fn_.library();
        server_ = lib.server();
        username_ = lib.username();
        password_ = lib.password();
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "fnmusic",
            "飞牛音乐",
            "飞牛 fnOS 音乐服务：歌曲、歌单、专辑、艺人与风格的局域网播放器",
            "src/concepts/fnmusic.cpp",
            audio::SoundSet::glass,
            Color::rgb(0xff6a13),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        Library &lib = fn_.library();
        tracks_.enter();
        reset_grid();
        fields_.enter();
        keys_.enter();
        enter_.snap(0.0f);
        enter_.target = 1.0f;
        if (lib.signed_in())
            lib.show(Shelf::songs);
        else
            lib.show(Shelf::setup);
        chip_ = rail_index(lib.shelf());
        // The launch sequence comes first: the splash lifts on its own, or on a
        // press, and only then does the shelf answer the controller.
        zone_ = Zone::boot;
        boot_age_ = 0.0f;
        // A title that died still leaves its receipts on disk (`dev/steps.log`),
        // but nothing says so on screen: the machine cannot tell whether the last
        // run ended or crashed, and a card about it read as a broken product.
        rebuild_ = true;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        clock_ += dt;
        // Without this tick the page never arrives: enter_ stays at the 0 it
        // was snapped to in enter(), and every layer gated on it is invisible.
        enter_.update(dt, 12.0f);
        if (!notice_.empty())
        {
            notice_age_ += dt;
            if (notice_age_ > 8.0f)
            {
                notice_.clear();
                rebuild_ = true;
            }
        }
        Library &lib = fn_.library();
        fnos::Player &player = fn_.player();

        // The splash owns the controller until it lifts.
        if (zone_ == Zone::boot)
        {
            boot_age_ += dt;
            if (input.pressed != 0 || boot_age_ >= kSplashSeconds)
            {
                feedback.play(audio::Cue::back);
                zone_ = lib.signed_in() ? Zone::chips : Zone::content;
                rebuild_ = true;
            }
            return;
        }

        layout();
        tracks_.update(dt);
        update_grid(dt);
        fields_.update(dt);
        controls_.update(dt);
        keys_.update(dt);
        // The bars feed the home plate as well as Now Playing, so they keep
        // moving wherever the song is named.
        update_spectrum(dt, player);

        feed_transport(lib, player);
        // A login that lands has to move the player: the setup rows stay where
        // they are otherwise, and a sign-in looks like nothing happened.
        const bool signed_in = lib.signed_in();
        if (signed_in && !was_signed_in_ && zone_ != Zone::keyboard)
        {
            lib.show(Shelf::songs);
            chip_ = rail_index(Shelf::songs);
            zone_ = Zone::chips;
            feedback.play(audio::Cue::welcome);
            rebuild_ = true;
        }
        was_signed_in_ = signed_in;
        if (lib.failed() && !was_failed_ && !signed_in)
            feedback.play(audio::Cue::error); // a refused login must be heard as well as read
        was_failed_ = lib.failed();
        touch_covers(lib);
        if (rebuild_ || signature(lib) != signature_)
            rebuild(lib);
        // A shelf that holds nothing cannot hold the focus either: it stays on
        // the row of chips, where ○ and the shoulders still do something. Only
        // once the request has settled — a shelf being fetched is empty too, and
        // bouncing off that reads as a page that will not open.
        if (zone_ == Zone::content && !lib.busy() && shelf_empty(lib))
        {
            zone_ = Zone::chips;
            rebuild_ = true;
        }

        if (zone_ != Zone::keyboard)
        {
            if (input.is_pressed(Action::west))
            {
                now_ = !now_;
                if (now_)
                    zone_ = Zone::transport;
                feedback.play(audio::Cue::tab);
                rebuild_ = true;
                return;
            }
            if (input.is_pressed(Action::north))
            {
                lyric_open_ = !lyric_open_;
                feedback.play(lyric_open_ ? audio::Cue::open : audio::Cue::back);
                return;
            }
            // The left of each pair steps back, the right steps on: L1/R1 the
            // previous and next song, L2/R2 the shelf to the left and the right.
            // The position itself is on the right stick, so no button
            // fast-forwards and none needs the player bar's focus.
            if (input.is_pressed(Action::page_prev))
            {
                step_song(-1, feedback);
                return;
            }
            if (input.is_pressed(Action::page_next))
            {
                step_song(1, feedback);
                return;
            }
            if (input.is_pressed(Action::jump_prev))
            {
                step_rail(-1, lib, feedback);
                return;
            }
            if (input.is_pressed(Action::jump_next))
            {
                step_rail(1, lib, feedback);
                return;
            }
            // Options on the chips row means "read this shelf again"; anywhere
            // else it is the favourite switch of the row or track in hand.
            if (input.is_pressed(Action::menu) && zone_ != Zone::chips &&
                lib.shelf() != Shelf::setup && (track_shelf(lib.shelf()) || !show_cards(lib)))
            {
                toggle_favorite(lib, focused_track(lib), feedback);
                return;
            }
        }

        // Read after the buttons above, so a shoulder press on the same frame
        // does not also drag the position. The left stick's sideways axis is the
        // volume, so a step that came from the stick sideways must not move the
        // focus as well; the D-pad and the stick's up and down still navigate.
        InputFrame walked = input;
        if (input.nav_from_stick &&
            (input.nav == Direction::left || input.nav == Direction::right))
            walked.nav = Direction::none;
        if (zone_ != Zone::keyboard)
        {
            on_scrub_stick(input, dt, feedback);
            on_volume_stick(input, dt, feedback);
        }
        if (volume_hud_ > 0.0f)
            volume_hud_ -= dt;

        switch (zone_)
        {
        case Zone::boot:
            break;
        case Zone::chips:
            on_chips(walked, lib, feedback);
            break;
        case Zone::content:
            on_content(walked, lib, feedback);
            break;
        case Zone::transport:
            on_transport(walked, feedback);
            break;
        case Zone::keyboard:
            on_keyboard(walked, lib, feedback);
            break;
        }
    }

    void draw(app::Frame &frame) const override
    {
        // A coal backdrop, as on the Storefront and Now Playing sheets.
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = kCoal;
        frame.backdrop.colors[1] = kCoalLow;
        frame.backdrop.colors[2] = kAccent.with_alpha(0.16f);
        frame.backdrop.colors[3] = kClear;
        frame.backdrop.params[0] = 0.2f;
        frame.backdrop.params[1] = 0.04f;
        frame.backdrop.params[2] = 0.55f;
        frame.backdrop.time = clock_;
        frame.glass = now_ || lyric_open_;

        ui::Canvas canvas{frame.scene, glyphs_, frame.glass_texture, clock_};
        Library &lib = fn_.library();
        if (zone_ == Zone::boot)
        {
            draw_boot_splash(canvas);
            return;
        }
        if (now_)
        {
            draw_playing(canvas, lib);
            controls_.draw(canvas);
            if (lyric_open_)
                draw_lyric(canvas, lib);
            draw_hints(canvas, lib);
            return;
        }
        draw_top_bar(canvas, lib);
        draw_banner(canvas, lib);
        if (lib.shelf() == Shelf::setup)
            draw_setup_side(canvas);
        draw_chips(canvas);
        if (lib.shelf() == Shelf::setup)
            fields_.draw(canvas);
        else if (show_cards(lib))
            draw_grid(canvas.list);
        else
            tracks_.draw(canvas);
        // The shop's page has no player bar on it: ▼ at the bottom of the grid
        // raises one over the tiles and ✕ puts it back, so the capsules keep the
        // whole lower half.
        if (zone_ == Zone::transport)
        {
            const Rect scrim{0.0f, transport_.y - 40.0f, kWidth, kHeight - transport_.y + 40.0f};
            canvas.list.rounded_rect(scrim, 0.0f, kCoal.with_alpha(0.93f));
            controls_.draw(canvas);
        }
        if (lyric_open_)
            draw_lyric(canvas, lib);
        if (zone_ == Zone::keyboard)
            draw_keyboard(canvas);
        draw_hints(canvas, lib);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    static int rail_index(Shelf shelf)
    {
        for (int i = 0; i < kRailCount; ++i)
        {
            if (kRail[static_cast<std::size_t>(i)].shelf == shelf)
                return i;
        }
        return 0;
    }

    void build_theme()
    {
        ui::Theme &theme = theme_;
        theme = ui::default_theme();
        theme.id = "fnmusic";
        theme.name = "飞牛音乐";
        theme.family = "飞牛 fnOS";
        theme.summary = "煤黑底、暖白字、鲜橙主色的局域网音乐播放器";
        theme.page = kCoal;
        theme.surface = kPanel;
        theme.surface_high = Color::rgb(0x2b2e36);
        theme.text = kInk;
        theme.text_muted = kInk.with_alpha(0.62f);
        theme.primary = kAccent;
        theme.on_primary = kOnAccent;
        theme.secondary = Color::rgb(0x2b2e36);
        theme.on_secondary = kInk;
        theme.accent = Color::rgb(0x12b886);
        theme.outline = kInk.with_alpha(0.14f);
        theme.focus = kInk.with_alpha(0.85f);
        theme.shadow = Color::rgb(0x000000, 0.45f);
        theme.light = kInk.with_alpha(0.5f);
        theme.style = ui::SurfaceStyle::soft;
        theme.corner = ui::Corner::round;
        theme.radius = 14.0f;
        theme.radius_card = 28.0f;
        theme.shadow_offset = 8.0f;
        theme.shadow_blur = 26.0f;
        theme.dark = true;
        theme.sounds = audio::SoundSet::glass;
        theme.omega = 15.0f;

        tracks_.style.theme = theme;
        fields_.style.theme = theme;
        controls_.style.theme = theme;
        keys_.style.theme = theme;
    }

    void layout()
    {
        // One full-width sheet under the chips row, as on the Storefront page:
        // the featured plate is the page, on every shelf including setup. Six
        // rows do not fit under it, so the setup list scrolls to the focus.
        const bool setup = fn_.library().shelf() == Shelf::setup;
        // The sheet's window: 628 down to 968, one row of capsules whole and
        // the top of the next. The transport bar is not part of this page — it
        // rises over the bottom when ▼ reaches the last row — so the grid has
        // the room the shop's own grid has.
        content_ = {kMargin, kGridY, kWidth - 2.0f * kMargin, kViewBottom - kGridY};
        // The widget lays its title, artist, scrubber and buttons out from the
        // height it is handed, and the compact bar's 140 is not enough for the
        // full one: those bands landed on top of each other on Now Playing.
        controls_.style.layout = now_ ? ui::MediaLayout::full : ui::MediaLayout::compact;
        // On Now Playing the card is part of the page: nothing lifts or drops it,
        // so ▲ at the top must not throw the page away. Over the storefront it is
        // a bar that ▼ brings up and puts back down.
        controls_.style.exits.up = !now_;
        // The card on Now Playing is part of the page, so it carries no panel of
        // its own: the sheet behind it is the sheet. The bar over the storefront
        // still needs one, because it lies on top of other people's artwork.
        controls_.style.panel = !now_;
        const float transport_h =
            now_ ? std::max(kTransportHeight, controls_.preferred_height()) : kTransportHeight;
        transport_ = {kMargin, kHeight - transport_h - 64.0f, kWidth - 2.0f * kMargin,
                      transport_h};
        header_left_ = kMargin;

        tracks_.set_bounds(content_);
        fields_.set_bounds(setup ? Rect{content_.x, content_.y, content_.w - 520.0f, content_.h}
                                 : content_);
        controls_.set_bounds(transport_);

        const float keys_h = 470.0f;
        const float keys_w = kWidth - 2.0f * (kMargin + 60.0f);
        keys_.set_bounds({kMargin + 60.0f, kHeight - keys_h - 36.0f, keys_w, keys_h});
    }

    // How many a shelf holds, and only when this run already knows: a chip that
    // guesses at a number is worse than one that shows none.
    std::string chip_count(Shelf shelf, const Library &lib) const
    {
        std::size_t held = 0;
        switch (shelf)
        {
        case Shelf::playlists:
            held = lib.playlists().size();
            break;
        case Shelf::albums:
            held = lib.albums().size();
            break;
        case Shelf::artists:
            held = lib.artists().size();
            break;
        case Shelf::genres:
            held = lib.genres().size();
            break;
        case Shelf::setup:
            break;
        default:
            // Songs, favourites and search results: the count is the page this
            // shelf is showing right now, and no other.
            if (shelf == lib.shelf() && lib.collection().empty())
                held = lib.tracks().size();
            break;
        }
        return held > 0 ? std::to_string(held) : std::string();
    }

    // Which shelves are a list of songs rather than a list of groups: a song
    // has cover art of its own, so those pages wear the Storefront grid too.
    static bool track_shelf(Shelf shelf)
    {
        return shelf == Shelf::songs || shelf == Shelf::favorites || shelf == Shelf::search;
    }

    // Tiles of a group, or the songs inside one?
    bool show_cards(const Library &lib) const
    {
        // Inside one album or playlist the page stays a row list: a row there
        // owes you a running time and the "播放中" mark, which a cover tile has
        // no room to say.
        if (!lib.collection().empty())
            return false;
        return lib.shelf() != Shelf::setup;
    }

    // Whether the page under the chips has anything at all to walk over.
    bool shelf_empty(const Library &lib) const
    {
        if (lib.shelf() == Shelf::setup)
            return fields_.items().empty();
        return show_cards(lib) ? grid_.empty() : tracks_.items().empty();
    }

    std::size_t signature(const Library &lib) const
    {
        std::size_t hash = static_cast<std::size_t>(lib.shelf());
        hash = hash * 131u + lib.collection().size();
        hash = hash * 131u + lib.tracks().size();
        hash = hash * 131u + lib.playlists().size();
        hash = hash * 131u + lib.albums().size();
        hash = hash * 131u + lib.artists().size();
        hash = hash * 131u + lib.genres().size();
        hash = hash * 131u + fn_.art().held();
        hash = hash * 131u + static_cast<std::size_t>(now_ ? 1 : 0);
        hash = hash * 131u + static_cast<std::size_t>(lib.signed_in() ? 1 : 0);
        hash = hash * 131u + static_cast<std::size_t>(fn_.index() + 1);
        // The plate names the song and says whether it is running, so a state
        // change has to redraw it. Without this the plate keeps saying 正在播放
        // after the song stops.
        hash = hash * 131u + static_cast<std::size_t>(fn_.player().state());
        return hash;
    }

    // Cover ids and textures for whatever is on screen, stamped as used so the
    // cache keeps them. A texture arriving or going asks for a rebuild, since
    // the items carry their textures.
    void touch_covers(Library &lib)
    {
        cover_ids_.clear();
        if (lib.shelf() == Shelf::setup)
        {
            cover_gl_.clear();
            return;
        }
        if (show_cards(lib) && !track_shelf(lib.shelf()))
        {
            switch (lib.shelf())
            {
            case Shelf::playlists:
                for (const fnos::Playlist &item : lib.playlists())
                    cover_ids_.push_back(item.cover_id);
                break;
            case Shelf::albums:
                for (const fnos::Album &item : lib.albums())
                    cover_ids_.push_back(item.cover_id);
                break;
            case Shelf::artists:
                for (const fnos::Artist &item : lib.artists())
                    cover_ids_.push_back(item.cover_id);
                break;
            default:
                // A style has no artwork from the NAS: every tile is a plate.
                cover_ids_.assign(lib.genres().size(), std::string());
                break;
            }
        }
        else
        {
            for (const fnos::Track &track : lib.tracks())
                cover_ids_.push_back(track.cover_id);
        }

        cover_gl_.assign(cover_ids_.size(), 0u);
        // Only what the window can show. The cache holds forty textures, so
        // asking for a whole shelf of songs means the upload drops a cover the
        // page is still drawing, the next frame wants it back, and the fetch
        // and the GL upload never stop: that is the flicker, and the load that
        // took the console down mid-playback.
        for (int i = 0; i < static_cast<int>(cover_ids_.size()); ++i)
        {
            const std::string &key = cover_ids_[static_cast<std::size_t>(i)];
            if (key.empty() || !cover_visible(lib, i))
                continue;
            cover_gl_[static_cast<std::size_t>(i)] = fn_.art().texture(key);
            lib.request_cover(key);
        }
        // The banner keeps showing the playing track wherever the focus is.
        if (const fnos::Track *playing = fn_.current();
            playing != nullptr && !playing->cover_id.empty())
        {
            fn_.art().texture(playing->cover_id);
            lib.request_cover(playing->cover_id);
        }
        if (cover_gl_ != painted_)
        {
            // A cover landing changes the cards that hold it, not the page. A
            // full rebuild fits every title on the shelf again, so on a library
            // of several hundred songs scrolling one row at a time would pay a
            // whole page's work per frame.
            if (painted_.size() == cover_gl_.size() &&
                (!show_cards(lib) || grid_.size() == cover_ids_.size()))
                paint_covers();
            else
            {
                painted_ = cover_gl_;
                rebuild_ = true;
            }
        }
    }

    // Paint the covers that arrived — or were dropped from the forty-slot cache,
    // which hands back 0 and must clear the card too, since a deleted GL id can
    // never be drawn again — without touching the cards that did not change.
    void paint_covers()
    {
        for (std::size_t i = 0; i < cover_gl_.size(); ++i)
        {
            const std::uint32_t gl = cover_gl_[i];
            if (gl == painted_[i])
                continue;
            painted_[i] = gl;
            const std::string &key = cover_ids_[i];
            if (i < grid_.size())
            {
                ui::CardItem &card = grid_[i];
                card.texture = gl;
                card.uv = gfx::kFullUv;
                int width = 0;
                int height = 0;
                if (gl != 0 && fn_.art().size(key, &width, &height))
                    card.uv = banner_uv(width, height);
            }
            if (!banner_key_.empty() && key == banner_key_ && gl != banner_gl_)
            {
                banner_gl_ = gl;
                banner_uv_ = gfx::kFullUv;
                int width = 0;
                int height = 0;
                if (gl != 0 && fn_.art().size(key, &width, &height))
                    banner_uv_ = square_uv(width, height);
            }
        }
    }

    // True when item k is inside the visible part of the grid or the row list.
    bool cover_visible(const Library &lib, int k) const
    {
        if (!show_cards(lib))
        {
            // The shelf can gain a page before the list is rebuilt, and
            // row_rect has no answer for a row that is not there yet.
            if (k >= static_cast<int>(tracks_.items().size()))
                return false;
            const Rect row = tracks_.row_rect(k);
            const Rect box = tracks_.bounds();
            return row.y + row.h > box.y && row.y < box.y + box.h;
        }
        const float top = card_rect(k).y - grid_scroll_.value;
        return top + kCardH > window_top() && top < kViewBottom;
    }

    void rebuild(const Library &lib)
    {
        rebuild_ = false;
        signature_ = signature(lib);
        rebuild_banner(lib);
        // A new shelf or a new collection is a new page: start at its top.
        if (lib.shelf() != last_shelf_ || lib.collection() != last_collection_)
        {
            last_shelf_ = lib.shelf();
            last_collection_ = lib.collection();
            tracks_.set_focus(0, true);
            reset_grid();
            fields_.set_focus(0, true);
        }

        if (lib.shelf() == Shelf::setup)
        {
            std::vector<ui::ListItem> items;
            items.push_back(field_row("服务器", "例如 http://192.168.1.20:5666",
                                      server_.empty() ? "未设置" : server_, Field::server, true));
            items.push_back(field_row("账号", "飞牛音乐的用户名",
                                      username_.empty() ? "未设置" : username_, Field::username,
                                      true));
            items.push_back(field_row("密码", "填一次就存在这台机器上，退出登录时清除",
                                      password_.empty() ? "未填写" : "已保存", Field::password,
                                      true));
            // Nothing here is disabled: a row that cannot be honoured yet says
            // what it is waiting for, and still answers the press.
            const std::string waiting = missing_field();
            ui::ListItem login =
                field_row("登录", waiting.empty() ? "连上这台 NAS，读取它的音乐库"
                                                  : "还差 " + waiting,
                          std::string(), Field::login, false);
            items.push_back(login);
            ui::ListItem out =
                field_row("退出登录", lib.signed_in() ? "清除这台设备上的令牌" : "当前未登录",
                          std::string(), Field::sign_out, false);
            out.disabled = !lib.signed_in();
            items.push_back(out);
            items.push_back(field_row("本机编号", "在 NAS 的设备列表里看到它", lib.device(),
                                      Field::device, false));
            // The screen this app is built on gets named where the account is
            // named: the kit is GPL-3.0-or-later, so the notice belongs in the
            // running program and not only in its repository. The number is the
            // one param.json carries, which is also what the build is named for.
            items.push_back(field_row("关于", "github.com/blackbearreloaded · GPL-3.0-or-later",
                                      version_.empty() ? std::string("版本未知") : version_,
                                      Field::about, false));
            fields_.set_items(std::move(items));
            return;
        }

        if (show_cards(lib))
        {
            std::vector<ui::CardItem> cards;
            cards.reserve(cover_ids_.size());
            std::size_t index = 0;
            auto fill = [&](const std::string &name, const std::string &second,
                            const std::string &badge = std::string())
            {
                ui::CardItem card;
                card.title = name;
                card.subtitle = second;
                card.badge = badge;
                const std::string &key = index < cover_ids_.size() ? cover_ids_[index] : empty_key_;
                ++index;
                const Color plate = plate_color(name + key);
                card.top = gfx::mix(plate, Color::rgb(0xffffff), 0.22f).with_alpha(0.95f);
                card.bottom = plate.with_alpha(0.95f);
                card.accent = plate;
                card.texture = index - 1 < cover_gl_.size() ? cover_gl_[index - 1] : 0u;
                int width = 0;
                int height = 0;
                if (card.texture != 0 && fn_.art().size(key, &width, &height))
                    card.uv = banner_uv(width, height);
                cards.push_back(std::move(card));
            };
            if (track_shelf(lib.shelf()))
            {
                // The Storefront grid answers for the whole library, the
                // favourites and a search at once: all three are songs.
                const fnos::Track *playing = fn_.current();
                for (const fnos::Track &item : lib.tracks())
                {
                    std::string second = item.artist;
                    if (item.duration > 0)
                        second += (second.empty() ? "" : " · ") + clock_text(item.duration);
                    const std::string badge = playing != nullptr && playing->guid == item.guid
                                                  ? "播放中"
                                                  : (item.favorite ? "已收藏" : std::string());
                    fill(item.title, second, badge);
                }
            }
            else
            {
                switch (lib.shelf())
                {
                case Shelf::playlists:
                    for (const fnos::Playlist &item : lib.playlists())
                        fill(item.name, count_text(item.track_count));
                    break;
                case Shelf::albums:
                    for (const fnos::Album &item : lib.albums())
                    {
                        std::string second = item.artist;
                        if (item.year > 0)
                            second += (second.empty() ? "" : " · ") + std::to_string(item.year);
                        fill(item.name, second);
                    }
                    break;
                case Shelf::artists:
                    for (const fnos::Artist &item : lib.artists())
                        fill(item.name, std::string());
                    break;
                default:
                    // A style has no artwork from the NAS: every tile is a plate.
                    for (const fnos::Genre &item : lib.genres())
                        fill(item.name, count_text(item.track_count));
                    break;
                }
            }
            set_grid(std::move(cards));
            return;
        }

        std::vector<ui::ListItem> rows;
        rows.reserve(lib.tracks().size());
        const fnos::Track *playing = fn_.current();
        int number = 0;
        for (const fnos::Track &track : lib.tracks())
        {
            ++number;
            ui::ListItem row;
            row.title = track.title;
            std::string second = track.artist;
            if (!track.album.empty())
                second += (second.empty() ? "" : " — ") + track.album;
            row.subtitle = second;
            row.value = track.duration > 0 ? clock_text(track.duration) : std::string("--:--");
            if (playing != nullptr && playing->guid == track.guid)
                row.badge = "播放中";
            else if (track.favorite)
                row.badge = "已收藏";
            row.tag = number;
            row.swatch = plate_color(track.guid + track.title);
            rows.push_back(std::move(row));
        }
        tracks_.set_items(std::move(rows));
    }

    // The banner speaks for the shelf on screen: its name, how much it holds
    // and the artwork of its first entry.
    void rebuild_banner(const Library &lib)
    {
        banner_gl_ = 0;
        banner_uv_ = gfx::kFullUv;
        banner_key_.clear();
        banner_unit_.clear();
        banner_song_ = false;
        banner_kicker_ = shelf_title(lib.shelf());
        if (!lib.signed_in() || lib.shelf() == Shelf::setup)
        {
            banner_kicker_ = "局域网音乐库";
            banner_title_ = "飞牛音乐";
            banner_note_ = "连上 NAS，把它的音乐放到客厅里听";
            banner_stat_.clear();
            banner_tone_ = kAccent;
            return;
        }

        const fnos::Track *song = fn_.current();
        if (song != nullptr)
        {
            // The plate is the largest thing on the shop page, and it used to
            // speak for whichever entry the shelf happened to list first. The
            // song in the room is the one worth that much space. It stays on the
            // plate once chosen, even after it stops: a shelf's first entry is
            // often that same song, so a plate that only appears while audio
            // runs looks like a plate that never changed at all.
            const fnos::PlayerState state = fn_.player().state();
            const char *verb = state == fnos::PlayerState::playing ||
                                       state == fnos::PlayerState::opening
                                   ? "正在播放 · "
                                   : (state == fnos::PlayerState::paused ? "暂停中 · "
                                                                         : "已停下 · ");
            banner_song_ = true;
            banner_kicker_ = std::string(verb) + shelf_title(lib.shelf());
            banner_title_ = song->title;
            banner_note_ = song->artist;
            if (!song->album.empty())
                banner_note_ += (banner_note_.empty() ? "" : " — ") + song->album;
            // The container's own length once it is open, the NAS's until then:
            // the two are not always the same number, and the one you can hear is
            // the one worth printing.
            const int length = fn_.player().duration() > 0 ? fn_.player().duration()
                                                           : song->duration;
            banner_stat_ = length > 0 ? clock_text(length) : std::string("--:--");
            banner_tone_ = gfx::mix(plate_color(song->guid + song->title), kCoal, 0.55f);
            banner_gl_ = song->cover_id.empty() ? 0u : fn_.art().texture(song->cover_id);
            banner_key_ = song->cover_id;
            int width = 0;
            int height = 0;
            if (banner_gl_ != 0 && fn_.art().size(song->cover_id, &width, &height))
                banner_uv_ = square_uv(width, height);
            return;
        }

        std::string name;
        std::string second;
        int count = 0;
        if (!lib.collection().empty())
        {
            name = lib.collection();
            count = static_cast<int>(lib.tracks().size());
            banner_kicker_ = std::string("正在浏览 · ") + shelf_title(lib.shelf());
        }
        else
        {
            switch (lib.shelf())
            {
            case Shelf::playlists:
                if (!lib.playlists().empty())
                {
                    const fnos::Playlist &first = lib.playlists()[0];
                    name = first.name;
                    count = first.track_count;
                }
                break;
            case Shelf::albums:
                if (!lib.albums().empty())
                {
                    const fnos::Album &first = lib.albums()[0];
                    name = first.name;
                    second = first.artist;
                    if (first.year > 0)
                        second += (second.empty() ? "" : " · ") + std::to_string(first.year);
                    count = first.track_count;
                }
                break;
            case Shelf::artists:
                if (!lib.artists().empty())
                    name = lib.artists()[0].name;
                break;
            case Shelf::genres:
                if (!lib.genres().empty())
                {
                    const fnos::Genre &first = lib.genres()[0];
                    name = first.name;
                    count = first.track_count;
                }
                break;
            default:
                if (!lib.tracks().empty())
                {
                    const fnos::Track &first = lib.tracks()[0];
                    name = first.title;
                    second = first.artist;
                    count = static_cast<int>(lib.tracks().size());
                }
                break;
            }
        }

        if (name.empty())
        {
            banner_title_ = shelf_title(lib.shelf());
            banner_note_ = "这台 NAS 上还没有内容";
            banner_stat_.clear();
            banner_tone_ = gfx::mix(plate_color(banner_title_), kCoal, 0.62f);
            return;
        }
        banner_title_ = name;
        // The count stands on its own big line, the way the sheet prices its
        // feature, which leaves the sentence under the title to say one thing.
        // The figure goes in the mono face and the word after it does not: that
        // font has no Chinese in it, and a 首 drawn there comes out a ?.
        banner_stat_ = count > 0 ? std::to_string(count) : std::string();
        banner_unit_ = count > 0 ? " 首" : std::string();
        banner_note_ = second.empty() ? std::string("这一栏已经准备好") : second;
        banner_tone_ = gfx::mix(plate_color(name), kCoal, 0.62f);
        if (!cover_gl_.empty())
        {
            banner_gl_ = cover_gl_[0];
            const std::string &key = cover_ids_.empty() ? empty_key_ : cover_ids_[0];
            banner_key_ = key;
            int width = 0;
            int height = 0;
            if (banner_gl_ != 0 && fn_.art().size(key, &width, &height))
                banner_uv_ = square_uv(width, height);
        }
    }

    static std::string count_text(int count)
    {
        return count > 0 ? std::to_string(count) + " 首" : std::string();
    }

    static ui::ListItem field_row(std::string title, std::string subtitle, std::string value,
                                  Field field, bool editable)
    {
        ui::ListItem row;
        row.title = std::move(title);
        row.subtitle = std::move(subtitle);
        row.value = std::move(value);
        row.chevron = editable;
        row.tag = static_cast<int>(field) + 1; // 0 means "not a field"
        return row;
    }

    // ---- drawing ----

    void draw_square(gfx::DrawList &list, const Rect &box, float radius, std::uint32_t texture,
                     const Rect &uv) const
    {
        if (texture != 0)
        {
            list.image(texture, box, uv, Color{1.0f, 1.0f, 1.0f, 1.0f}, radius);
            return;
        }
        const Color plate = Color::rgb(0xff6a13);
        list.gradient_rect(box, radius, gfx::mix(plate, Color::rgb(0xffffff), 0.24f), plate);
        draw_note(list, box, Color{1.0f, 1.0f, 1.0f, 0.9f});
    }

    void draw_row_cover(ui::Canvas &canvas, const ui::ListItem &item, const Rect &box, int index,
                        float focus) const
    {
        gfx::DrawList &list = canvas.list;
        const float radius = theme_.radius * 0.7f;
        std::uint32_t texture = 0;
        Rect uv = gfx::kFullUv;
        if (index >= 0 && static_cast<std::size_t>(index) < cover_gl_.size())
            texture = cover_gl_[static_cast<std::size_t>(index)];
        if (texture != 0)
        {
            const std::string &key = cover_ids_[static_cast<std::size_t>(index)];
            int width = 0;
            int height = 0;
            if (fn_.art().size(key, &width, &height))
                uv = square_uv(width, height);
            list.push_opacity(0.6f + 0.4f * focus);
            list.image(texture, box, uv, Color{1.0f, 1.0f, 1.0f, 1.0f}, radius);
            list.pop_opacity();
            return;
        }
        const Color plate = item.swatch.a > 0.0f ? item.swatch : Color::rgb(0xffb35c);
        list.gradient_rect(box, radius, gfx::mix(plate, Color::rgb(0xffffff), 0.25f), plate);
        char text[8];
        std::snprintf(text, sizeof(text), "%d", index + 1);
        ui::text(list, glyphs_.mono, text, box.cx(), box.cy() + 7.0f, 21.0f,
                 Color{1.0f, 1.0f, 1.0f, 0.95f}, gfx::Align::center);
    }

    // ---- the three pages ----
    // The splash is the kit's Launch Sequence, first screen; the home page is
    // its Storefront (banner, chips, grid); the playing sheet is Now Playing
    // (large artwork, spectrum, queue).

    // An emblem that draws itself, a wordmark, and a skip that says what it does.
    void draw_boot_splash(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        const float t = std::max(0.0f, boot_age_);
        const float cx = kCentreX;
        const float cy = kEmblemY;
        const float ring = tween::cubic_in_out(t / 0.9f);

        list.glow({cx - 70.0f, cy - 70.0f, 140.0f, 140.0f}, 70.0f, 250.0f,
                  kAccent.with_alpha(0.10f + 0.04f * ui::breathe(clock_, 3.2f)));
        list.arc(cx, cy, kEmblemRadius, 6, 0.0f, kTau * ring, kInk);
        // A hairline drawn the other way round closes at the same moment.
        list.arc(cx, cy, kEmblemRadius + 18.0f, 2, kTau * (1.0f - ring), kTau * ring,
                 kInk.with_alpha(0.3f));

        // The monogram: a note whose stem, flag and head arrive one after another.
        const float stem = tween::cubic_out((t - 0.30f) / 0.4f);
        if (stem > 0.0f)
            list.rounded_rect({cx - 6.0f, cy + 40.0f - 84.0f * stem, 12.0f, 84.0f * stem}, 6.0f,
                              kInk.with_alpha(stem));
        const float flag = tween::cubic_out((t - 0.55f) / 0.4f);
        if (flag > 0.0f)
            list.rotated_rect({cx + 2.0f, cy - 46.0f, 58.0f * flag, 12.0f}, 6.0f, 0.35f,
                              kInk.with_alpha(flag));
        const float head = tween::cubic_out((t - 0.75f) / 0.4f);
        if (head > 0.0f)
        {
            list.glow({cx - 38.0f, cy + 22.0f, 36.0f, 36.0f}, 18.0f, 40.0f,
                      kAccent.with_alpha(0.5f * head));
            list.circle(cx - 20.0f, cy + 40.0f, 17.0f * head, kAccent.with_alpha(head));
        }

        const float word = tween::cubic_out((t - 0.85f) / 0.6f);
        ui::text(list, glyphs_.display, "飞牛音乐", cx, cy + 232.0f - 16.0f * (1.0f - word), 88.0f,
                 kInk.with_alpha(word), gfx::Align::center);
        const float rule = 170.0f * tween::cubic_out((t - 1.15f) / 0.5f);
        list.gradient_rect_h({cx - rule, cy + 266.0f, rule, 2.0f}, 1.0f, kClear,
                             kInk.with_alpha(0.45f));
        list.gradient_rect_h({cx, cy + 266.0f, rule, 2.0f}, 1.0f, kInk.with_alpha(0.45f), kClear);
        ui::text(list, glyphs_.semibold, "飞牛 fnOS 局域网音乐库", cx, cy + 306.0f, 22.0f,
                 kInk.with_alpha(0.62f * tween::cubic_out((t - 1.25f) / 0.5f)), gfx::Align::center,
                 5.0f);

        list.push_opacity(0.75f * tween::cubic_out((t - 0.5f) / 0.5f));
        const ui::Hint hint = {ui::Button::cross, "跳过"};
        ui::HintLayout layout;
        layout.cy = kHeight - 22.0f;
        layout.size = 32.0f;
        layout.text_size = 20.0f;
        ui::draw_hints(list, glyphs_, ui::GlyphStyle::dark(), &hint, 1, kRight, true, layout);
        list.pop_opacity();
    }

    // Where the sheet keeps its brand, this app keeps what is playing: one song
    // is the only thing worth that much attention. With nothing loaded the bar
    // falls back to the wordmark, so a page never opens without an identity.
    // ---- the home page, drawn the way design 13 draws it -------------------

    // The chips scroll with the page until they reach the top bar, then stay.
    float chips_y() const
    {
        return std::max(kChipsY - grid_scroll_.value, kStickY);
    }

    // Chip rectangles on screen, left to right: an L2 glyph opens the row, one
    // pill per shelf carries its label and its count, an R2 glyph closes it.
    void chip_layout(Rect *out) const
    {
        const Library &lib = fn_.library();
        float x = kMargin + ui::button_width(ui::Button::l2, 30.0f) + 16.0f;
        for (int i = 0; i < kRailCount; ++i)
        {
            const std::string label = kRail[static_cast<std::size_t>(i)].label;
            const std::string count = chip_count(kRail[static_cast<std::size_t>(i)].shelf, lib);
            float w = 48.0f + glyphs_.semibold.measure(label, 22.0f);
            if (!count.empty())
                w += 10.0f + glyphs_.mono.measure(count, 18.0f);
            out[i] = {x, chips_y(), w, kChipH};
            x += w + 12.0f;
        }
    }

    // The grid's window: from just under the chips to above the hint row.
    float window_top() const
    {
        return chips_y() + kChipH + 4.0f;
    }
    // How visible something at screen height y is inside that window.
    float fade_at(float y) const
    {
        return std::min(tween::clamp01((y - window_top()) / kFadeHead),
                        tween::clamp01((kViewBottom - y) / kFadeFoot));
    }
    float fade_text(float baseline, float size) const
    {
        return std::min(fade_at(baseline - size * 0.8f), fade_at(baseline + 4.0f));
    }
    // A picture spanning y0..y1, as the opacity at its two ends: an image's
    // tint can only run linearly, so the fade is laid through "nothing at the
    // window's edge" and the part below zero is clipped away anyway.
    void fade_span(float y0, float y1, float *a0, float *a1) const
    {
        const float top = window_top();
        *a0 = *a1 = 1.0f;
        if (y0 < top + kFadeHead)
        {
            const float full = std::max(y1, top + kFadeHead);
            *a0 = (y0 - top) / (full - top);
            *a1 = (y1 - top) / (full - top);
        }
        else if (y1 > kViewBottom - kFadeFoot)
        {
            const float full = std::min(y0, kViewBottom - kFadeFoot);
            *a0 = (kViewBottom - y0) / (kViewBottom - full);
            *a1 = (kViewBottom - y1) / (kViewBottom - full);
        }
    }

    // What the one ring of the page surrounds, in page coordinates: the chip
    // the rail rests on, or the focused capsule.
    Rect focus_target() const
    {
        if (zone_ == Zone::chips || grid_.empty())
        {
            Rect chips[kRailCount];
            chip_layout(chips);
            Rect chip = chips[static_cast<std::size_t>(chip_)].inset(-6.0f);
            chip.y += grid_scroll_.value;
            return chip;
        }
        return card_rect(grid_focus_).inset(-14.0f);
    }
    float focus_radius() const
    {
        if (zone_ == Zone::chips || grid_.empty())
            return kChipH * 0.5f + 6.0f;
        return 24.0f;
    }
    // Where the sheet's scroll should rest: any row but the first sends the
    // banner away and parks the chips under the top bar.
    float scroll_target() const
    {
        if (zone_ != Zone::content || grid_.empty() || grid_focus_ < 5)
            return 0.0f;
        const float top = card_rect(grid_focus_).y;
        const float lowest = std::max(kChipsY - kStickY, top + kCardH + kFadeFoot - kViewBottom);
        const float highest = top - kStuckTop;
        return std::clamp(grid_scroll_.target, lowest, std::max(lowest, highest));
    }

    // A short pill on the art's corner, where the shop hangs its "-30%": here
    // it says 播放中 or 已收藏.
    float badge_pill(gfx::DrawList &list, const std::string &text, float x, float cy, float height,
                     float alpha, Color accent) const
    {
        const float size = height * 0.6f;
        const float w = glyphs_.semibold.measure(text, size) + height * 0.8f;
        const Color base = accent.a > 0.0f ? accent : kAccent;
        list.rounded_rect({x, cy - height * 0.5f, w, height}, height * 0.32f,
                          base.with_alpha(alpha));
        ui::text(list, glyphs_.semibold, text, x + w * 0.5f, centred(cy, size), size, kOnAccent,
                 gfx::Align::center);
        return w;
    }

    // One capsule: cover, marks, title, and the line the shop spends its price
    // on. The cover's tint runs from the visibility at its top to the one at
    // its bottom, which gives the grid a per-pixel soft edge for free.
    void draw_card(gfx::DrawList &list, int k, const Rect &r, float alpha, unsigned layers) const
    {
        const ui::CardItem &item = grid_[static_cast<std::size_t>(k)];
        float top = 1.0f;
        float foot = 1.0f;
        fade_span(r.y, r.y + kCoverH, &top, &foot);
        if (top <= 0.01f && foot <= 0.01f)
            return;
        top *= alpha;
        foot *= alpha;
        const float lift = lift_[static_cast<std::size_t>(k)].value;
        const Rect cover{r.x, r.y, r.w, kCoverH};
        if (layers & kImages)
        {
            const float light = 0.84f + 0.16f * lift; // resting covers sit back a little
            if (item.texture != 0)
                list.image_gradient(item.texture, cover, item.uv,
                                    Color{light, light, light, top},
                                    Color{light, light, light, foot}, kCardRadius);
            else
                list.gradient_rect(cover, kCardRadius, item.top.with_alpha(top),
                                   item.bottom.with_alpha(foot));
        }
        const float marks = alpha * fade_at(r.y + 8.0f);
        if (marks > 0.01f && (layers & kShapes))
        {
            if (!item.badge.empty())
                badge_pill(list, item.badge, r.x + 10.0f, r.y + 26.0f, 32.0f, marks, item.accent);
            if (item.selected)
            {
                const float mark = r.x + r.w - 26.0f;
                list.circle(mark, r.y + 26.0f, 17.0f, Color::rgb(0x000000, 0.5f * marks));
                list.star(mark, r.y + 26.0f, 10.0f, kGold.with_alpha(marks));
            }
        }
        // On the focus plate the words step in from its edge.
        const float x = r.x + 2.0f + 10.0f * lift;
        const float title = alpha * fade_text(r.y + kCoverH + 38.0f, 24.0f);
        if (title > 0.01f && (layers & kSemibold))
            ui::text(list, glyphs_.semibold, grid_titles_[static_cast<std::size_t>(k)], x,
                     r.y + kCoverH + 38.0f, 24.0f, kInk.with_alpha(title * (0.84f + 0.16f * lift)));
        const float note = alpha * fade_text(r.y + kCoverH + 74.0f, 19.0f);
        if (note > 0.01f && (layers & kMono))
            ui::text(list, glyphs_.regular, grid_notes_[static_cast<std::size_t>(k)], x,
                     r.y + kCoverH + 74.0f, 19.0f, kInk.with_alpha(0.62f * note));
    }

    void draw_grid_card(gfx::DrawList &list, int k, float scroll, unsigned layers) const
    {
        Rect r = card_rect(k);
        r.y -= scroll;
        if (r.y > kViewBottom || r.y + r.h < kPageTop)
            return;
        const float in =
            tween::stagger(page_age_ - 0.3f, k % 5 + 2 * (k / 5), 0.05f, 0.45f) * enter_.value;
        const float lift = lift_[static_cast<std::size_t>(k)].value;
        const float nudge =
            k == grid_focus_ && zone_ == Zone::content ? ui::shake(nudge_.value, clock_) : 0.0f;
        list.push_transform(1.0f + kLift * lift, r.cx(), r.cy(), nudge * nudge_x_,
                            nudge * nudge_y_ + 26.0f * (1.0f - in));
        draw_card(list, k, r, in, layers);
        list.pop_transform();
    }

    // The grid, and the one focus object that glides over the whole page.
    void draw_grid(gfx::DrawList &list) const
    {
        const float scroll = grid_scroll_.value;
        const Rect window{0.0f, window_top(), kWidth, kViewBottom - window_top()};
        const int count = static_cast<int>(grid_.size());
        const int focused = zone_ == Zone::content && count > 0 ? grid_focus_ : -1;

        list.push_clip(window);
        // Layer by layer across the cards, not card by card, so the page costs
        // a handful of draw calls instead of one per element.
        for (const unsigned layer : {kImages, kShapes, kSemibold, kMono})
        {
            for (int k = 0; k < count; ++k)
            {
                if (k != focused) // that one is drawn last, over its neighbours
                    draw_grid_card(list, k, scroll, layer);
            }
        }
        if (count == 0)
        {
            const float in = tween::clamp01(page_age_ / 0.3f) * enter_.value;
            ui::text(list, glyphs_.semibold, "这一栏现在是空的", kCentreX, 760.0f, 30.0f,
                     kInk.with_alpha(0.86f * in), gfx::Align::center);
            ui::text(list, glyphs_.regular, "换一栏看看，或者回「设置」里重新连接。", kCentreX,
                     802.0f, 24.0f, kInk.with_alpha(0.6f * in), gfx::Align::center);
        }
        list.pop_clip();

        const float in = tween::stagger(page_age_, 4, 0.07f, 0.5f);
        Rect ring = ring_.value();
        const float shake = ui::shake(nudge_.value, clock_);
        ring.x += shake * nudge_x_;
        ring.y += shake * nudge_y_ - scroll;
        const float radius = ring_radius_.value;
        const float plate = plate_.value * in;
        if (plate > 0.01f)
        {
            list.shadow({ring.x, ring.y + 16.0f, ring.w, ring.h}, radius, 38.0f,
                        Color::rgb(0x000000, 0.55f * plate));
            list.glow(ring, radius, 30.0f,
                      glow_.value((0.26f + 0.12f * ui::breathe(clock_, 1.9f)) * plate));
            list.rounded_rect(ring, radius, kPanel.with_alpha(plate));
        }
        list.bordered_rect(ring, radius, kClear, 3.0f, kInk.with_alpha(0.94f * in));
        if (focused >= 0)
        {
            list.push_clip(window);
            draw_grid_card(list, focused, scroll, kAllLayers);
            list.pop_clip();
        }
    }

    // The page's springs: the sheet's scroll, the gliding ring, the lift of
    // one capsule and the pill that runs along the chips.
    void update_grid(float dt)
    {
        page_age_ += dt;
        grid_scroll_.target = scroll_target();
        grid_scroll_.update(dt, 11.0f);
        for (std::size_t i = 0; i < lift_.size(); ++i)
        {
            lift_[i].target = (zone_ == Zone::content && static_cast<int>(i) == grid_focus_)
                                  ? 1.0f
                                  : 0.0f;
            lift_[i].update(dt, 18.0f);
        }
        ring_.target(focus_target());
        ring_.update(dt, 20.0f);
        ring_radius_.target = focus_radius();
        ring_radius_.update(dt, 20.0f);
        const bool carded = zone_ == Zone::content && !grid_.empty();
        plate_.target = carded ? 1.0f : 0.0f;
        plate_.update(dt, 16.0f);
        glow_.target(carded ? grid_[static_cast<std::size_t>(grid_focus_)].accent : kAccent);
        glow_.update(dt, 10.0f);
        Rect chips[kRailCount];
        chip_layout(chips);
        chip_pill_.target(chips[static_cast<std::size_t>(chip_)]);
        chip_pill_.update(dt, 20.0f);
        // The row sticks under the top bar, so its pill must not spring there.
        chip_pill_.y.snap(chips[static_cast<std::size_t>(chip_)].y);
        nudge_.update(dt, 9.0f);
        chip_nudge_.update(dt, 9.0f);
    }

    // A new shelf is a new page: start at its top, with the ring already there.
    void reset_grid()
    {
        grid_focus_ = 0;
        page_age_ = 0.0f;
        grid_scroll_.snap(0.0f);
        grid_scroll_.target = 0.0f;
        ring_.snap(focus_target());
        ring_radius_.snap(focus_radius());
        plate_.snap(zone_ == Zone::content && !grid_.empty() ? 1.0f : 0.0f);
        glow_.snap(grid_.empty() ? kAccent : grid_.front().accent);
        Rect chips[kRailCount];
        chip_layout(chips);
        chip_pill_.snap(chips[static_cast<std::size_t>(chip_)]);
    }

    // The cards themselves, with their words fitted once instead of per frame.
    void set_grid(std::vector<ui::CardItem> cards)
    {
        grid_ = std::move(cards);
        grid_titles_.clear();
        grid_notes_.clear();
        lift_.clear();
        for (const ui::CardItem &item : grid_)
        {
            grid_titles_.push_back(
                glyphs_.semibold.font->fit(item.title, 24.0f, kCardW - 24.0f));
            grid_notes_.push_back(glyphs_.regular.font->fit(item.subtitle, 19.0f, kCardW - 24.0f));
            lift_.emplace_back();
        }
        grid_focus_ = std::clamp(grid_focus_, 0, std::max(0, static_cast<int>(grid_.size()) - 1));
    }

    // An edge of the page: a quiet "no", unless it is the left edge, which is
    // how this product hands the focus back to the rail.
    void refuse_grid(app::Feedback &feedback, bool repeat, float dx, float dy)
    {
        if (repeat)
            return;
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_.trigger();
        nudge_x_ = dx;
        nudge_y_ = dy;
    }

    void tick_card(app::Feedback &feedback) const
    {
        // Rows further down sound a little lower.
        const float pitch =
            std::max(0.82f, 1.06f - 0.04f * static_cast<float>(grid_focus_ / 5));
        feedback.play(audio::Cue::focus, pitch, ui::pan_for_x(card_rect(grid_focus_).cx()));
    }

    void navigate_grid(const InputFrame &input, app::Feedback &feedback)
    {
        const int count = static_cast<int>(grid_.size());
        const int column = grid_focus_ % 5;
        const int row = grid_focus_ / 5;
        switch (input.nav)
        {
        case Direction::left:
            if (column == 0)
            {
                if (input.nav_repeat)
                    return;
                zone_ = Zone::chips;
                feedback.play(audio::Cue::focus, 1.0f, -0.6f, 0.8f);
                return;
            }
            --grid_focus_;
            break;
        case Direction::right:
            if (column == 4 || grid_focus_ + 1 >= count)
            {
                refuse_grid(feedback, input.nav_repeat, 1.0f, 0.0f);
                return;
            }
            ++grid_focus_;
            break;
        case Direction::up:
            if (row == 0)
            {
                if (input.nav_repeat)
                    return;
                zone_ = Zone::chips;
                feedback.play(audio::Cue::focus, 1.1f, ui::pan_for_x(ring_.x.value));
                return;
            }
            grid_focus_ -= 5;
            break;
        case Direction::down:
            if (grid_focus_ + 5 < count)
                grid_focus_ += 5;
            else if ((count - 1) / 5 > row)
                grid_focus_ = count - 1; // a short last row: land on its last card
            else if (fn_.current() != nullptr && !input.nav_repeat)
            {
                // The bottom edge of the tiles is where the player bar lives: ▼
                // raises it over the grid and ✕ puts it back down.
                zone_ = Zone::transport;
                feedback.play(audio::Cue::focus, 1.0f, 0.0f, 0.8f);
                return;
            }
            else
            {
                refuse_grid(feedback, input.nav_repeat, 0.0f, 1.0f);
                return;
            }
            break;
        case Direction::none:
            return;
        }
        tick_card(feedback);
    }

    void draw_top_bar(ui::Canvas &canvas, const Library &lib) const
    {
        gfx::DrawList &list = canvas.list;
        list.push_opacity(enter_.value);
        const fnos::Track *track = fn_.current();
        fnos::Player &player = fn_.player();
        if (track == nullptr)
        {
            list.rotated_rect({kMargin, kTopBarY - 11.0f, 22.0f, 22.0f}, 5.0f, 0.7854f, kAccent);
            list.rotated_rect({kMargin + 6.0f, kTopBarY - 5.0f, 10.0f, 10.0f}, 2.0f, 0.7854f, kCoal);
            ui::text(list, glyphs_.semibold, "飞牛音乐", kMargin + 42.0f, kTopBarY + 8.0f, 24.0f,
                     kInk, gfx::Align::left, 3.0f);
        }
        else
        {
            const bool live = player.state() == fnos::PlayerState::playing;
            const Rect thumb{kMargin, kTopBarY - 28.0f, 44.0f, 44.0f};
            draw_square(list, thumb, 12.0f, transport_gl_, transport_uv_);
            list.bordered_rect(thumb, 12.0f, kClear, 1.0f, kInk.with_alpha(0.14f));
            // A dot that breathes while the song runs: the bar says this is
            // playing, not merely that a song is loaded.
            list.circle(thumb.x + thumb.w + 24.0f, thumb.cy(), 5.0f,
                        kAccent.with_alpha(live ? 0.55f + 0.45f * ui::breathe(clock_, 1.9f)
                                                : 0.35f));
            float x = thumb.x + thumb.w + 42.0f;
            const std::string title = glyphs_.semibold.font->fit(track->title, 24.0f, 620.0f);
            ui::text(list, glyphs_.semibold, title, x, kTopBarY + 8.0f, 24.0f, kInk);
            x += glyphs_.semibold.measure(title, 24.0f) + 18.0f;
            const std::string artist = glyphs_.regular.font->fit(
                track->artist.empty() ? std::string("飞牛音乐") : track->artist, 19.0f, 420.0f);
            ui::text(list, glyphs_.regular, artist, x, kTopBarY + 8.0f, 19.0f,
                     kInk.with_alpha(0.6f));
        }

        // The right end: how far into the song, or whose account this is.
        if (track != nullptr && player.duration() > 0)
        {
            const std::string run =
                clock_text(player.position()) + " / " + clock_text(player.duration());
            ui::text(list, glyphs_.mono, run, kRight, kTopBarY + 8.0f, 20.0f,
                     kInk.with_alpha(0.7f), gfx::Align::right);
        }
        else
        {
            const std::string account = lib.signed_in()
                                            ? (username_.empty() ? std::string("已登录") : username_)
                                            : std::string("未登录");
            ui::text(list, glyphs_.regular, account, kRight, kTopBarY + 7.0f, 21.0f,
                     kInk.with_alpha(0.6f), gfx::Align::right);
        }
        // The left stick turns the volume with no button focused anywhere, and
        // over the storefront the bar that carries its slider is put away: the
        // number rides under the clock for as long as the stick is moving.
        if (volume_hud_ > 0.0f && !volume_read_.empty())
        {
            ui::text(list, glyphs_.mono, volume_read_, kRight, kTopBarY + 42.0f, 22.0f,
                     kInk.with_alpha(std::min(1.0f, volume_hud_) * 0.9f), gfx::Align::right);
        }
        const std::string message = banner(lib);
        // A login that is working, that failed, or that was refused, has to say
        // so where the eyes already are: a pill over the sheet, not a quiet line
        // in the corner the player has already stopped reading.
        const bool shout = lib.busy() || lib.failed() || !notice_.empty();
        if (!message.empty() && shout)
        {
            const Color base = (lib.failed() || !notice_.empty()) ? theme_.danger : kAccent;
            const float w = glyphs_.regular.measure(message, 22.0f) + 62.0f;
            const Rect pill{kCentreX - w * 0.5f, kTopBarY - 22.0f, w, 40.0f};
            list.rounded_rect(pill, 20.0f, base.with_alpha(0.16f));
            list.bordered_rect(pill, 20.0f, kClear, 1.5f, base.with_alpha(0.5f));
            if (lib.busy())
                list.arc(pill.x + 24.0f, pill.cy(), 8.0f, 2.6f, clock_ * 2.0f, 6.283185f, base);
            else
                list.circle(pill.x + 24.0f, pill.cy(), 5.0f, base);
            ui::text(list, glyphs_.regular, message, pill.x + 42.0f, pill.cy() + 8.0f, 22.0f, kInk);
        }
        else if (!message.empty())
        {
            ui::text(list, glyphs_.regular, message, kRight, kTopBarY + 40.0f, 19.0f,
                     kInk.with_alpha(0.55f), gfx::Align::right);
        }
        list.pop_opacity();
    }

    // The featured plate: artwork on the right, the shelf's words on the left.
    void draw_banner(ui::Canvas &canvas, const Library &lib) const
    {
        gfx::DrawList &list = canvas.list;
        // The plate rides the page: scrolling the sheet lifts it under the top
        // bar and away, and once it is gone it costs nothing to draw.
        const float scroll = grid_scroll_.value;
        const float fade = tween::clamp01(1.0f - scroll / 360.0f);
        if (fade <= 0.004f)
            return;
        const Rect b{kMargin, kBannerY - scroll, kWidth - 2.0f * kMargin, kBannerH};
        list.push_opacity(enter_.value * fade);
        const Color tone = banner_tone_;
        // Nothing here is a rectangle any more. A shelf's colour is a wash with
        // blurred edges that sinks into the backdrop; a song's plate carries no
        // wash at all, because the record's own picture is the colour.
        if (!banner_song_)
            list.shadow({b.x, b.y + 12.0f, b.w, b.h}, kBannerRadius, 130.0f, tone.with_alpha(0.5f));

        const Rect art{b.x + b.w - kBannerArtW, b.y, kBannerArtW, b.h};
        if (banner_gl_ != 0)
            // The picture lets go of the page at its foot rather than ending in
            // a line across it.
            list.image_gradient(banner_gl_, art, banner_uv_, kWhite.with_alpha(0.88f),
                                kWhite.with_alpha(0.16f), kBannerRadius);
        else if (banner_song_)
        {
            // No artwork for this song: a picture-shaped hollow, so the words
            // do not run out into an empty page.
            list.shadow({art.x, b.y + 8.0f, art.w, art.h}, kBannerRadius, 70.0f,
                        kPanel.with_alpha(0.55f));
            draw_note(list, art, kInk.with_alpha(0.35f));
        }
        else
        {
            list.shadow({art.x, b.y + 8.0f, art.w, art.h}, kBannerRadius, 70.0f,
                        tone.with_alpha(0.75f));
            draw_note(list, art, kInk.with_alpha(0.5f));
        }

        const float x = b.x + 56.0f;
        list.rounded_rect({x, b.y + 71.0f, 28.0f, 3.0f}, 1.5f, kAccent);
        ui::text(list, glyphs_.semibold, banner_kicker_, x + 40.0f, b.y + 80.0f, 18.0f, kAccent,
                 gfx::Align::left, 4.0f);
        ui::text(list, glyphs_.display, glyphs_.display.font->fit(banner_title_, 66.0f, 700.0f),
                 x - 3.0f, b.y + 164.0f, 66.0f, kInk);
        ui::text(list, glyphs_.regular, glyphs_.regular.font->fit(banner_note_, 27.0f, 660.0f), x,
                 b.y + 212.0f, 27.0f, kInk.with_alpha(0.78f));

        if (banner_song_)
        {
            // One line of the words, the one the singer is on. It follows the
            // play position, so it is read here rather than stored with the rest
            // of the plate, which is rebuilt only when the song changes.
            const std::vector<fnos::LyricLine> &lines = lib.lyric();
            const int position = fn_.player().position();
            std::string line;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i].at <= position)
                    line = lines[i].text;
            }
            if (line.empty() && !lines.empty())
                line = lines.back().text;
            if (line.empty())
                line = lib.busy() ? "正在取歌词…" : "这首歌没有歌词";
            ui::text(list, glyphs_.semibold, glyphs_.semibold.font->fit(line, 26.0f, 660.0f), x,
                     b.y + 250.0f, 26.0f, kInk.with_alpha(0.7f));
        }

        // The figure, where the sheet has its price: how much of the library
        // this shelf actually holds.
        if (!banner_stat_.empty())
        {
            constexpr float size = 46.0f;
            const float radius = size * 0.28f;
            const float cy = b.y + 296.0f - size * 0.35f;
            const float stat_x = x + radius * 2.0f + size * 0.24f;
            if (banner_song_)
            {
                // A song has no count to price with; the length it holds is the
                // same figure in the same slot.
                ui::text(list, glyphs_.mono, banner_stat_, x, b.y + 296.0f, size, kInk);
            }
            else
            {
                list.circle(x + radius, cy, radius, kInk.with_alpha(0.85f));
                list.circle(x + radius, cy, radius * 0.42f, tone);
                ui::text(list, glyphs_.mono, banner_stat_, stat_x, b.y + 296.0f, size, kInk);
                ui::text(list, glyphs_.semibold, banner_unit_,
                         stat_x + glyphs_.mono.measure(banner_stat_, size) + 10.0f, b.y + 296.0f,
                         24.0f, kInk.with_alpha(0.72f));
            }
        }

        if (banner_song_)
        {
            // The band the 打开这一栏 pill used to own belongs to the song now:
            // bars that move with what is playing. They start clear of the
            // length's digits and stop short of the picture. The pill is gone on
            // a shelf's plate too: ✕ on the row of chips already opens it, and
            // the hint line at the foot of the page says so.
            const float left = x + 190.0f;
            const float wide = art.x - 40.0f - left;
            if (wide > 120.0f)
                draw_spectrum(list, left, wide, b.y + 372.0f, 88.0f, false);
        }

        list.pop_opacity();
    }

    // The shelf selector, as design 13 draws its filter row: an L2 glyph, one
    // white pill that glides between the chips, then outlines, labels and
    // counts in three passes, and an R2 glyph to close it.
    void draw_chips(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        const Library &lib = fn_.library();
        Rect chips[kRailCount];
        chip_layout(chips);
        const float cy = chips_y() + kChipH * 0.5f;
        list.push_opacity(enter_.value);
        ui::draw_button(list, glyphs_, style, ui::Button::l2, kMargin, cy, 30.0f);

        // The active shelf is one pill that glides between the chips; whichever
        // chip it covers has to change its ink, so the overlap is measured.
        Rect pill = chip_pill_.value();
        pill.x += ui::shake(chip_nudge_.value, clock_) * chip_nudge_x_;
        list.rounded_rect(pill, kChipH * 0.5f, kInk);
        for (int pass = 0; pass < 3; ++pass)
        {
            for (int i = 0; i < kRailCount; ++i)
            {
                const Rect &chip = chips[static_cast<std::size_t>(i)];
                const float overlap =
                    std::min(pill.x + pill.w, chip.x + chip.w) - std::max(pill.x, chip.x);
                const float on = tween::clamp01(overlap / chip.w);
                const Color ink = gfx::mix(kInk.with_alpha(0.78f), kCoal, on);
                if (pass == 0)
                {
                    list.bordered_rect(chip, kChipH * 0.5f, kClear, 1.5f,
                                       kInk.with_alpha(0.2f * (1.0f - on)));
                    // A row list has no page ring of its own to glide here, so
                    // the focused chip has to wear its own.
                    if (i == chip_ && zone_ == Zone::chips && !show_cards(lib))
                        list.bordered_rect({chip.x - 6.0f, chip.y - 6.0f, chip.w + 12.0f,
                                            chip.h + 12.0f},
                                           kChipH * 0.5f + 6.0f, kClear, 3.0f, kAccent);
                }
                else if (pass == 1)
                    ui::text(list, glyphs_.semibold, kRail[static_cast<std::size_t>(i)].label,
                             chip.x + 24.0f, centred(cy, 22.0f), 22.0f, ink);
                else
                {
                    const std::string count =
                        chip_count(kRail[static_cast<std::size_t>(i)].shelf, lib);
                    if (count.empty())
                        continue;
                    const float label =
                        glyphs_.semibold.measure(kRail[static_cast<std::size_t>(i)].label, 22.0f);
                    ui::text(list, glyphs_.mono, count, chip.x + 34.0f + label, centred(cy, 18.0f),
                             18.0f, ink.with_alpha(0.6f));
                }
            }
        }
        const Rect &last = chips[kRailCount - 1];
        ui::draw_button(list, glyphs_, style, ui::Button::r2, last.x + last.w + 16.0f, cy, 30.0f);
        list.pop_opacity();
    }

    // The setup shelf's side panel: how loud this console plays, and what it is
    // connected to. The rows beside it stay the only place that edits anything.
    void draw_setup_side(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        const Rect panel{kRight - 460.0f, kGridY, 460.0f, kViewBottom - kGridY};
        list.push_opacity(enter_.value);
        list.shadow({panel.x, panel.y + 10.0f, panel.w, panel.h}, kBannerRadius, 26.0f,
                    Color::rgb(0x000000, 0.4f));
        list.rounded_rect(panel, kBannerRadius, kPanel.with_alpha(0.8f));
        list.bordered_rect(panel, kBannerRadius, kClear, 1.5f, kInk.with_alpha(0.08f));

        ui::text(list, glyphs_.semibold, "本机播放", panel.x + 32.0f, panel.y + 40.0f, 20.0f,
                 kInk.with_alpha(0.6f), gfx::Align::left, 3.0f);

        // The volume as a row of cells, the way the kit's own page shows levels.
        constexpr int kCells = 20;
        const float level = tween::clamp01(fn_.volume());
        const float cell_w = (panel.w - 64.0f) / static_cast<float>(kCells);
        const float bar_y = panel.y + 56.0f;
        for (int i = 0; i < kCells; ++i)
        {
            const bool lit = static_cast<float>(i) / static_cast<float>(kCells) < level;
            list.rounded_rect({panel.x + 32.0f + static_cast<float>(i) * cell_w + 1.0f, bar_y,
                               cell_w - 3.0f, 12.0f},
                              3.0f, lit ? kAccent : kInk.with_alpha(0.12f));
        }
        char text[24];
        std::snprintf(text, sizeof(text), "%d%%", static_cast<int>(level * 100.0f + 0.5f));
        ui::text(list, glyphs_.mono, text, panel.x + panel.w - 32.0f, panel.y + 40.0f, 22.0f,
                 kInk.with_alpha(0.7f), gfx::Align::right);
        // The kit draws its own button shapes: the □ character is not in the
        // baked Chinese atlas, and on the console it came out as a question
        // mark in the middle of this sentence.
        const float note = panel.y + 96.0f;
        const float typed = ui::text(list, glyphs_.regular, "音量在播放条里调：按", panel.x + 32.0f,
                                     note, 21.0f, kInk.with_alpha(0.55f));
        const ui::GlyphStyle caps = ui::GlyphStyle::dark();
        const float glyph_x = panel.x + 32.0f + typed + 7.0f;
        ui::draw_button(list, glyphs_, caps, ui::Button::square, glyph_x, note - 7.0f, 20.0f);
        ui::text(list, glyphs_.regular, "进播放页",
                 glyph_x + ui::button_width(ui::Button::square, 20.0f) + 7.0f, note, 21.0f,
                 kInk.with_alpha(0.55f));

        ui::text(list, glyphs_.semibold, "飞牛音乐PS5端开发者：Naye", panel.x + 32.0f,
                 panel.y + 176.0f, 22.0f, kInk.with_alpha(0.9f));
        ui::text(list, glyphs_.regular, "QQ群：310630593", panel.x + 32.0f, panel.y + 214.0f,
                 20.0f, kInk.with_alpha(0.6f));
        ui::text(list, glyphs_.mono, version_.empty() ? "版本 未知" : "版本 " + version_,
                 panel.x + 32.0f, panel.y + 252.0f, 20.0f, kInk.with_alpha(0.6f));
        ui::text(list, glyphs_.regular, "界面框架 ps5-homebrew-ui", panel.x + 32.0f,
                 panel.y + panel.h - 48.0f, 19.0f, kInk.with_alpha(0.45f));
        ui::text(list, glyphs_.regular, "作者 BlackBearReloaded · GPL-3.0", panel.x + 32.0f,
                 panel.y + panel.h - 22.0f, 19.0f, kInk.with_alpha(0.45f));
        list.pop_opacity();
    }

    // Now Playing: the artwork breathes, one pseudo-spectrum drives the bars,
    // the queue rides alongside, and the transport below still owns the seek.
    void draw_playing(ui::Canvas &canvas, const Library &lib) const
    {
        gfx::DrawList &list = canvas.list;
        const fnos::Track *track = fn_.current();
        fnos::Player &player = fn_.player();
        const bool live = player.state() == fnos::PlayerState::playing;
        const float in = enter_.value;
        list.push_opacity(in);

        const Rect art{kMargin, kPlayArtY, kPlayArt, kPlayArt};
        const Color plate = track != nullptr ? plate_color(track->guid + track->title) : kAccent;
        const float pulse = 0.5f + 0.5f * ui::breathe(clock_, 2.6f);
        const float scale = tween::lerp(kPlayPausedScale, 1.0f, live ? 1.0f : 0.0f) +
                            0.008f * (live ? pulse : 0.0f);
        list.push_transform(scale, art.cx(), art.cy(), 0.0f, 0.0f);
        list.glow(art.inset(-4.0f), kPlayArtRadius, 90.0f + 60.0f * pulse,
                  plate.with_alpha((0.14f + 0.30f * pulse) * (live ? 1.0f : 0.3f)));
        list.shadow({art.x, art.y + 28.0f, art.w, art.h}, kPlayArtRadius, 50.0f,
                    Color::rgb(0x000000, 0.5f));
        if (transport_gl_ != 0)
            list.image(transport_gl_, art, transport_uv_,
                       live ? kWhite : gfx::mix(Color::rgb(0xb9bcc8), kWhite, 0.4f),
                       kPlayArtRadius);
        else
        {
            list.gradient_rect(art, kPlayArtRadius, gfx::mix(plate, kWhite, 0.22f), plate);
            draw_note(list, art, kInk.with_alpha(0.85f));
        }
        list.bordered_rect(art, kPlayArtRadius, kClear, 2.0f, kWhite.with_alpha(0.16f));
        list.pop_transform();

        const float column_x = art.x + art.w + kPlayColumnGap;
        const float column_w = kRight - kQueueW - kPlayColumnGap - column_x;
        const std::string title =
            track != nullptr ? track->title : (lib.signed_in() ? "选一首歌" : "先连上这台 NAS");
        const std::string artist = track != nullptr ? track->artist : std::string("飞牛音乐");

        ui::text(list, glyphs_.semibold, live ? "正在播放" : "已暂停", column_x, 206.0f, 18.0f,
                 kAccent, gfx::Align::left, 4.0f);
        char count[24];
        std::snprintf(count, sizeof(count), "%02d / %02d",
                      std::max(0, fn_.index() + 1), std::max(0, fn_.queue_size()));
        ui::text(list, glyphs_.mono, count, column_x + column_w, 206.0f, 20.0f,
                 kInk.with_alpha(0.6f), gfx::Align::right);
        ui::text(list, glyphs_.display, glyphs_.display.font->fit(title, 64.0f, column_w), column_x - 3.0f,
                 300.0f, 64.0f, kInk);
        ui::text(list, glyphs_.semibold, glyphs_.semibold.font->fit(artist, 30.0f, column_w),
                 column_x, 348.0f, 30.0f, kInk.with_alpha(0.92f));
        if (track != nullptr && !track->album.empty())
            ui::text(list, glyphs_.regular, glyphs_.regular.font->fit(track->album, 24.0f, column_w),
                     column_x, 386.0f, 24.0f, kInk.with_alpha(0.62f));
        const std::string status = fnos::player_error_text(player.error());
        if (!status.empty())
            ui::text(list, glyphs_.regular, status, column_x, 420.0f, 21.0f, theme_.danger);

        draw_spectrum(list, column_x, column_w);

        // The queue: a frosted column down the right edge, as on Now Playing.
        const Rect panel{kRight - kQueueW, kQueueY, kQueueW, transport_.y - kQueueY - 16.0f};
        list.shadow({panel.x, panel.y + 10.0f, panel.w, panel.h}, kQueueRadius, 26.0f,
                    Color::rgb(0x000000, 0.4f));
        if (canvas.glass != 0)
            list.glass(canvas.glass, panel, kQueueRadius, kWhite.with_alpha(0.06f));
        else
            list.rounded_rect(panel, kQueueRadius, kPanel.with_alpha(0.86f));
        const std::vector<fnos::Track> &queue = fn_.queue();
        const int index = fn_.index();
        char head[24];
        std::snprintf(head, sizeof(head), "%d",
                      std::max(0, static_cast<int>(queue.size()) - index - 1));
        ui::text(list, glyphs_.semibold, "队列", panel.x + 26.0f, panel.y + 44.0f, 20.0f,
                 kInk.with_alpha(0.6f), gfx::Align::left, 3.0f);
        // The count in the mono face, the words in the one that has them: the
        // first font carries no Chinese, so a 首 drawn with it comes out a ?.
        ui::text(list, glyphs_.mono, head, panel.x + panel.w - 96.0f, panel.y + 44.0f, 18.0f,
                 kInk.with_alpha(0.4f), gfx::Align::right);
        ui::text(list, glyphs_.semibold, "首在后", panel.x + panel.w - 26.0f, panel.y + 44.0f,
                 18.0f, kInk.with_alpha(0.4f), gfx::Align::right);
        int row = 0;
        for (int at = std::max(0, index); at < static_cast<int>(queue.size()); ++at)
        {
            const float line_y = panel.y + 96.0f + kQueueRow * static_cast<float>(row);
            if (line_y > panel.y + panel.h - 20.0f)
                break;
            const fnos::Track &item = queue[static_cast<std::size_t>(at)];
            const bool current = at == index;
            if (current)
            {
                list.rounded_rect({panel.x + 14.0f, line_y - 34.0f, panel.w - 28.0f, 50.0f}, 14.0f,
                                  kInk.with_alpha(0.10f));
                list.circle(panel.x + 34.0f, line_y - 9.0f, 3.5f, kAccent);
            }
            char number[8];
            std::snprintf(number, sizeof(number), "%02d", at + 1);
            ui::text(list, glyphs_.mono, number, panel.x + 52.0f, line_y, 19.0f,
                     current ? kAccent : kInk.with_alpha(0.4f));
            ui::text(list, current ? glyphs_.semibold : glyphs_.regular,
                     glyphs_.regular.font->fit(item.title, 24.0f, panel.w - 170.0f),
                     panel.x + 100.0f, line_y, 24.0f, current ? kInk : kInk.with_alpha(0.85f));
            if (item.favorite)
                list.star(panel.x + panel.w - 122.0f, line_y - 8.0f, 8.0f, kGold);
            if (item.duration > 0)
                ui::text(list, glyphs_.mono, clock_text(item.duration), panel.x + panel.w - 26.0f,
                         line_y, 19.0f, kInk.with_alpha(0.45f), gfx::Align::right);
            ++row;
        }
        list.pop_opacity();
    }

    // One pseudo-spectrum: a beat plus per-bar sines, fast to rise and slow to
    // fall, so the bars move with the music even without an FFT.
    void update_spectrum(float dt, const fnos::Player &player)
    {
        const bool live = player.state() == fnos::PlayerState::playing;
        beat_ += dt * (live ? 1.0f : 0.15f);
        const float pulse = live ? std::max(0.0f, std::sin(beat_ * 3.4f)) : 0.0f;
        for (int i = 0; i < kBars; ++i)
        {
            const float f = static_cast<float>(i) / static_cast<float>(kBars);
            const float wobble = live
                                     ? 0.45f + 0.35f * std::sin(clock_ * (1.6f + 2.4f * f) + f * 9.0f)
                                     : 0.06f + 0.04f * std::sin(f * 7.0f);
            const float shape = 1.0f - 0.55f * std::fabs(f - 0.35f);
            const float want = tween::clamp01(wobble * shape + 0.35f * pulse * shape);
            float &level = levels_[static_cast<std::size_t>(i)];
            level = want > level ? want : tween::lerp(level, want, 1.0f - std::exp(-4.0f * dt));
            float &peak = peaks_[static_cast<std::size_t>(i)];
            peak = std::max(level, peak - 1.4f * dt);
        }
    }

    // The bars stand on a baseline and rise no higher than `tall`. The Now
    // Playing page uses the page's own band; the storefront plate borrows the
    // same levels at a tenth of the height.
    void draw_spectrum(gfx::DrawList &list, float x, float width, float base = kBarsBase,
                       float tall = kBarsHeight, bool mirror = true) const
    {
        const float bar_w = std::min(width / static_cast<float>(kBars) * 0.56f, tall * 0.5f);
        const float pitch = (width - bar_w) / static_cast<float>(kBars - 1);
        const Color top = gfx::mix(kAccent, kWhite, 0.35f);
        const Color bottom = gfx::mix(kAccent, kCoal, 0.45f);
        for (int i = 0; i < kBars; ++i)
        {
            const float level = levels_[static_cast<std::size_t>(i)];
            const float h = bar_w + level * (tall - bar_w);
            const float bx = x + static_cast<float>(i) * pitch;
            list.gradient_rect({bx, base - h, bar_w, h}, bar_w * 0.5f,
                               gfx::mix(bottom, top, 0.35f + 0.65f * level), bottom);
            if (!mirror)
                continue;
            // The floor is a mirror: a short, fading copy below the line.
            const float copy = bar_w + (h - bar_w) * 0.42f;
            list.gradient_rect({bx, base + 8.0f, bar_w, copy}, bar_w * 0.5f,
                               bottom.with_alpha(0.3f), bottom.with_alpha(0.0f));
            const float peak = peaks_[static_cast<std::size_t>(i)];
            if (peak - level > 0.02f)
                list.rounded_rect({bx, base - bar_w - peak * (tall - bar_w) - 8.0f, bar_w, 3.0f},
                                  1.5f, top.with_alpha(0.5f));
        }
    }

    std::string banner(const Library &lib) const
    {
        if (!notice_.empty())
            return notice_;
        if (!lib.message().empty())
            return lib.message();
        if (lib.shelf() == Shelf::search && !lib.query().empty())
            return "搜索：" + lib.query();
        if (lib.shelf() == Shelf::songs && lib.tracks().empty())
            return "这台 NAS 上还没有歌曲";
        return std::string();
    }

    void draw_lyric(ui::Canvas &canvas, const Library &lib) const
    {
        gfx::DrawList &list = canvas.list;
        ui::Painter paint(list, glyphs_, theme_, canvas.glass);
        const Rect panel = content_;
        list.push_opacity(0.94f);
        list.shadow(panel, theme_.radius_card, 28.0f, theme_.shadow);
        paint.panel(panel);
        list.pop_opacity();

        const std::vector<fnos::LyricLine> &lines = lib.lyric();
        const int position = fn_.player().position();
        int current = -1;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].at <= position)
                current = static_cast<int>(i);
        }
        const float centre = panel.cy();
        if (lines.empty())
        {
            const char *text = lib.busy() ? "正在取歌词…" : "这首歌没有歌词";
            paint.body(text, panel.cx(), centre, 26.0f, theme_.text_muted, gfx::Align::center);
            return;
        }
        list.push_clip({panel.x, panel.y + 24.0f, panel.w, panel.h - 48.0f});
        const int anchor = current < 0 ? 0 : current;
        const int rows = static_cast<int>(lines.size());
        for (int at = std::max(0, anchor - 4); at < std::min(rows, anchor + 5); ++at)
        {
            const int offset = at - anchor;
            const float y = centre + 54.0f * static_cast<float>(offset);
            const float near = 1.0f - std::min(1.0f, std::fabs(static_cast<float>(offset)) / 4.0f);
            list.push_opacity(offset == 0 ? 1.0f : 0.3f + 0.5f * near);
            paint.body(lines[static_cast<std::size_t>(at)].text, panel.cx(), y,
                       offset == 0 ? 32.0f : 24.0f, offset == 0 ? theme_.text : theme_.text_muted,
                       gfx::Align::center);
            list.pop_opacity();
        }
        list.pop_clip();
    }

    void draw_keyboard(ui::Canvas &canvas) const
    {
        gfx::DrawList &list = canvas.list;
        ui::Painter paint(list, glyphs_, theme_, canvas.glass);
        list.push_opacity(0.5f);
        list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, theme_.page);
        list.pop_opacity();

        const Rect board = keys_.bounds();
        const Rect plate{board.x - 28.0f, board.y - 108.0f, board.w + 56.0f, board.h + 136.0f};
        list.shadow(plate, theme_.radius_card, 26.0f, theme_.shadow);
        paint.panel(plate);
        paint.label(field_name(editing_), plate.x + 28.0f, plate.y + 40.0f, 20.0f,
                    theme_.text_muted);
        const Rect field{plate.x + 28.0f, plate.y + 56.0f, plate.w - 56.0f, 56.0f};
        // A password stays dots on screen: the TV may be in a shared room.
        if (editing_ == Field::password)
            paint.field(field, std::string(edit_buffer().size(), '*'), true, ui::Look{});
        else
            paint.field(field, edit_buffer(), true, ui::Look{});
        keys_.draw(canvas);
    }

    void draw_hints(ui::Canvas &canvas, const Library &lib) const
    {
        gfx::DrawList &list = canvas.list;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        ui::HintLayout layout;
        layout.cy = kHeight - 22.0f;
        layout.size = 32.0f;
        layout.text_size = 20.0f;
        std::vector<ui::Hint> hints;
        if (zone_ == Zone::keyboard)
        {
            hints.push_back({ui::Button::cross, "输入"});
            hints.push_back({ui::Button::circle, "返回"});
            hints.push_back({ui::Button::square, "删除"});
            hints.push_back({ui::Button::triangle, "空格"});
            hints.push_back({ui::Button::l2, "大写"});
            hints.push_back({ui::Button::r2, "符号"});
            hints.push_back({ui::Button::options, "完成"});
        }
        else if (zone_ == Zone::transport)
        {
            hints.push_back({ui::Button::cross, fn_.player().state() == fnos::PlayerState::paused
                                                    ? "继续播放"
                                                    : "暂停"});
            hints.push_back({ui::Button::dpad, "选按钮"});
            if (now_)
                hints.push_back({ui::Button::square, "回主页"});
            else
                hints.push_back({ui::Button::circle, "收起播放条"});
            hints.push_back({ui::Button::right_stick, "快进快退"});
            hints.push_back({ui::Button::left_stick, "音量"});
            hints.push_back({ui::Button::l1, "上一曲"});
            hints.push_back({ui::Button::r1, "下一曲"});
        }
        else if (zone_ == Zone::chips)
        {
            hints.push_back({ui::Button::cross, "打开这一栏"});
            hints.push_back({ui::Button::options, "刷新这一栏"});
            hints.push_back({ui::Button::dpad, "左右选分类，下键进内容"});
        }
        else if (lib.shelf() == Shelf::setup)
        {
            hints.push_back({ui::Button::cross, "编辑这一项"});
            hints.push_back({ui::Button::circle, "返回导航"});
            hints.push_back({ui::Button::l2, "上一栏"});
            hints.push_back({ui::Button::r2, "下一栏"});
        }
        else if (show_cards(lib))
        {
            hints.push_back({ui::Button::cross, "打开"});
            hints.push_back({ui::Button::circle, "返回导航"});
            hints.push_back({ui::Button::square, now_ ? "收起封面" : "大封面"});
            hints.push_back({ui::Button::l2, "上一栏"});
            hints.push_back({ui::Button::r2, "下一栏"});
        }
        else
        {
            hints.push_back({ui::Button::cross, "播放这首"});
            hints.push_back({ui::Button::circle, "返回"});
            hints.push_back({ui::Button::options, "收藏"});
            hints.push_back({ui::Button::triangle, "歌词"});
            hints.push_back({ui::Button::right_stick, "快进快退"});
            hints.push_back({ui::Button::left_stick, "音量"});
            hints.push_back({ui::Button::l1, "上一曲"});
            hints.push_back({ui::Button::r1, "下一曲"});
        }
        ui::draw_hints(list, glyphs_, style, hints.data(), static_cast<int>(hints.size()),
                       kWidth - kMargin, true, layout);
    }

    // ---- input ----

    // The chips row: left and right walk the shelves, down or confirm opens one.
    void on_chips(const InputFrame &input, Library &lib, app::Feedback &feedback)
    {
        int next = chip_;
        if (input.nav == Direction::left)
            --next;
        else if (input.nav == Direction::right)
            ++next;
        if (next != chip_)
        {
            if (next < 0 || next >= kRailCount)
                feedback.play(audio::Cue::error);
            else
            {
                chip_ = next;
                feedback.play(audio::Cue::tab);
            }
            return;
        }
        if (input.nav == Direction::down || input.is_pressed(Action::confirm))
        {
            open_chip(lib, feedback);
            return;
        }
        if (input.is_pressed(Action::menu))
        {
            // The NAS has gained songs since this console last looked.
            if (lib.shelf() == Shelf::setup)
                feedback.play(audio::Cue::error);
            else
            {
                lib.refresh();
                feedback.play(audio::Cue::open);
                rebuild_ = true;
            }
            return;
        }
        if (input.nav == Direction::up)
        {
            feedback.play(audio::Cue::error);
            return;
        }
        if (input.is_pressed(Action::back))
        {
            // The chips row is the home level: ○ has nowhere left to climb to, so
            // it says so rather than dropping the focus into the list.
            feedback.play(audio::Cue::error);
        }
    }

    // L2 / R2: the shelf to the left and the shelf to the right, wrapping at the
    // ends, and the page turns with the pill so one press is one visible change.
    void step_rail(int dir, Library &lib, app::Feedback &feedback)
    {
        chip_ = (chip_ + dir + kRailCount) % kRailCount;
        open_chip(lib, feedback);
    }

    void step_song(int dir, app::Feedback &feedback)
    {
        if (fn_.current() == nullptr)
        {
            feedback.play(audio::Cue::error);
            return;
        }
        if (dir > 0)
            fn_.next();
        else
            fn_.previous();
        feedback.play(audio::Cue::tab, 1.0f, dir > 0 ? 0.3f : -0.3f);
        rebuild_ = true;
    }

    // The right stick is the fast-forward. No button has to win the player
    // bar's focus for it: the position moves wherever the stick is pushed,
    // and it keeps moving while the stick stays there.
    void on_scrub_stick(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        const float x = input.stick2_x;
        if (std::fabs(x) < kScrubEdge || fn_.current() == nullptr)
        {
            scrub_left_ = 0.0f;
            return;
        }
        // A light push creeps, a full push runs: 4 to 30 seconds of song per
        // second of hold.
        const float rate = 4.0f + 26.0f * (std::fabs(x) - kScrubEdge) / (1.0f - kScrubEdge);
        scrub_left_ += (x > 0.0f ? rate : -rate) * dt;
        if (std::fabs(scrub_left_) < kScrubStep)
            return;
        const int jump = static_cast<int>(scrub_left_ / kScrubStep);
        scrub_left_ -= static_cast<float>(jump) * kScrubStep;
        fnos::Player &player = fn_.player();
        const int length = std::max(player.duration(), fn_.current()->duration);
        const int goal = player.position() + jump * static_cast<int>(kScrubStep);
        const int target = std::clamp(goal, 0, length > 1 ? length - 1 : 0);
        // At either end there is nowhere left to go. Staying quiet there beats
        // a refusal on every step of a held stick.
        if (target == player.position() || length <= 1)
        {
            scrub_left_ = 0.0f;
            return;
        }
        fn_.seek_to(target);
        feedback.play(audio::Cue::slider, 1.0f, x > 0.0f ? 0.2f : -0.2f);
    }

    // The left stick is the room's volume. No button has to find the bar's knob
    // to change how loud it is, and the number reads out where the bar is not.
    void on_volume_stick(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        const float x = input.stick_x;
        if (std::fabs(x) < kScrubEdge)
        {
            volume_step_ = 0.0f;
            return;
        }
        const float rate = 0.25f + 0.85f * (std::fabs(x) - kScrubEdge) / (1.0f - kScrubEdge);
        const float goal = std::clamp(fn_.volume() + (x > 0.0f ? rate : -rate) * dt, 0.0f, 1.0f);
        if (goal == fn_.volume())
        {
            volume_step_ = 0.0f;
            return;
        }
        // One cue per twentieth of the way, the beat the buttons make: a held
        // stick ticks up the scale instead of crying a slider every frame.
        volume_step_ += std::fabs(goal - fn_.volume());
        if (volume_step_ >= 0.05f)
        {
            volume_step_ = 0.0f;
            feedback.play(audio::Cue::slider, 1.0f, x > 0.0f ? 0.15f : -0.15f);
        }
        fn_.set_volume(goal);
        volume_hud_ = 1.6f;
        char text[24];
        std::snprintf(text, sizeof(text), "音量 %d%%", static_cast<int>(goal * 100.0f + 0.5f));
        volume_read_ = text;
    }

    void open_chip(Library &lib, app::Feedback &feedback)
    {
        const Shelf shelf = kRail[static_cast<std::size_t>(chip_)].shelf;
        now_ = false;
        if (shelf == Shelf::search)
        {
            open_keyboard(Field::query);
            return;
        }
        lib.show(shelf);
        zone_ = Zone::content;
        feedback.play(audio::Cue::open);
        rebuild_ = true;
    }

    void on_content(const InputFrame &input, Library &lib, app::Feedback &feedback)
    {
        // On the grid the left edge of the first column is what hands the focus
        // to the rail (navigate_grid does that); a row list has no columns, so
        // any left press leaves.
        if (input.nav == Direction::left && !show_cards(lib))
        {
            zone_ = Zone::chips;
            feedback.play(audio::Cue::focus, 1.0f, -0.6f, 0.8f);
            return;
        }
        if (lib.shelf() == Shelf::setup)
        {
            if (fields_.handle(input, feedback) != ui::Event::activated)
                return;
            const int tag = fields_.item(fields_.focus()).tag;
            if (tag >= 1 && tag <= 3)
                open_keyboard(static_cast<Field>(tag - 1));
            else if (tag == static_cast<int>(Field::login) + 1)
                sign_in(feedback, lib);
            else if (tag == static_cast<int>(Field::sign_out) + 1)
            {
                lib.sign_out();
                fn_.art().clear();
                fn_.stop();
                password_.clear(); // leaving drops the typed password with the token
                feedback.play(audio::Cue::modal_close);
                rebuild_ = true;
            }
            return;
        }
        if (show_cards(lib))
        {
            navigate_grid(input, feedback);
            if (zone_ != Zone::content || input.nav != Direction::none)
                return;
            if (input.is_pressed(Action::back) && grid_focus_ >= 5)
            {
                // Back has one step to take here: from deep in the grid to its top.
                grid_focus_ %= 5;
                feedback.play(audio::Cue::back);
                return;
            }
            if (!input.is_pressed(Action::confirm))
                return;
            if (grid_.empty())
            {
                refuse_grid(feedback, false, 0.0f, 1.0f);
                return;
            }
            if (track_shelf(lib.shelf()))
            {
                start_playback(lib, grid_focus_, feedback);
                return;
            }
            open_collection(lib, grid_focus_, feedback);
            return;
        }
        // The same bottom edge on a row list: ▼ on its last song raises the bar.
        if (input.nav == Direction::down && !input.nav_repeat && fn_.current() != nullptr &&
            tracks_.focus() + 1 >= static_cast<int>(tracks_.items().size()))
        {
            zone_ = Zone::transport;
            feedback.play(audio::Cue::focus, 1.0f, 0.0f, 0.8f);
            return;
        }
        const ui::Event event = tracks_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            start_playback(lib, tracks_.focus(), feedback);
            return;
        }
        if (event == ui::Event::cancelled)
            go_back(feedback, lib);
    }

    // A song picked from the grid or a row starts playing and leaves the page
    // where it was: the plate along the top already names what is playing, so
    // the record does not have to turn the page under someone who was listing
    // songs. □ is the way to Now Playing, and ▼ the way to the bar.
    void start_playback(Library &lib, int index, app::Feedback &feedback)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= lib.tracks().size())
            return;
        // Which row was chosen, and what the shelf said about it: the tail of a
        // long list failing while the head works is otherwise a guess.
        const fnos::Track &picked = lib.tracks()[static_cast<std::size_t>(index)];
        sys::log("[HUI] row %d/%d guid=%d chars dur=%d", index,
                 static_cast<int>(lib.tracks().size()), static_cast<int>(picked.guid.size()),
                 picked.duration);
        fn_.play_queue(lib.tracks(), index);
        feedback.rumble(0.35f, 0.05f);
        rebuild_ = true;
    }

    void on_transport(const InputFrame &input, app::Feedback &feedback)
    {
        // Over the storefront the bar is a guest: ▼ again puts it back down and
        // leaves the focus on the last row it came from. On Now Playing the card
        // is part of the page, so ▼ moves focus as any other row would.
        if (!now_ && input.nav == Direction::down && !input.nav_repeat)
        {
            zone_ = Zone::content;
            feedback.play(audio::Cue::back, 1.0f, 0.0f, 0.9f);
            return;
        }
        const ui::Event event = controls_.handle(input, feedback);
        if (controls_.exit() != Direction::none)
        {
            now_ = false;
            zone_ = Zone::content;
            rebuild_ = true;
            return;
        }
        if (event == ui::Event::cancelled)
        {
            zone_ = now_ ? Zone::transport : Zone::content;
            return;
        }
        switch (controls_.command())
        {
        case ui::MediaCommand::play:
        case ui::MediaCommand::pause:
            fn_.toggle_play();
            break;
        case ui::MediaCommand::previous:
            fn_.previous();
            rebuild_ = true;
            break;
        case ui::MediaCommand::next:
            fn_.next();
            rebuild_ = true;
            break;
        case ui::MediaCommand::rewind:
        case ui::MediaCommand::forward:
        case ui::MediaCommand::seek:
            fn_.seek_to(static_cast<int>(controls_.seek_position()));
            break;
        case ui::MediaCommand::shuffle:
            fn_.toggle_shuffle();
            break;
        case ui::MediaCommand::repeat:
            fn_.cycle_repeat();
            break;
        case ui::MediaCommand::volume:
            fn_.set_volume(controls_.volume());
            break;
        case ui::MediaCommand::mute:
            fn_.set_volume(controls_.muted() ? 0.0f : controls_.volume());
            break;
        default:
            break;
        }
    }

    void on_keyboard(const InputFrame &input, Library &lib, app::Feedback &feedback)
    {
        const ui::Event event = keys_.handle(input, feedback);
        if (event == ui::Event::cancelled)
        {
            editing_ = Field::none;
            zone_ = Zone::content;
            rebuild_ = true;
            return;
        }
        if (event != ui::Event::activated)
            return;
        const Field done = editing_;
        editing_ = Field::none;
        if (done == Field::server)
            server_ = with_scheme(server_);
        zone_ = Zone::content;
        rebuild_ = true;
        if (done == Field::query)
        {
            lib.search(query_draft_);
            feedback.play(audio::Cue::open);
            return;
        }
        // 完成 only stores what was typed. Signing in is the 登录 row's job, so
        // closing the password field must not spend it: a failed login has to be
        // retryable without typing the whole thing again.
        feedback.play(audio::Cue::saved);
    }

    void open_keyboard(Field field)
    {
        editing_ = field;
        zone_ = Zone::keyboard;
        keys_.set_layout(0);
        // An address always carries its scheme: picking out "http://" letter by
        // letter on the grid is the part that makes people give up.
        if (field == Field::server && server_.empty())
            server_ = "http://";
        // Start on the first letter key: the last edit left the focus on 完成,
        // and every press there would close the keyboard without typing.
        keys_.set_focus(0, 0);
        keys_.set_length(static_cast<int>(edit_buffer().size()));
        keys_.enter();
        rebuild_ = true;
    }

    std::string &edit_buffer()
    {
        switch (editing_)
        {
        case Field::server:
            return server_;
        case Field::username:
            return username_;
        case Field::password:
            return password_;
        case Field::query:
            return query_draft_;
        default:
            return scratch_;
        }
    }
    const std::string &edit_buffer() const
    {
        switch (editing_)
        {
        case Field::server:
            return server_;
        case Field::username:
            return username_;
        case Field::password:
            return password_;
        case Field::query:
            return query_draft_;
        default:
            return scratch_;
        }
    }

    static std::string field_name(Field field)
    {
        switch (field)
        {
        case Field::server:
            return "服务器地址";
        case Field::username:
            return "账号";
        case Field::password:
            return "密码";
        case Field::query:
            return "搜索歌曲";
        default:
            return "输入";
        }
    }

    // "192.168.1.20:5666" and "http://192.168.1.20:5666" ask the API for the
    // same thing, and the short form is what people remember, so the field
    // finishes it off. A prefix on its own is not an address yet: it goes back
    // to empty rather than leaving 登录 to try and connect to "http://".
    static std::string with_scheme(std::string value)
    {
        const std::size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos)
            return std::string();
        const std::size_t last = value.find_last_not_of(" \t/");
        value = value.substr(first, last - first + 1);
        if (value == "http:" || value == "http://" || value == "https:" || value == "https://")
            return std::string();
        if (value.compare(0, 7, "http://") == 0 || value.compare(0, 8, "https://") == 0)
            return value;
        return "http://" + value;
    }

    // The first setup field 登录 still needs, in the order the player reads
    // them. Empty means the page is ready to sign in.
    std::string missing_field() const
    {
        if (server_.empty())
            return "服务器";
        if (username_.empty())
            return "账号";
        if (password_.empty())
            return "密码";
        return std::string();
    }

    void sign_in(app::Feedback &feedback, Library &lib)
    {
        const std::string missing = missing_field();
        if (!missing.empty())
        {
            // A press that cannot be honoured still has to answer. A quiet
            // refusal reads exactly like a broken button.
            notice_ = "登录前要先填好" + missing;
            notice_age_ = 0.0f;
            feedback.play(audio::Cue::error);
            feedback.rumble(0.5f, 0.06f);
            rebuild_ = true;
            return;
        }
        notice_.clear();
        lib.sign_in(server_, username_, password_);
        feedback.play(audio::Cue::launch);
        feedback.rumble(0.4f, 0.05f);
        rebuild_ = true;
    }

    void open_collection(Library &lib, int index, app::Feedback &feedback)
    {
        std::string guid;
        std::string name;
        switch (lib.shelf())
        {
        case Shelf::playlists:
            if (index >= 0 && static_cast<std::size_t>(index) < lib.playlists().size())
            {
                guid = lib.playlists()[static_cast<std::size_t>(index)].guid;
                name = lib.playlists()[static_cast<std::size_t>(index)].name;
            }
            break;
        case Shelf::albums:
            if (index >= 0 && static_cast<std::size_t>(index) < lib.albums().size())
            {
                guid = lib.albums()[static_cast<std::size_t>(index)].guid;
                name = lib.albums()[static_cast<std::size_t>(index)].name;
            }
            break;
        case Shelf::artists:
            if (index >= 0 && static_cast<std::size_t>(index) < lib.artists().size())
            {
                guid = lib.artists()[static_cast<std::size_t>(index)].guid;
                name = lib.artists()[static_cast<std::size_t>(index)].name;
            }
            break;
        default:
            if (index >= 0 && static_cast<std::size_t>(index) < lib.genres().size())
            {
                guid = lib.genres()[static_cast<std::size_t>(index)].guid;
                name = lib.genres()[static_cast<std::size_t>(index)].name;
            }
            break;
        }
        if (guid.empty())
        {
            feedback.play(audio::Cue::error);
            return;
        }
        lib.open(lib.shelf(), guid, name);
        feedback.play(audio::Cue::open);
        rebuild_ = true;
    }

    // One level per press, with the storefront as the floor: 歌词 → 播放页 →
    // 播放条 → 内容 → 栏目. On the chips row nothing is left to go back to, so
    // the press is heard as a refusal instead of teleporting the focus.
    void go_back(app::Feedback &feedback, Library &lib)
    {
        if (lyric_open_)
        {
            lyric_open_ = false;
            feedback.play(audio::Cue::back, 1.0f, 0.0f, 0.9f);
            return;
        }
        if (now_)
        {
            now_ = false;
            rebuild_ = true;
            feedback.play(audio::Cue::back);
            return;
        }
        if (zone_ == Zone::transport)
        {
            zone_ = Zone::content;
            feedback.play(audio::Cue::back, 1.0f, 0.0f, 0.9f);
            return;
        }
        if (!lib.collection().empty())
        {
            lib.show(lib.shelf()); // out of the songs, back to the tiles
            rebuild_ = true;
            feedback.play(audio::Cue::back);
            return;
        }
        if (zone_ == Zone::chips)
        {
            feedback.play(audio::Cue::error);
            return;
        }
        zone_ = Zone::chips;
        feedback.play(audio::Cue::back);
    }

    void toggle_favorite(Library &lib, int index, app::Feedback &feedback)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= lib.tracks().size())
            return;
        const fnos::Track &track = lib.tracks()[static_cast<std::size_t>(index)];
        lib.set_favorite(track.guid, !track.favorite);
        feedback.play(track.favorite ? audio::Cue::favorite_off : audio::Cue::favorite_on);
        feedback.rumble(0.3f, 0.06f);
        rebuild_ = true;
    }

    // Which song the content page points at, whether it is drawn as a cover
    // tile or as a row.
    int focused_track(const Library &lib) const
    {
        return show_cards(lib) ? grid_focus_ : tracks_.focus();
    }

    void feed_transport(Library &lib, fnos::Player &player)
    {
        const fnos::Track *track = fn_.current();
        controls_.title =
            track != nullptr ? track->title : (lib.signed_in() ? "未选择歌曲" : "请先登录");
        controls_.artist = track != nullptr ? track->artist
                                            : (lyric_open_ ? "歌词已开" : shelf_title(lib.shelf()));
        // A container that will not say how long it is still has a length on the
        // NAS, and a bar with no total cannot be scrubbed to a second.
        const int from_server = track != nullptr ? track->duration : 0;
        controls_.set_duration(static_cast<float>(player.duration() > 0 ? player.duration()
                                                                        : from_server));
        // A new song is a new bar, whatever has focus: the scrubber used to hold
        // its position when the focus sat on it, so L1/R1 changed the record and
        // left the thumb at the far end of the last one.
        const int index = fn_.index();
        if (index != played_index_)
        {
            played_index_ = index;
            controls_.set_position(0.0f, true);
            const int container = player.container_length();
            if (container > 0 && from_server > 0 && std::abs(container - from_server) > 5)
            {
                // The server's number and the container's disagree by more than a
                // breath: either one of them is not in seconds, or the bytes
                // behind this address are a longer record than its one song.
                sys::log("[HUI] duration server=%d container=%d", from_server, container);
            }
        }
        else if (controls_.focus() != ui::MediaControl::scrubber)
        {
            controls_.set_position(static_cast<float>(player.position()));
        }
        controls_.set_playing(player.state() == fnos::PlayerState::playing);
        controls_.set_shuffle(fn_.shuffle());
        controls_.set_repeat(static_cast<ui::MediaRepeat>(static_cast<int>(fn_.repeat())));
        controls_.set_volume(fn_.volume());

        transport_gl_ = 0;
        transport_uv_ = gfx::kFullUv;
        if (track != nullptr && !track->cover_id.empty())
        {
            transport_gl_ = fn_.art().texture(track->cover_id);
            int width = 0;
            int height = 0;
            if (transport_gl_ != 0 && fn_.art().size(track->cover_id, &width, &height))
                transport_uv_ = square_uv(width, height);
        }
    }

    // The pictures are taken while the app is signed in: the snapshot host
    // answers the API out of a demo library (src/fnos/http.cpp) before the
    // first frame, so every shelf has rows to lay out.
    //
    // A step's picture shows the page its predecessor's press led to, so the
    // press that opens a state belongs to the step above the one named for it.
    // Each press is one frame: L2/R2 wrap around the eight chips, so a step
    // backwards from 歌曲 lands on 设置.
    //
    // The waits are long enough for the page's own entrance to finish: a capsule
    // fades in over the last second of the stagger, and a picture taken before
    // it shows two cards of eight.
    static constexpr std::array<app::TourStep, 10> kTour = {
        app::TourStep{0.4f, 0u, Direction::none, "fnmusic-boot", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{1.8f, action_bit(Action::confirm), Direction::none, "fnmusic-home", 0.0f,
                      0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{1.0f, action_bit(Action::jump_next), Direction::none, "fnmusic-grid", 0.0f,
                      0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.8f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.9f, 0u, Direction::down, "fnmusic-list", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.7f, 0u, Direction::left, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.7f, action_bit(Action::west), Direction::none, "fnmusic-rail", 0.0f, 0.0f,
                      0u, 0.0f, 0.0f},
        app::TourStep{0.9f, action_bit(Action::north), Direction::none, "fnmusic-playing", 0.0f,
                      0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{1.0f, 0u, Direction::none, "fnmusic-lyric", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
    };

    Service &fn_;
    // param.json's contentVersion, through the shell: the same number the build
    // and the release are named with.
    const std::string &version_;
    ui::Theme theme_;
    ui::Fonts glyphs_;
    ui::ListView tracks_;
    ui::ListView fields_;
    ui::MediaControls controls_;
    ui::Keyboard keys_;

    // The Storefront page, drawn here rather than by a component: its cards,
    // the sheet's own scroll, and the one ring that glides over the page.
    std::vector<ui::CardItem> grid_;
    std::vector<std::string> grid_titles_; // titles fitted to a card, once
    std::vector<std::string> grid_notes_;  // ... and the line under them
    int grid_focus_ = 0;                   // index into grid_
    tween::Spring grid_scroll_;
    ui::SpringRect ring_;
    tween::Spring ring_radius_;
    tween::Spring plate_;
    ui::SpringColor glow_;
    std::vector<tween::Spring> lift_; // per card: 1 while its capsule is focused
    ui::SpringRect chip_pill_;
    ui::Pulse nudge_;
    float nudge_x_ = 0.0f;
    float nudge_y_ = 0.0f;
    // Seconds the right stick has banked but not spent on a seek yet.
    float scrub_left_ = 0.0f;
    // The queue position the transport bar is showing, so a new song can reset it.
    int played_index_ = -1;
    // Seconds of stick the volume has banked but not spent on a cue yet, and
    // how long the readout of the last turn has left to live.
    float volume_step_ = 0.0f;
    float volume_hud_ = 0.0f;
    std::string volume_read_;
    ui::Pulse chip_nudge_;
    float chip_nudge_x_ = 0.0f;
    float page_age_ = 0.0f; // seconds this page has been up: its entrance wave

    std::string notice_;
    float notice_age_ = 0.0f;

    Zone zone_ = Zone::boot;
    bool now_ = false;
    bool lyric_open_ = false;
    Field editing_ = Field::none;

    std::string server_;
    std::string username_;
    std::string password_;
    std::string query_draft_;
    std::string scratch_;

    Rect content_{0.0f, 0.0f, 100.0f, 100.0f};
    Rect transport_{0.0f, 0.0f, 100.0f, 100.0f};
    float header_left_ = 0.0f;
    float clock_ = 0.0f;
    tween::Spring enter_{0.0f, 0.0f, 1.0f};

    bool rebuild_ = true;
    bool was_signed_in_ = false;
    bool was_failed_ = false;
    Shelf last_shelf_ = Shelf::songs;
    std::string last_collection_;
    std::size_t signature_ = 0;
    std::vector<std::string> cover_ids_;
    std::vector<std::uint32_t> cover_gl_;
    std::vector<std::uint32_t> painted_;
    std::string empty_key_;
    std::uint32_t transport_gl_ = 0;
    Rect transport_uv_ = gfx::kFullUv;

    // The chips row, the splash clock and the spectrum are drawn by this design
    // itself, so their state lives here.
    int chip_ = 0;
    float boot_age_ = 0.0f;
    float beat_ = 0.0f;
    std::array<float, kBars> levels_{};
    std::array<float, kBars> peaks_{};
    std::uint32_t banner_gl_ = 0;
    Rect banner_uv_ = gfx::kFullUv;
    std::string banner_key_; // the cover the plate shows, so a late one still lands on it
    Color banner_tone_ = kAccent;
    std::string banner_kicker_;
    std::string banner_title_;
    std::string banner_note_;
    // The sheet's big mono figure: how much this shelf holds.
    std::string banner_stat_;
    // The word after the figure, which the mono face cannot draw, and whether
    // the plate speaks for the song in the room rather than the shelf.
    std::string banner_unit_;
    bool banner_song_ = false;
};

std::unique_ptr<app::Concept> make_fnmusic(app::Context &context)
{
    return std::make_unique<Fnmusic>(context);
}

} // namespace hui::concepts
