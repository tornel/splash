/*
 * Copyright (C) 2014 Emmanuel Durand
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
 * @widget_global_view.h
 * The global view widget, to calibrate cameras
 */

#ifndef SPLASH_WIDGET_GLOBAL_VIEW_H
#define SPLASH_WIDGET_GLOBAL_VIEW_H

#include "./widget.h"

namespace Splash
{

/*************/
class GuiGlobalView : public GuiWidget
{
    public:
        GuiGlobalView(std::string name = "");
        void render();
        int updateWindowFlags();
        void setCamera(CameraPtr cam);
        void setJoystick(const std::vector<float>& axes, const std::vector<uint8_t>& buttons);
        void setScene(SceneWeakPtr scene) {_scene = scene;}

    protected:
        CameraPtr _camera, _guiCamera;
        SceneWeakPtr _scene;
        bool _camerasHidden {false};
        bool _beginDrag {true};
        bool _noMove {false};

        // Size of the view
        int _camWidth, _camHeight;

        // Joystick state
        std::vector<float> _joyAxes {};
        std::vector<uint8_t> _joyButtons {};
        std::vector<uint8_t> _joyButtonsPrevious {};

        // Store the previous camera values
        struct CameraParameters
        {
            Values eye, target, up, fov, principalPoint;
        };
        std::vector<CameraParameters> _previousCameraParameters;
        Values _newTarget;
        float _newTargetDistance {1.f};

        // Previous point added
        Values _previousPointAdded;

        void processJoystickState();
        void processKeyEvents();
        void processMouseEvents();

        // Actions
        void doCalibration();
        void propagateCalibration(); // Propagates calibration to other Scenes if needed
        void switchHideOtherCameras();
        void nextCamera();
        void revertCalibration();
        void showAllCalibrationPoints();
        void showAllCamerasCalibrationPoints();

        // Other
        std::vector<glm::dmat4> getCamerasRTMatrices();
        std::vector<std::shared_ptr<Camera>> getCameras();
};

} // end of namespace

#endif
