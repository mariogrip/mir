/*
 * Copyright (C) 2018 Marius Gripsgard <marius@ubports.com>
 * Copyright (C) 2019-2020 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3,
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
 */

#include "xwayland_server.h"
#include "xwayland_spawner.h"

#include "wayland_connector.h"

#include "mir/dispatch/multiplexing_dispatchable.h"
#include "mir/dispatch/readable_fd.h"
#include "mir/fd.h"
#include "mir/log.h"
#include "mir/terminate_with_current_exception.h"
#include <mir/thread_name.h>

#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <condition_variable>

namespace mf = mir::frontend;
namespace md = mir::dispatch;
using namespace std::chrono_literals;

mf::XWaylandServer::XWaylandServer(
    std::shared_ptr<WaylandConnector> const& wayland_connector,
    XWaylandSpawner const& spawner,
    std::string const& xwayland_path)
    : wayland_connector{wayland_connector},
      xwayland_path{xwayland_path}
{
    enum { server, client, size };
    int wl_client_fd[size], wm_fd[size];

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl_client_fd) < 0)
    {
        // "Shouldn't happen" but continuing is weird.
        mir::fatal_error("wl connection socketpair failed");
        return; // doesn't reach here
    }

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm_fd) < 0)
    {
        // "Shouldn't happen" but continuing is weird.
        mir::fatal_error("wm fd socketpair failed");
        return; // doesn't reach here
    }

    mir::log_info("Starting Xwayland");
    xwayland_pid = fork();

    switch (xwayland_pid)
    {
    case -1:
        mir::fatal_error("Failed to fork");
        return; // Keep compiler happy (doesn't reach here)

    case 0:
        close(wl_client_fd[server]);
        close(wm_fd[server]);
        execl_xwayland(spawner, wl_client_fd[client], wm_fd[client]);
        return; // Keep compiler happy (doesn't reach here)

    default:
        close(wl_client_fd[client]);
        close(wm_fd[client]);
        x11_fd = Fd{wm_fd[server]};
        wayland_fd = Fd{wl_client_fd[server]};
        connect_wm_to_xwayland();
        break;
    }
}

mf::XWaylandServer::~XWaylandServer()
{
    mir::log_info("Deiniting xwayland server");

    // Terminate any running xservers
    if (kill(xwayland_pid, SIGTERM) == 0)
    {
        std::this_thread::sleep_for(100ms);// After 100ms...
        if (kill(xwayland_pid, 0) == 0)    // ...if Xwayland is still running...
        {
            mir::log_info("Xwayland didn't close, killing it");
            kill(xwayland_pid, SIGKILL);     // ...then kill it!
        }
    }
}

void mf::XWaylandServer::execl_xwayland(XWaylandSpawner const& spawner, int wl_client_client_fd, int wm_client_fd)
{
    setenv("EGL_PLATFORM", "DRM", 1);

    auto const wl_client_fd = dup(wl_client_client_fd);
    if (wl_client_fd < 0)
        mir::log_error("Failed to duplicate xwayland FD");
    else
        setenv("WAYLAND_SOCKET", std::to_string(wl_client_fd).c_str(), 1);

    auto const wm_fd = dup(wm_client_fd);
    if (wm_fd < 0)
    mir::log_error("Failed to duplicate xwayland wm FD");
    auto const wm_fd_str = std::to_string(wm_fd);

    auto const dsp_str = spawner.x11_display();

    // Propagate SIGUSR1 to parent process
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &action, nullptr);

    std::vector<char const*> args =
        {
            xwayland_path.c_str(),
            dsp_str.c_str(),
            "-rootless",
            "-wm", wm_fd_str.c_str(),
            "-terminate",
        };

    for (auto const& fd : spawner.socket_fds())
    {
        XWaylandSpawner::set_cloexec(fd, false);
        args.push_back("-listen");
        // strdup() may look like a leak, but execvp() will trash all resource management
        args.push_back(strdup(std::to_string(fd).c_str()));
    }

    args.push_back(NULL);

    execvp(xwayland_path.c_str(), const_cast<char* const*>(args.data()));
}

namespace
{
bool spin_wait_for(sig_atomic_t& xserver_ready)
{
    auto const end = std::chrono::steady_clock::now() + 5s;

    while (std::chrono::steady_clock::now() < end && !xserver_ready)
    {
        std::this_thread::sleep_for(100ms);
    }

    return xserver_ready;
}
}

void mf::XWaylandServer::connect_wm_to_xwayland()
{
    // We need to set up the signal handling before connecting wl_client_server_fd
    static sig_atomic_t xserver_ready{ false };

    // In practice, there ought to be no contention on xserver_ready, but let's be certain
    static std::mutex xserver_ready_mutex;
    std::lock_guard<decltype(xserver_ready_mutex)> lock{xserver_ready_mutex};
    xserver_ready = false;

    struct sigaction action;
    struct sigaction old_action;
    action.sa_handler = [](int) { xserver_ready = true; };
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGUSR1, &action, &old_action);

    {
        std::mutex client_mutex;
        std::condition_variable client_ready;

        wayland_connector->run_on_wayland_display(
            [this, &client_mutex, &client_ready](wl_display* display)
            {
                std::lock_guard<std::mutex> lock{client_mutex};
                wayland_client = wl_client_create(display, wayland_fd);
                client_ready.notify_all();
            });

        std::unique_lock<std::mutex> lock{client_mutex};
        if (!client_ready.wait_for(lock, 10s, [&]{ return wayland_client; }))
        {
            // "Shouldn't happen" but this is better than hanging.
            mir::fatal_error("Failed to create wl_client for Xwayland");
        }
    }

    //The client can connect, now wait for it to signal ready (SIGUSR1)
    auto const xwayland_startup_timed_out = !spin_wait_for(xserver_ready);
    sigaction(SIGUSR1, &old_action, nullptr);

    if (xwayland_startup_timed_out)
    {
        mir::fatal_error("XWayland failed to start");
    }
}
