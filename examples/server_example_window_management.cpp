/*
 * Copyright © 2014-2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored By: Alan Griffiths <alan@octopull.co.uk>
 */

#define MIR_INCLUDE_DEPRECATED_EVENT_HEADER 

#include "server_example_window_management.h"
#include "server_example_basic_window_manager.h"

#include "mir/abnormal_exit.h"
#include "mir/server.h"
#include "mir/geometry/rectangles.h"
#include "mir/geometry/displacement.h"
#include "mir/input/composite_event_filter.h"
#include "mir/options/option.h"
#include "mir/scene/surface.h"
#include "mir/shell/display_layout.h"

#include <linux/input.h>

#include <map>
#include <vector>
#include <mutex>

namespace mc = mir::compositor;
namespace me = mir::examples;
namespace mf = mir::frontend;
namespace mg = mir::graphics;
namespace mi = mir::input;
namespace ms = mir::scene;
namespace msh = mir::shell;
using namespace mir::geometry;

///\example server_example_window_management.cpp
/// Demonstrate simple window management strategies

char const* const me::wm_option = "window-manager";
char const* const me::wm_description = "window management strategy [{tiling|fullscreen}]";

namespace
{
char const* const wm_tiling = "tiling";
char const* const wm_fullscreen = "fullscreen";

struct SessionInfo
{
    Rectangle tile;
    std::vector<std::weak_ptr<ms::Surface>> surfaces;
};

struct SurfaceInfo
{
    SurfaceInfo() = default;
    SurfaceInfo(
        std::shared_ptr<ms::Session> const& session,
        std::shared_ptr<ms::Surface> const& surface) :
        session{session},
        state{mir_surface_state_restored},
        restore_rect{surface->top_left(), surface->size()}
    {}

    std::weak_ptr<ms::Session> session;
    MirSurfaceState state;
    Rectangle restore_rect;
};

// Very simple - make every surface fullscreen
class FullscreenWindowManagerPolicy
{
public:
    using Tools = me::BasicWindowManagerTools<SessionInfo, SurfaceInfo>;
    using SessionInfoMap = typename me::SessionTo<SessionInfo>::type;

    FullscreenWindowManagerPolicy(Tools* const /*tools*/, std::shared_ptr<msh::DisplayLayout> const& display_layout) :
        display_layout{display_layout} {}

    void handle_click(const Point& /*cursor*/) {}

    void handle_session_info_updated(SessionInfoMap& /*session_info*/, Rectangles const& /*displays*/) {}

    void handle_displays_updated(SessionInfoMap& /*session_info*/, Rectangles const& /*displays*/) {}

    void handle_resize(Point const& /*cursor*/, Point const& /*old_cursor*/) {}

    auto handle_place_new_surface(
        std::shared_ptr<ms::Session> const& /*session*/,
        ms::SurfaceCreationParameters const& request_parameters)
    -> ms::SurfaceCreationParameters
    {
        auto placed_parameters = request_parameters;

        Rectangle rect{request_parameters.top_left, request_parameters.size};
        display_layout->size_to_output(rect);
        placed_parameters.size = rect.size;

        return placed_parameters;
    }

    void handle_new_surface(std::shared_ptr<ms::Session> const& /*session*/, std::shared_ptr<ms::Surface> const& /*surface*/)
    {
    }

    int handle_set_state(std::shared_ptr<ms::Surface> const& /*surface*/, MirSurfaceState value)
        { return value; }

    void handle_drag(Point const& /*cursor*/, Point const& /*old_cursor*/) {}

private:
    std::shared_ptr<msh::DisplayLayout> const display_layout;
};


// simple tiling algorithm:
//  o Switch apps: tap or click on the corresponding tile
//  o Move window: Alt-leftmousebutton drag
//  o Resize window: Alt-middle_button drag
//  o Maximize/restore current window (to tile size): Alt-F11
//  o Maximize/restore current window (to tile height): Shift-F11
//  o Maximize/restore current window (to tile width): Ctrl-F11
//  o client requests to maximize, vertically maximize & restore
class TilingWindowManagerPolicy
{
public:
    using Tools = me::BasicWindowManagerTools<SessionInfo, SurfaceInfo>;
    using SessionInfoMap = typename me::SessionTo<SessionInfo>::type;

    TilingWindowManagerPolicy(Tools* const tools) :
        tools{tools} {}

    void handle_click(const Point& cursor)
    {
        if (const auto session = session_under(cursor))
            tools->set_focus_to(session);
    }

    void handle_session_info_updated(SessionInfoMap& session_info, Rectangles const& displays)
    {
        update_tiles(session_info, displays);
    }

    void handle_displays_updated(SessionInfoMap& session_info, Rectangles const& displays)
    {
        update_tiles(session_info, displays);
    }

    void handle_resize(Point const& cursor, Point const& old_cursor)
    {
        if (auto const session = session_under(cursor))
        {
            if (session == session_under(old_cursor))
            {
                auto const& info = tools->info_for(session);

                if (resize(tools->working_surface(), cursor, old_cursor, info.tile))
                {
                    // Still dragging the same old_surface
                }
                else if (resize(session->default_surface(), cursor, old_cursor, info.tile))
                {
                    tools->set_working_surface_to(session->default_surface());
                }
                else
                {
                    for (auto const& ps : info.surfaces)
                    {
                        auto const new_surface = ps.lock();

                        if (resize(new_surface, cursor, old_cursor, info.tile))
                        {
                            tools->set_working_surface_to(new_surface);
                            break;
                        }
                    }
                }
            }
        }
    }

    auto handle_place_new_surface(
        std::shared_ptr<ms::Session> const& session,
        ms::SurfaceCreationParameters const& request_parameters)
    -> ms::SurfaceCreationParameters
    {
        auto parameters = request_parameters;

        Rectangle const& tile = tools->info_for(session).tile;
        parameters.top_left = parameters.top_left + (tile.top_left - Point{0, 0});

        clip_to_tile(parameters, tile);
        return parameters;
    }

    void handle_new_surface(std::shared_ptr<ms::Session> const& /*session*/, std::shared_ptr<ms::Surface> const& /*surface*/)
    {
    }

    int handle_set_state(std::shared_ptr<ms::Surface> const& surface, MirSurfaceState value)
    {
        auto& info = tools->info_for(surface);

        switch (value)
        {
        case mir_surface_state_restored:
        case mir_surface_state_maximized:
        case mir_surface_state_vertmaximized:
        case mir_surface_state_horizmaximized:
            break;

        default:
            return info.state;
        }

        if (info.state == mir_surface_state_restored)
        {
            info.restore_rect = {surface->top_left(), surface->size()};
        }

        if (info.state == value)
        {
            return info.state;
        }

        auto const& tile = tools->info_for(info.session).tile;

        switch (value)
        {
        case mir_surface_state_restored:
            surface->move_to(info.restore_rect.top_left);
            surface->resize(info.restore_rect.size);
            break;

        case mir_surface_state_maximized:
            surface->move_to(tile.top_left);
            surface->resize(tile.size);
            break;

        case mir_surface_state_horizmaximized:
            surface->move_to({tile.top_left.x, info.restore_rect.top_left.y});
            surface->resize({tile.size.width, info.restore_rect.size.height});
            break;

        case mir_surface_state_vertmaximized:
            surface->move_to({info.restore_rect.top_left.x, tile.top_left.y});
            surface->resize({info.restore_rect.size.width, tile.size.height});
            break;

        default:
            break;
        }

        return info.state = value;
    }

    void handle_drag(Point const& cursor, Point const& old_cursor)
    {
        if (const auto session = session_under(cursor))
        {
            if (session == session_under(old_cursor))
            {
                const auto& info = tools->info_for(session);
                if (drag(tools->working_surface(), cursor, old_cursor, info.tile))
                {
                    // Still dragging the same old_surface
                }
                else if (drag(session->default_surface(), cursor, old_cursor, info.tile))
                {
                    tools->set_working_surface_to(session->default_surface());
                }
                else
                {
                    for (const auto& ps : info.surfaces)
                    {
                        const auto new_surface = ps.lock();
                        if (drag(new_surface, cursor, old_cursor, info.tile))
                        {
                            tools->set_working_surface_to(new_surface);
                            break;
                        }
                    }
                }
            }
        }
    }

private:
    std::shared_ptr<ms::Session> session_under(Point position)
    {
        return tools->find_session([&](SessionInfo const& info) { return info.tile.contains(position);});
    }

    void update_tiles(
        SessionInfoMap& session_info,
        Rectangles const& displays)
    {
        if (session_info.size() < 1 || displays.size() < 1) return;

        auto const sessions = session_info.size();

        auto const bounding_rect = displays.bounding_rectangle();

        auto const total_width  = bounding_rect.size.width.as_int();
        auto const total_height = bounding_rect.size.height.as_int();

        auto index = 0;

        for (auto& info : session_info)
        {
            auto const x = (total_width*index)/sessions;
            ++index;
            auto const dx = (total_width*index)/sessions - x;

            auto const old_tile = info.second.tile;
            Rectangle const new_tile{{x, 0}, {dx, total_height}};

            update_surfaces(info.first, old_tile, new_tile);

            info.second.tile = new_tile;
        }
    }

    void update_surfaces(std::weak_ptr<ms::Session> const& session, Rectangle const& old_tile, Rectangle const& new_tile)
    {
        auto displacement = new_tile.top_left - old_tile.top_left;
        auto& info = tools->info_for(session);

        for (auto const& ps : info.surfaces)
        {
            if (auto const surface = ps.lock())
            {
                auto const old_pos = surface->top_left();
                surface->move_to(old_pos + displacement);

                fit_to_new_tile(*surface, old_tile, new_tile);
            }
        }
    }

    static void clip_to_tile(ms::SurfaceCreationParameters& parameters, Rectangle const& tile)
    {
        auto const displacement = parameters.top_left - tile.top_left;

        auto width = std::min(tile.size.width.as_int()-displacement.dx.as_int(), parameters.size.width.as_int());
        auto height = std::min(tile.size.height.as_int()-displacement.dy.as_int(), parameters.size.height.as_int());

        parameters.size = Size{width, height};
    }

    static void fit_to_new_tile(ms::Surface& surface, Rectangle const& old_tile, Rectangle const& new_tile)
    {
        auto const displacement = surface.top_left() - new_tile.top_left;

        // For now just scale if was filling width/height of tile
        auto const old_size = surface.size();
        auto const scaled_width = old_size.width == old_tile.size.width ? new_tile.size.width : old_size.width;
        auto const scaled_height = old_size.height == old_tile.size.height ? new_tile.size.height : old_size.height;

        auto width = std::min(new_tile.size.width.as_int()-displacement.dx.as_int(), scaled_width.as_int());
        auto height = std::min(new_tile.size.height.as_int()-displacement.dy.as_int(), scaled_height.as_int());

        surface.resize({width, height});
    }

    static bool drag(std::shared_ptr<ms::Surface> surface, Point to, Point from, Rectangle bounds)
    {
        if (surface && surface->input_area_contains(from))
        {
            auto const top_left = surface->top_left();
            auto const surface_size = surface->size();
            auto const bottom_right = top_left + as_displacement(surface_size);

            auto movement = to - from;

            if (movement.dx < DeltaX{0})
                movement.dx = std::max(movement.dx, (bounds.top_left - top_left).dx);

            if (movement.dy < DeltaY{0})
                movement.dy = std::max(movement.dy, (bounds.top_left - top_left).dy);

            if (movement.dx > DeltaX{0})
                movement.dx = std::min(movement.dx, (bounds.bottom_right() - bottom_right).dx);

            if (movement.dy > DeltaY{0})
                movement.dy = std::min(movement.dy, (bounds.bottom_right() - bottom_right).dy);

            auto new_pos = surface->top_left() + movement;

            surface->move_to(new_pos);
            return true;
        }

        return false;
    }

    static bool resize(std::shared_ptr<ms::Surface> surface, Point cursor, Point old_cursor, Rectangle bounds)
    {
        if (surface && surface->input_area_contains(old_cursor))
        {
            auto const top_left = surface->top_left();

            auto const old_displacement = old_cursor - top_left;
            auto const new_displacement = cursor - top_left;

            auto const scale_x = new_displacement.dx.as_float()/std::max(1.0f, old_displacement.dx.as_float());
            auto const scale_y = new_displacement.dy.as_float()/std::max(1.0f, old_displacement.dy.as_float());

            if (scale_x <= 0.0f || scale_y <= 0.0f) return false;

            auto const old_size = surface->size();
            Size new_size{scale_x*old_size.width, scale_y*old_size.height};

            auto const size_limits = as_size(bounds.bottom_right() - top_left);

            if (new_size.width > size_limits.width)
                new_size.width = size_limits.width;

            if (new_size.height > size_limits.height)
                new_size.height = size_limits.height;

            surface->resize(new_size);

            return true;
        }

        return false;
    }

    Tools* const tools;
};
}

using TilingWindowManager = me::BasicWindowManager<TilingWindowManagerPolicy, SessionInfo, SurfaceInfo>;
using FullscreenWindowManager = me::BasicWindowManager<FullscreenWindowManagerPolicy, SessionInfo, SurfaceInfo>;

class me::EventTracker : public mi::EventFilter
{
public:
    explicit EventTracker(std::shared_ptr<me::WindowManager> const& window_manager) :
        window_manager{window_manager} {}

    bool handle(MirEvent const& event) override
    {
        switch (event.type)
        {
        case mir_event_type_key:
            return handle_key_event(event.key);

        case mir_event_type_motion:
            return handle_motion_event(event.motion);

        default:
            return false;
        }
    }

private:
    bool handle_key_event(MirKeyEvent const& event)
    {
        static const int modifier_mask =
            mir_key_modifier_alt |
            mir_key_modifier_shift |
            mir_key_modifier_sym |
            mir_key_modifier_ctrl |
            mir_key_modifier_meta;

        if (event.action == mir_key_action_down &&
            event.scan_code == KEY_F11)
        {
            if (auto const wm = window_manager.lock())
            switch (event.modifiers & modifier_mask)
            {
            case mir_key_modifier_alt:
                wm->toggle(mir_surface_state_maximized);
                return true;

            case mir_key_modifier_shift:
                wm->toggle(mir_surface_state_vertmaximized);
                return true;

            case mir_key_modifier_ctrl:
                wm->toggle(mir_surface_state_horizmaximized);
                return true;

            default:
                break;
            }
        }

        return false;
    }

    bool handle_motion_event(MirMotionEvent const& event)
    {
        if (event.action == mir_motion_action_down ||
            event.action == mir_motion_action_pointer_down)
        {
            if (auto const wm = window_manager.lock())
            {
                wm->click(average_pointer(event.pointer_count, event.pointer_coordinates));
                return false;
            }
        }
        else if (event.action == mir_motion_action_move &&
                 event.modifiers & mir_key_modifier_alt)
        {
            if (auto const wm = window_manager.lock())
            {
                switch (event.button_state)
                {
                case mir_motion_button_primary:
                    wm->drag(average_pointer(event.pointer_count, event.pointer_coordinates));
                    return true;

                case mir_motion_button_tertiary:
                    wm->resize(average_pointer(event.pointer_count, event.pointer_coordinates));
                    return true;

                default:
                    ;// ignore
                }
            }
        }

        return false;
    }

    static Point average_pointer(size_t pointer_count, MirMotionPointer const* pointer_coordinates)
    {
        long total_x = 0;
        long total_y = 0;

        for (auto p = pointer_coordinates; p != pointer_coordinates + pointer_count; ++p)
        {
            total_x += p->x;
            total_y += p->y;
        }

        return Point{total_x/pointer_count, total_y/pointer_count};
    }

    std::weak_ptr<me::WindowManager> const window_manager;
};

auto me::WindowManagmentFactory::window_manager() -> std::shared_ptr<me::WindowManager>
{
    auto tmp = wm.lock();

    if (!tmp)
    {
        auto const options = server.get_options();
        auto const selection = options->get<std::string>(wm_option);

        if (selection == wm_tiling)
        {
            tmp = std::make_shared<TilingWindowManager>(
                server.the_input_targeter(),
                server.the_surface_coordinator(),
                server.the_session_coordinator(),
                server.the_prompt_session_manager());
        }
        else if (selection == wm_fullscreen)
            tmp = std::make_shared<FullscreenWindowManager>(
                server.the_input_targeter(),
                server.the_surface_coordinator(),
                server.the_session_coordinator(),
                server.the_prompt_session_manager(),
                server.the_shell_display_layout());
        else
            throw mir::AbnormalExit("Unknown window manager: " + selection);

        et = std::make_shared<EventTracker>(tmp);
        server.the_composite_event_filter()->prepend(et);
        wm = tmp;
    }

    return tmp;
}
