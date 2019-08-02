/*
 * Copyright © 2018 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by:
 *   Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
 *   Alan Griffiths <alan@octopull.co.uk>
 */

#include "wlshmbuffer.h"
#include "wayland_executor.h"

#include <mir/log.h>

#include <wayland-server-protocol.h>

#include MIR_SERVER_GL_H
#include MIR_SERVER_GLEXT_H

#include <boost/throw_exception.hpp>

#include <cstring>

namespace
{
wl_shm_buffer* shm_buffer_from_resource_checked(wl_resource* resource)
{
    auto const buffer = wl_shm_buffer_get(resource);
    if (!buffer)
    {
        BOOST_THROW_EXCEPTION((std::logic_error{"Tried to create WlShmBuffer from non-shm resource"}));
    }

    return buffer;
}

MirPixelFormat wl_format_to_mir_format(uint32_t format)
{
    switch (format)
    {
        case WL_SHM_FORMAT_ARGB8888:
            return mir_pixel_format_argb_8888;
        case WL_SHM_FORMAT_XRGB8888:
            return mir_pixel_format_xrgb_8888;
        case WL_SHM_FORMAT_RGBA4444:
            return mir_pixel_format_rgba_4444;
        case WL_SHM_FORMAT_RGBA5551:
            return mir_pixel_format_rgba_5551;
        case WL_SHM_FORMAT_RGB565:
            return mir_pixel_format_rgb_565;
        case WL_SHM_FORMAT_RGB888:
            return mir_pixel_format_rgb_888;
        case WL_SHM_FORMAT_BGR888:
            return mir_pixel_format_bgr_888;
        case WL_SHM_FORMAT_XBGR8888:
            return mir_pixel_format_xbgr_8888;
        case WL_SHM_FORMAT_ABGR8888:
            return mir_pixel_format_abgr_8888;
        default:
            return mir_pixel_format_invalid;
    }
}

bool get_gl_pixel_format(
    MirPixelFormat mir_format,
    GLenum& gl_format,
    GLenum& gl_type)
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
    GLenum const argb = GL_BGRA_EXT;
    GLenum const abgr = GL_RGBA;
#elif __BYTE_ORDER == __BIG_ENDIAN
    // TODO: Big endian support
GLenum const argb = GL_INVALID_ENUM;
GLenum const abgr = GL_INVALID_ENUM;
//GLenum const rgba = GL_RGBA;
//GLenum const bgra = GL_BGRA_EXT;
#endif

    static const struct
    {
        MirPixelFormat mir_format;
        GLenum gl_format, gl_type;
    } mapping[mir_pixel_formats] =
        {
            {mir_pixel_format_invalid,   GL_INVALID_ENUM, GL_INVALID_ENUM},
            {mir_pixel_format_abgr_8888, abgr,            GL_UNSIGNED_BYTE},
            {mir_pixel_format_xbgr_8888, abgr,            GL_UNSIGNED_BYTE},
            {mir_pixel_format_argb_8888, argb,            GL_UNSIGNED_BYTE},
            {mir_pixel_format_xrgb_8888, argb,            GL_UNSIGNED_BYTE},
            {mir_pixel_format_bgr_888,   GL_INVALID_ENUM, GL_INVALID_ENUM},
            {mir_pixel_format_rgb_888,   GL_RGB,          GL_UNSIGNED_BYTE},
            {mir_pixel_format_rgb_565,   GL_RGB,          GL_UNSIGNED_SHORT_5_6_5},
            {mir_pixel_format_rgba_5551, GL_RGBA,         GL_UNSIGNED_SHORT_5_5_5_1},
            {mir_pixel_format_rgba_4444, GL_RGBA,         GL_UNSIGNED_SHORT_4_4_4_4},
        };

    if (mir_format > mir_pixel_format_invalid &&
        mir_format < mir_pixel_formats &&
        mapping[mir_format].mir_format == mir_format) // just a sanity check
    {
        gl_format = mapping[mir_format].gl_format;
        gl_type = mapping[mir_format].gl_type;
    }
    else
    {
        gl_format = GL_INVALID_ENUM;
        gl_type = GL_INVALID_ENUM;
    }

    return gl_format != GL_INVALID_ENUM && gl_type != GL_INVALID_ENUM;
}
}

namespace mf = mir::frontend;
namespace mg = mir::graphics;
using namespace mir::geometry;

mf::WlShmBuffer::~WlShmBuffer()
{
    executor->spawn([buffer_mutex = buffer_mutex, buffer = buffer, resource = resource]()
        {
            std::lock_guard <std::mutex> lock{*buffer_mutex};
            if (buffer) {
                wl_resource_queue_event(resource, WL_BUFFER_RELEASE);
            }
        });
}

std::shared_ptr<mg::Buffer> mf::WlShmBuffer::mir_buffer_from_wl_buffer(
    wl_resource *buffer,
    std::shared_ptr<Executor> executor,
    std::function<void()> &&on_consumed)
{
    std::shared_ptr <WlShmBuffer> mir_buffer;
    DestructionShim *shim;

    if (auto notifier = wl_resource_get_destroy_listener(buffer, &on_buffer_destroyed)) {
        // We've already constructed a shim for this buffer, update it.
        shim = wl_container_of(notifier, shim, destruction_listener);

        if (!(mir_buffer = shim->associated_buffer.lock())) {
            /*
             * We've seen this wl_buffer before, but all the WlShmBuffers associated with it
             * have been destroyed.
             *
             * Recreate a new WlShmBuffer to track the new compositor lifetime.
             */
            mir_buffer = std::shared_ptr < WlShmBuffer > {new WlShmBuffer{buffer, executor, std::move(on_consumed)}};
            shim->associated_buffer = mir_buffer;
        }
    } else {
        mir_buffer = std::shared_ptr < WlShmBuffer > {new WlShmBuffer{buffer, executor, std::move(on_consumed)}};
        shim = new DestructionShim;
        shim->destruction_listener.notify = &on_buffer_destroyed;
        shim->associated_buffer = mir_buffer;

        wl_resource_add_destroy_listener(buffer, &shim->destruction_listener);
    }

    mir_buffer->buffer_mutex = shim->mutex;
    return mir_buffer;
}

std::shared_ptr <mg::NativeBuffer> mf::WlShmBuffer::native_buffer_handle() const
{
    return nullptr;
}

Size mf::WlShmBuffer::size() const
{
    return size_;
}

MirPixelFormat mf::WlShmBuffer::pixel_format() const
{
    return format_;
}

mg::NativeBufferBase* mf::WlShmBuffer::native_buffer_base()
{
    return this;
}

void mf::WlShmBuffer::gl_bind_to_texture()
{
    GLenum format, type;

    if (get_gl_pixel_format(
        format_,
        format,
        type)) {
        /*
         * All existing Mir logic assumes that strides are whole multiples of
         * pixels. And OpenGL defaults to expecting strides are multiples of
         * 4 bytes. These assumptions used to be compatible when we only had
         * 4-byte pixels but now we support 2/3-byte pixels we need to be more
         * careful...
         */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        read(
            [this, format, type](unsigned char const *pixels)
            {
                auto const size = this->size();
                glTexImage2D(GL_TEXTURE_2D, 0, format,
                             size.width.as_int(), size.height.as_int(),
                             0, format, type, pixels);
            });
    }
}

void mf::WlShmBuffer::bind()
{
    gl_bind_to_texture();
}

void mf::WlShmBuffer::secure_for_render()
{
}

void mf::WlShmBuffer::write(unsigned char const *pixels, size_t size)
{
    std::lock_guard <std::mutex> lock{*buffer_mutex};
    wl_shm_buffer_begin_access(buffer);
    auto data = wl_shm_buffer_get_data(buffer);
    ::memcpy(data, pixels, size);
    wl_shm_buffer_end_access(buffer);
}

void mf::WlShmBuffer::read(std::function<void(unsigned char const *)> const &do_with_pixels)
{
    std::lock_guard <std::mutex> lock{*buffer_mutex};
    if (!buffer) {
        log_warning("Attempt to read from WlShmBuffer after the wl_buffer has been destroyed");
        return;
    }

    if (!consumed) {
        on_consumed();
        consumed = true;
    }

    do_with_pixels(static_cast<unsigned char const *>(data.get()));
}

Stride mf::WlShmBuffer::stride() const
{
    return stride_;
}

mf::WlShmBuffer::WlShmBuffer(
    wl_resource *buffer,
    std::shared_ptr<Executor> executor,
    std::function<void()> &&on_consumed)
    :
    buffer{shm_buffer_from_resource_checked(buffer)},
    resource{buffer},
    size_{wl_shm_buffer_get_width(this->buffer), wl_shm_buffer_get_height(this->buffer)},
    stride_{wl_shm_buffer_get_stride(this->buffer)},
    format_{wl_format_to_mir_format(wl_shm_buffer_get_format(this->buffer))},
    data{std::make_unique<uint8_t[]>(size_.height.as_int() * stride_.as_int())},
    consumed{false},
    on_consumed{std::move(on_consumed)},
    executor{executor}
{
    if (stride_.as_int() < size_.width.as_int() * MIR_BYTES_PER_PIXEL(format_)) {
        wl_resource_post_error(
            resource,
            WL_SHM_ERROR_INVALID_STRIDE,
            "Stride (%u) is less than width × bytes per pixel (%u×%u). "
                "Did you accidentally specify stride in pixels?",
            stride_.as_int(), size_.width.as_int(), MIR_BYTES_PER_PIXEL(format_));

        BOOST_THROW_EXCEPTION((
                                  std::runtime_error{"Buffer has invalid stride"}));
    }

    wl_shm_buffer_begin_access(this->buffer);
    std::memcpy(data.get(), wl_shm_buffer_get_data(this->buffer), size_.height.as_int() * stride_.as_int());
    wl_shm_buffer_end_access(this->buffer);
}

void mf::WlShmBuffer::on_buffer_destroyed(wl_listener *listener, void *)
{
    static_assert(
        std::is_standard_layout<DestructionShim>::value,
        "DestructionShim must be Standard Layout for wl_container_of to be defined behaviour");

    DestructionShim *shim;
    shim = wl_container_of(listener, shim, destruction_listener);

    {
        if (auto mir_buffer = shim->associated_buffer.lock()) {
            std::lock_guard <std::mutex> lock{*shim->mutex};
            mir_buffer->buffer = nullptr;
        }
    }

    delete shim;
}
