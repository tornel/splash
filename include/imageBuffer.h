/*
 * Copyright (C) 2016 Emmanuel Durand
 *
 * This file is part of Splash.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Splash is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Splash.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * @imageBuffer.h
 * The ImageBufferBuffer and ImageBufferBufferSpec classes
 */

#ifndef SPLASH_IMAGEBUFFER_H
#define SPLASH_IMAGEBUFFER_H

#include <chrono>
#include <memory>
#include <mutex>

#include "config.h"

#include "coretypes.h"
#include "basetypes.h"

namespace Splash {

/*************/
class ImageBufferSpec
{
    public:
        enum class Type : uint32_t {
            UINT8,
            UINT16,
            FLOAT
        };

        ImageBufferSpec() {};
        ImageBufferSpec(unsigned int w, unsigned int h, unsigned int c, ImageBufferSpec::Type t)
        {
            width = w;
            height = h;
            channels = c;
            type = t;

            switch (channels)
            {
            default:
                format = {"R", "G", "B"};
                break;
            case 0:
                break;
            case 1:
                format = {"R"};
                break;
            case 2:
                format = {"R", "G"};
                break;
            case 3:
                format = {"R", "G", "B"};
                break;
            case 4:
                format = {"R", "G", "B", "A"};
                break;
            }
        }

        uint32_t width {0};
        uint32_t height {0};
        uint32_t channels {0};
        Type type {Type::UINT8};
        std::vector<std::string> format {};

        inline bool operator==(const ImageBufferSpec& spec)
        {
            if (width != spec.width)
                return false;
            if (height != spec.height)
                return false;
            if (channels != spec.channels)
                return false;
            if (type != spec.type)
                return false;
            if (format != spec.format)
                return false;

            return true;
        }

        inline bool operator!=(const ImageBufferSpec& spec)
        {
            return !(*this == spec);
        }

        std::string to_string();
        void from_string(const std::string& spec);

        int pixelBytes()
        {
            int bytes = channels;
            switch (type)
            {
            case Type::UINT8:
                break;
            case Type::UINT16:
                bytes *= 2;
                break;
            case Type::FLOAT:
                bytes *= 4;
                break;
            }

            return bytes;
        }

        int rawSize()
        {
            return pixelBytes() * width * height;
        }
};

/*************/
class ImageBuffer
{
    public:
        /**
         * Constructor
         */
        ImageBuffer();
        ImageBuffer(const ImageBufferSpec& spec);
        ImageBuffer(unsigned int width, unsigned int height, unsigned int channels, ImageBufferSpec::Type type);

        /**
         * Destructor
         */
        ~ImageBuffer();

        ImageBuffer(const ImageBuffer& i) = default;
        ImageBuffer(ImageBuffer&& i) = default;
        ImageBuffer& operator=(const ImageBuffer& i) = default;
        ImageBuffer& operator=(ImageBuffer&& i) = default;

        char* data()
        {
            return _buffer.data();
        }

        ImageBufferSpec getSpec() {return _spec;}

        void fill(float value);

        /**
         * Set the inner raw buffer, to use with caution,
         * its size must match the spec
         */
        void setRawBuffer(ResizableArray<char>&& buffer)
        {
            _buffer = std::move(buffer);
        }
        
    private:
        ImageBufferSpec _spec {};
        ResizableArray<char> _buffer;

        void init(const ImageBufferSpec& spec);
};

} // end of namespace

#endif // SPLASH_IMAGEBUFFER_H
