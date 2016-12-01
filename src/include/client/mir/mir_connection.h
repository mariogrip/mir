/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */
#ifndef MIR_CLIENT_MIR_CONNECTION_H_
#define MIR_CLIENT_MIR_CONNECTION_H_

#include "mir/lifecycle_control.h"
#include "mir/ping_handler.h"
#include "mir/error_handler.h"
//#include "mir_wait_handle.h"

#include "mir/geometry/size.h"
#include "mir/client_platform.h"
#include "mir/frontend/surface_id.h"
#include "mir/client_context.h"
#include "mir_toolkit/mir_client_library.h"
#include "mir_toolkit/client_types_nbs.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

struct MirWaitHandle;
namespace google { namespace protobuf { class Closure; }}
namespace mir
{
namespace input
{
class InputDevices;
}
namespace protobuf
{
class Void;
class BufferStream;
class Connection;
class ConnectParameters;
class PlatformOperationMessage;
class DisplayConfiguration;
}
/// The client-side library implementation namespace
namespace client
{
class ConnectionConfiguration;
class ClientPlatformFactory;
class ConnectionSurfaceMap;
class DisplayConfiguration;
class EventHandlerRegister;
class AsyncBufferFactory;
class MirBuffer;
class BufferStream;

namespace rpc
{
class DisplayServer;
class DisplayServerDebug;
class MirBasicRpcChannel;
}
}

namespace input
{
namespace receiver
{
class InputPlatform;
}
}

namespace logging
{
class Logger;
}

namespace dispatch
{
class ThreadedDispatcher;
}
}

struct MirConnection : mir::client::ClientContext
{
public:
    MirConnection(std::string const& error_message);

    MirConnection(mir::client::ConnectionConfiguration& conf);
    ~MirConnection() noexcept;

    MirConnection(MirConnection const &) = delete;
    MirConnection& operator=(MirConnection const &) = delete;

    MirWaitHandle* create_surface(
        MirSurfaceSpec const& spec,
        mir_surface_callback callback,
        void * context);
    MirWaitHandle* release_surface(
        MirSurface *surface,
        mir_surface_callback callback,
        void *context);

    MirPromptSession* create_prompt_session();

    char const * get_error_message();

    MirWaitHandle* connect(
        const char* app_name,
        mir_connected_callback callback,
        void * context);

    MirWaitHandle* disconnect();

    MirWaitHandle* platform_operation(
        MirPlatformMessage const* request,
        mir_platform_operation_callback callback, void* context);

    void register_lifecycle_event_callback(mir_lifecycle_event_callback callback, void* context);

    void register_ping_event_callback(mir_ping_event_callback callback, void* context);
    void pong(int32_t serial);

    void register_display_change_callback(mir_display_config_callback callback, void* context);

    void register_error_callback(mir_error_callback callback, void* context);

    void populate(MirPlatformPackage& platform_package);
    void populate_graphics_module(MirModuleProperties& properties) override;
    MirDisplayConfiguration* create_copy_of_display_config();
    std::unique_ptr<mir::protobuf::DisplayConfiguration> snapshot_display_configuration() const;
    void available_surface_formats(MirPixelFormat* formats,
                                   unsigned int formats_size, unsigned int& valid_formats);

    std::shared_ptr<MirBufferStream> make_consumer_stream(
       mir::protobuf::BufferStream const& protobuf_bs);

    typedef void (*buffer_stream_callback)(mir::client::BufferStream* stream, void* context);

    MirWaitHandle* create_client_buffer_stream(
        int width, int height,
        MirPixelFormat format,
        MirBufferUsage buffer_usage,
        MirRenderSurface* render_surface,
        mir_buffer_stream_callback mbs_callback,
        void *context);
    std::shared_ptr<mir::client::BufferStream> create_client_buffer_stream_with_id(
        int width, int height,
        MirRenderSurface* render_surface,
        mir::protobuf::BufferStream const& a_protobuf_bs);
    MirWaitHandle* release_buffer_stream(
        MirBufferStream*,
        mir_buffer_stream_callback callback,
        void *context);

    void create_presentation_chain(
        mir_presentation_chain_callback callback,
        void *context);
    void release_presentation_chain(MirPresentationChain* context);

    void release_consumer_stream(MirBufferStream*);

    static bool is_valid(MirConnection *connection);

    EGLNativeDisplayType egl_native_display();
    MirPixelFormat       egl_pixel_format(EGLDisplay, EGLConfig) const;

    void on_stream_created(int id, MirBufferStream* stream);

    MirWaitHandle* configure_display(MirDisplayConfiguration* configuration);
    void done_display_configure();

    MirWaitHandle* set_base_display_configuration(MirDisplayConfiguration const* configuration);
    void preview_base_display_configuration(
        mir::protobuf::DisplayConfiguration const& configuration,
        std::chrono::seconds timeout);
    void confirm_base_display_configuration(
        mir::protobuf::DisplayConfiguration const& configuration);
    void cancel_base_display_configuration_preview();
    void done_set_base_display_configuration();

    std::shared_ptr<mir::client::rpc::MirBasicRpcChannel> rpc_channel() const
    {
        return channel;
    }

    mir::client::rpc::DisplayServer& display_server();
    mir::client::rpc::DisplayServerDebug& debug_display_server();
    std::shared_ptr<mir::input::InputDevices> const& the_input_devices() const
    {
        return input_devices;
    }

    std::shared_ptr<mir::client::ConnectionSurfaceMap> const& connection_surface_map() const
    {
        return surface_map;
    }

    void allocate_buffer(
        mir::geometry::Size size, MirPixelFormat format, MirBufferUsage usage,
        mir_buffer_callback callback, void* context);
    void release_buffer(mir::client::MirBuffer* buffer);

    void create_render_surface_with_content(
        mir::geometry::Size logical_size,
        mir_render_surface_callback callback,
        void* context,
        void** native_window);
    void release_render_surface_with_content(
        void* render_surface);

    void* request_interface(char const* name, int version);

    void connected(mir_connected_callback callback, void * context);
private:
    struct SurfaceCreationRequest;
    std::vector<std::shared_ptr<SurfaceCreationRequest>> surface_requests;
    void surface_created(SurfaceCreationRequest*);

    struct StreamCreationRequest;
    std::vector<std::shared_ptr<StreamCreationRequest>> stream_requests;
    void stream_created(StreamCreationRequest*);
    void stream_error(std::string const& error_msg, std::shared_ptr<StreamCreationRequest> const& request);

    struct ChainCreationRequest;
    std::vector<std::shared_ptr<ChainCreationRequest>> context_requests;
    void context_created(ChainCreationRequest*);
    void chain_error(std::string const& error_msg, std::shared_ptr<ChainCreationRequest> const& request);

    struct RenderSurfaceCreationRequest;
    std::vector<std::shared_ptr<RenderSurfaceCreationRequest>> render_surface_requests;
    void render_surface_created(RenderSurfaceCreationRequest*);
    void render_surface_error(std::string const& error_msg, std::shared_ptr<RenderSurfaceCreationRequest> const& request);

    void populate_server_package(MirPlatformPackage& platform_package) override;
    struct Deregisterer
    { MirConnection* const self; ~Deregisterer(); } deregisterer;

    mutable std::mutex mutex; // Protects all members of *this (except release_wait_handles)

    std::shared_ptr<mir::client::ClientPlatform> platform;
    std::shared_ptr<mir::client::ConnectionSurfaceMap> surface_map;
    std::shared_ptr<mir::client::AsyncBufferFactory> buffer_factory;
    std::shared_ptr<mir::client::rpc::MirBasicRpcChannel> const channel;
    std::unique_ptr<mir::client::rpc::DisplayServer> server;
    std::unique_ptr<mir::client::rpc::DisplayServerDebug> debug;
    std::shared_ptr<mir::logging::Logger> const logger;
    std::unique_ptr<mir::protobuf::Void> void_response;
    std::unique_ptr<mir::protobuf::Connection> connect_result;
    std::atomic<bool> connect_done;
    std::unique_ptr<mir::protobuf::Void> ignored;
    std::unique_ptr<mir::protobuf::ConnectParameters> connect_parameters;
    std::unique_ptr<mir::protobuf::PlatformOperationMessage> platform_operation_reply;
    std::unique_ptr<mir::protobuf::DisplayConfiguration> display_configuration_response;
    std::unique_ptr<mir::protobuf::Void> set_base_display_configuration_response;
    std::atomic<bool> disconnecting{false};

    mir::frontend::SurfaceId next_error_id(std::unique_lock<std::mutex> const&);
    int surface_error_id{-1};

    std::shared_ptr<mir::client::ClientPlatformFactory> const client_platform_factory;
    std::shared_ptr<mir::client::ClientBufferFactory> client_buffer_factory;
    std::shared_ptr<EGLNativeDisplayType> native_display;

    std::shared_ptr<mir::input::receiver::InputPlatform> const input_platform;

    std::string error_message;

    std::unique_ptr<MirWaitHandle> connect_wait_handle;
    std::unique_ptr<MirWaitHandle> disconnect_wait_handle;
    std::unique_ptr<MirWaitHandle> platform_operation_wait_handle;
    std::unique_ptr<MirWaitHandle> configure_display_wait_handle;
    std::unique_ptr<MirWaitHandle> set_base_display_configuration_wait_handle;
    std::mutex release_wait_handle_guard;
    std::vector<MirWaitHandle*> release_wait_handles;

    std::shared_ptr<mir::client::DisplayConfiguration> const display_configuration;
    std::shared_ptr<mir::input::InputDevices> const input_devices;

    std::shared_ptr<mir::client::LifecycleControl> const lifecycle_control;

    std::shared_ptr<mir::client::PingHandler> const ping_handler;

    std::shared_ptr<mir::client::ErrorHandler> error_handler;

    std::shared_ptr<mir::client::EventHandlerRegister> const event_handler_register;

    std::unique_ptr<google::protobuf::Closure> const pong_callback;

    std::unique_ptr<mir::dispatch::ThreadedDispatcher> const eventloop;
    

    struct SurfaceRelease;
    struct StreamRelease;

    MirConnection* next_valid{nullptr};

    void set_error_message(std::string const& error);
    void done_disconnect();
    void released(SurfaceRelease);
    void released(StreamRelease);
    void done_platform_operation(mir_platform_operation_callback, void* context);
    bool validate_user_display_config(MirDisplayConfiguration const* config);

    int const nbuffers;
};

#endif /* MIR_CLIENT_MIR_CONNECTION_H_ */
