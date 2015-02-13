/*
 * Copyright © 2015 Canonical Ltd.
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

#ifndef MIR_EXAMPLE_CANONICAL_WINDOW_MANAGER_H_
#define MIR_EXAMPLE_CANONICAL_WINDOW_MANAGER_H_

#include "server_example_basic_window_manager.h"

///\example server_example_canonical_window_manager.h
/// Demonstrate implementing a standard tiling algorithm

namespace mir
{
namespace graphics { class Display; }
namespace compositor { class Compositor; }

namespace examples
{
struct CanonicalSessionInfo
{
    geometry::Rectangle tile;
    std::vector<std::weak_ptr<scene::Surface>> surfaces;
};

struct CanonicalSurfaceInfo
{
    CanonicalSurfaceInfo(
        std::shared_ptr<scene::Session> const& session,
        std::shared_ptr<scene::Surface> const& surface);

    MirSurfaceState state;
    geometry::Rectangle restore_rect;
    std::weak_ptr<scene::Session> session;
    std::weak_ptr<scene::Surface> parent;
    std::vector<std::weak_ptr<scene::Surface>> children;
};

// standard window management algorithm:
//  o Switch apps: tap or click on the corresponding tile
//  o Move window: Alt-leftmousebutton drag
//  o Resize window: Alt-middle_button drag
//  o Maximize/restore current window (to display size): Alt-F11
//  o Maximize/restore current window (to display height): Shift-F11
//  o Maximize/restore current window (to display width): Ctrl-F11
//  o client requests to maximize, vertically maximize & restore
class CanonicalWindowManagerPolicy
{
public:
    using Tools = BasicWindowManagerTools<CanonicalSessionInfo, CanonicalSurfaceInfo>;
    using CanonicalSessionInfoMap = typename SessionTo<CanonicalSessionInfo>::type;

    CanonicalWindowManagerPolicy(
        Tools* const tools,
        std::shared_ptr<graphics::Display> const& display,
        std::shared_ptr<compositor::Compositor> const& compositor);

    void click(geometry::Point cursor);

    void handle_session_info_updated(CanonicalSessionInfoMap& session_info, geometry::Rectangles const& displays);

    void handle_displays_updated(CanonicalSessionInfoMap& session_info, geometry::Rectangles const& displays);

    void resize(geometry::Point cursor);

    auto handle_place_new_surface(
        std::shared_ptr<scene::Session> const& session,
        scene::SurfaceCreationParameters const& request_parameters)
    -> scene::SurfaceCreationParameters;

    void handle_new_surface(std::shared_ptr<scene::Session> const& session, std::shared_ptr<scene::Surface> const& surface);

    void handle_delete_surface(std::shared_ptr<scene::Session> const& session, std::weak_ptr<scene::Surface> const& surface);

    int handle_set_state(std::shared_ptr<scene::Surface> const& surface, MirSurfaceState value);

    void drag(geometry::Point cursor);

    bool handle_key_event(MirKeyInputEvent const* event);

    bool handle_touch_event(MirTouchInputEvent const* event);

    bool handle_pointer_event(MirPointerInputEvent const* event);

private:
    static const int modifier_mask =
        mir_input_event_modifier_alt |
        mir_input_event_modifier_shift |
        mir_input_event_modifier_sym |
        mir_input_event_modifier_ctrl |
        mir_input_event_modifier_meta;

    void toggle(MirSurfaceState state);

    Tools* const tools;
    std::shared_ptr<graphics::Display> const display;
    std::shared_ptr<compositor::Compositor> const compositor;

    geometry::Rectangle display_area;
    geometry::Point old_cursor{};
    std::weak_ptr<scene::Surface> old_surface;
};
}
}

#endif /* MIR_EXAMPLE_CANONICAL_WINDOW_MANAGER_H_ */
