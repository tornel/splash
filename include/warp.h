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
 * @warp.h
 * The Warp class, designed to allow for projection warping
 */

#ifndef SPLASH_WARP_H
#define SPLASH_WARP_H

#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "config.h"

#include "coretypes.h"
#include "basetypes.h"
#include "camera.h"
#include "mesh_bezierPatch.h"
#include "object.h"
#include "texture.h"
#include "texture_image.h"

namespace Splash {

/*************/
class Warp : public Texture
{
    public:
        /**
         * Constructor
         */
        Warp(RootObjectWeakPtr root);

        /**
         * Destructor
         */
        ~Warp();

        /**
         * No copy constructor, but a move one
         */
        Warp(const Warp&) = delete;
        Warp(Warp&&) = default;
        Warp& operator=(const Warp&) = delete;

        /**
         * Bind / unbind this texture of this warp
         */
        void bind();
        void unbind();

        /**
         * Get the shader parameters related to this texture
         * Texture should be locked first
         */
        std::unordered_map<std::string, Values> getShaderUniforms() const;

        /**
         * Get the rendered texture
         */
        std::shared_ptr<Texture_Image> getTexture() const {return _outTexture;}

        /**
         * Get spec of the texture
         */
        ImageBufferSpec getSpec() const {return _outTextureSpec;}

        /**
         * Try to link / unlink the given BaseObject to this
         */
        bool linkTo(std::shared_ptr<BaseObject> obj);
        void unlinkFrom(std::shared_ptr<BaseObject> obj);

        /**
         * Get the coordinates of the closest vertex to the given point
         * Returns its index in the bezier patch
         */
        int pickControlPoint(glm::vec2 p, glm::vec2& v);

        /**
         * Warps should always be saved as it hold user-modifiable parameters
         */
        void setSavable(bool savable) {_savable = true;}

        /**
         * Update the texture according to the owned Image
         */
        void update();

    private:
        bool _isInitialized {false};
        GlWindowPtr _window;
        std::weak_ptr<Camera> _inCamera;

        GLuint _fbo {0};
        std::shared_ptr<Texture_Image> _outTexture {nullptr};
        std::shared_ptr<Mesh_BezierPatch> _screenMesh {nullptr};
        std::shared_ptr<Object> _screen {nullptr};
        ImageBufferSpec _outTextureSpec;

        // Some default models use in various situations
        std::list<std::shared_ptr<Mesh>> _modelMeshes;
        std::list<std::shared_ptr<Geometry>> _modelGeometries;
        std::unordered_map<std::string, std::shared_ptr<Object>> _models;

        // Render options
        bool _showControlPoints {false};
        int _selectedControlPointIndex {-1};

        /**
         * Init function called in constructors
         */
        void init();

        /**
         * Load some defaults models
         */
        void loadDefaultModels();

        /**
         * Setup the output texture
         */
        void setOutput();

        /**
         * Updates the shader uniforms according to the textures and images
         * the warp is connected to.
         */
        void updateUniforms();

        /**
         * Register new functors to modify attributes
         */
        void registerAttributes();
};

} // end of namespace

#endif // SPLASH_WARP_H
