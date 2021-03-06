#include "./widget_global_view.h"

#include <array>
#include <fstream>
#include <imgui.h>

#include "scene.h"

using namespace std;

namespace Splash
{

/*************/
GuiGlobalView::GuiGlobalView(string name)
    : GuiWidget(name)
{
}

/*************/
void GuiGlobalView::render()
{
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::CollapsingHeader(_name.c_str()))
    {
        if (ImGui::Button("Hide other cameras"))
            switchHideOtherCameras();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide all but the selected camera (H while hovering the view)");
        ImGui::SameLine();

        if (ImGui::Button("Show targets"))
            showAllCalibrationPoints();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the target positions for the calibration points (A while hovering the view)");
        ImGui::SameLine();

        if (ImGui::Button("Show points everywhere"))
            showAllCamerasCalibrationPoints();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show this camera's calibration points in other cameras (O while hovering the view)");
        ImGui::SameLine();

        if (ImGui::Button("Calibrate camera"))
            doCalibration();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Calibrate the selected camera (C while hovering the view)");
        ImGui::SameLine();

        if (ImGui::Button("Revert camera"))
            revertCalibration();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Revert the selected camera to its previous calibration (Ctrl + Z while hovering the view)");

        ImVec2 winSize = ImGui::GetWindowSize();
        double leftMargin = ImGui::GetCursorScreenPos().x - ImGui::GetWindowPos().x;

        auto cameras = getCameras();
        
        ImGui::BeginChild("Cameras", ImVec2(ImGui::GetWindowWidth() * 0.25, ImGui::GetWindowWidth() * 0.67), true);
        ImGui::Text("Select a camera:");
        for (auto& camera : cameras)
        {
            camera->render();

            Values size;
            camera->getAttribute("size", size);

            int w = ImGui::GetWindowWidth() - 4 * leftMargin;
            int h = w * size[1].asInt() / size[0].asInt();

            if(ImGui::ImageButton((void*)(intptr_t)camera->getTextures()[0]->getTexId(), ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0)))
            {
                auto scene = _scene.lock();

                // If shift is pressed, we hide / unhide this camera
                if (io.KeyCtrl)
                {
                    scene->sendMessageToWorld("sendAll", {camera->getName(), "hide", -1});
                }
                else
                {
                    // Empty previous camera parameters
                    _previousCameraParameters.clear();

                    // Ensure that all cameras are shown
                    _camerasHidden = false;
                    for (auto& cam : cameras)
                        scene->sendMessageToWorld("sendAll", {cam->getName(), "hide", 0});

                    scene->sendMessageToWorld("sendAll", {_camera->getName(), "frame", 0});
                    scene->sendMessageToWorld("sendAll", {_camera->getName(), "displayCalibration", 0});

                    _camera = camera;

                    scene->sendMessageToWorld("sendAll", {_camera->getName(), "frame", 1});
                    scene->sendMessageToWorld("sendAll", {_camera->getName(), "displayCalibration", 1});
                }
            }

            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(camera->getName().c_str());
        }
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("Calibration", ImVec2(0, ImGui::GetWindowWidth() * 0.67), false);
        if (_camera != nullptr)
        {
            Values size;
            _camera->getAttribute("size", size);

            int w = ImGui::GetWindowWidth() - 2 * leftMargin;
            int h = w * size[1].asInt() / size[0].asInt();

            _camWidth = w;
            _camHeight = h;

            ImGui::Text(("Current camera: " + _camera->getName()).c_str());

            ImGui::Image((void*)(intptr_t)_camera->getTextures()[0]->getTexId(), ImVec2(w, h), ImVec2(0, 1), ImVec2(1, 0));
            if (ImGui::IsItemHoveredRect())
            {
                _noMove = true;
                processKeyEvents();
                processMouseEvents();
                processJoystickState();
            }
            else
            {
                _noMove = false;
            }
        }
        ImGui::EndChild();
    }
}

/*************/
int GuiGlobalView::updateWindowFlags()
{
    ImGuiWindowFlags flags = 0;
    if (_noMove)
    {
        flags |= ImGuiWindowFlags_NoMove;
        flags |= ImGuiWindowFlags_NoScrollWithMouse;
    }
    return flags;
}

/*************/
void GuiGlobalView::setCamera(CameraPtr cam)
{
    if (cam != nullptr)
    {
        _camera = cam;
        _guiCamera = cam;
        _camera->setAttribute("size", {800, 600});
    }
}

/*************/
void GuiGlobalView::setJoystick(const vector<float>& axes, const vector<uint8_t>& buttons)
{
    _joyAxes = axes;
    _joyButtons = buttons;
}

/*************/
vector<glm::dmat4> GuiGlobalView::getCamerasRTMatrices()
{
    auto scene = _scene.lock();
    auto rtMatrices = vector<glm::dmat4>();

    for (auto& obj : scene->_objects)
        if (obj.second->getType() == "camera")
            rtMatrices.push_back(dynamic_pointer_cast<Camera>(obj.second)->computeViewMatrix());
    for (auto& obj : scene->_ghostObjects)
        if (obj.second->getType() == "camera")
            rtMatrices.push_back(dynamic_pointer_cast<Camera>(obj.second)->computeViewMatrix());

    return rtMatrices;
}

/*************/
void GuiGlobalView::nextCamera()
{
    auto scene = _scene.lock();
    vector<CameraPtr> cameras;
    for (auto& obj : scene->_objects)
        if (obj.second->getType() == "camera")
            cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));
    for (auto& obj : scene->_ghostObjects)
        if (obj.second->getType() == "camera")
            cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));

    // Empty previous camera parameters
    _previousCameraParameters.clear();

    // Ensure that all cameras are shown
    _camerasHidden = false;
    for (auto& cam : cameras)
        scene->sendMessageToWorld("sendAll", {cam->getName(), "hide", 0});

    scene->sendMessageToWorld("sendAll", {_camera->getName(), "frame", 0});
    scene->sendMessageToWorld("sendAll", {_camera->getName(), "displayCalibration", 0});

    if (cameras.size() == 0)
        _camera = _guiCamera;
    else if (_camera == _guiCamera)
        _camera = cameras[0];
    else
    {
        for (int i = 0; i < cameras.size(); ++i)
        {
            if (cameras[i] == _camera && i == cameras.size() - 1)
            {
                _camera = _guiCamera;
                break;
            }
            else if (cameras[i] == _camera)
            {
                _camera = cameras[i + 1];
                break;
            }
        }
    }

    if (_camera != _guiCamera)
    {
        scene->sendMessageToWorld("sendAll", {_camera->getName(), "frame", 1});
        scene->sendMessageToWorld("sendAll", {_camera->getName(), "displayCalibration", 1});
    }

    return;
}

/*************/
void GuiGlobalView::revertCalibration()
{
    if (_previousCameraParameters.size() == 0)
        return;
    
    Log::get() << Log::MESSAGE << "GuiGlobalView::" << __FUNCTION__ << " - Reverting camera to previous parameters" << Log::endl;
    
    auto params = _previousCameraParameters.back();
    // We keep the very first calibration, it has proven useful
    if (_previousCameraParameters.size() > 1)
        _previousCameraParameters.pop_back();
    
    _camera->setAttribute("eye", params.eye);
    _camera->setAttribute("target", params.target);
    _camera->setAttribute("up", params.up);
    _camera->setAttribute("fov", params.fov);
    _camera->setAttribute("principalPoint", params.principalPoint);
    
    auto scene = _scene.lock();
    for (auto& obj : scene->_ghostObjects)
        if (_camera->getName() == obj.second->getName())
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "eye", params.eye[0], params.eye[1], params.eye[2]});
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "target", params.target[0], params.target[1], params.target[2]});
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "up", params.up[0], params.up[1], params.up[2]});
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "fov", params.fov[0]});
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "principalPoint", params.principalPoint[0], params.principalPoint[1]});
        }
}

/*************/
void GuiGlobalView::showAllCalibrationPoints()
{
    auto scene = _scene.lock();
    scene->sendMessageToWorld("sendAll", {_camera->getName(), "switchShowAllCalibrationPoints"});
}

/*************/
void GuiGlobalView::showAllCamerasCalibrationPoints()
{
    auto scene = _scene.lock();
    if (_camera == _guiCamera)
        _guiCamera->setAttribute("switchDisplayAllCalibration", {});
    else
        scene->sendMessageToWorld("sendAll", {_camera->getName(), "switchDisplayAllCalibration"});
}

/*************/
void GuiGlobalView::doCalibration()
{
    CameraParameters params;
     // We keep the current values
    _camera->getAttribute("eye", params.eye);
    _camera->getAttribute("target", params.target);
    _camera->getAttribute("up", params.up);
    _camera->getAttribute("fov", params.fov);
    _camera->getAttribute("principalPoint", params.principalPoint);
    _previousCameraParameters.push_back(params);

    // Calibration
    _camera->doCalibration();
    propagateCalibration();

    return;
}

/*************/
void GuiGlobalView::propagateCalibration()
{
    bool isDistant {false};
    auto scene = _scene.lock();
    for (auto& obj : scene->_ghostObjects)
        if (_camera->getName() == obj.second->getName())
            isDistant = true;
    
    if (isDistant)
    {
        vector<string> properties {"eye", "target", "up", "fov", "principalPoint"};
        auto scene = _scene.lock();
        for (auto& p : properties)
        {
            Values values;
            _camera->getAttribute(p, values);

            Values sendValues {_camera->getName(), p};
            for (auto& v : values)
                sendValues.push_back(v);

            scene->sendMessageToWorld("sendAll", sendValues);
        }
    }
}

/*************/
void GuiGlobalView::switchHideOtherCameras()
{
    auto scene = _scene.lock();
    vector<CameraPtr> cameras;
    for (auto& obj : scene->_objects)
        if (dynamic_pointer_cast<Camera>(obj.second).get() != nullptr)
            cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));
    for (auto& obj : scene->_ghostObjects)
        if (dynamic_pointer_cast<Camera>(obj.second).get() != nullptr)
            cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));

    if (!_camerasHidden)
    {
        for (auto& cam : cameras)
            if (cam.get() != _camera.get())
                scene->sendMessageToWorld("sendAll", {cam->getName(), "hide", 1});
        _camerasHidden = true;
    }
    else
    {
        for (auto& cam : cameras)
            if (cam.get() != _camera.get())
                scene->sendMessageToWorld("sendAll", {cam->getName(), "hide", 0});
        _camerasHidden = false;
    }
}

/*************/
void GuiGlobalView::processJoystickState()
{
    auto scene = _scene.lock();

    float speed = 1.f;

    // Buttons
    if (_joyButtons.size() >= 4)
    {
        if (_joyButtons[0] == 1 && _joyButtons[0] != _joyButtonsPrevious[0])
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "selectPreviousCalibrationPoint"});
        }
        else if (_joyButtons[1] == 1 && _joyButtons[1] != _joyButtonsPrevious[1])
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "selectNextCalibrationPoint"});
        }
        else if (_joyButtons[2] == 1)
        {
            speed = 10.f;
        }
        else if (_joyButtons[3] == 1 && _joyButtons[3] != _joyButtonsPrevious[3])
        {
            doCalibration();
        }
    }
    if (_joyButtons.size() >= 6)
    {
        if (_joyButtons[4] == 1 && _joyButtons[4] != _joyButtonsPrevious[4])
        {
            showAllCalibrationPoints();
        }
        else if (_joyButtons[5] == 1 && _joyButtons[5] != _joyButtonsPrevious[5])
        {
            switchHideOtherCameras();
        }
    }

    _joyButtonsPrevious = _joyButtons;

    // Axes
    if (_joyAxes.size() >= 2)
    {
        float xValue = _joyAxes[0];
        float yValue = -_joyAxes[1]; // Y axis goes downward for joysticks...

        if (xValue != 0.f || yValue != 0.f)
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "moveCalibrationPoint", xValue * speed, yValue * speed});
            _camera->moveCalibrationPoint(0.0, 0.0);
            propagateCalibration();
        }
    }

    // This prevents issues when disconnecting the joystick
    _joyAxes.clear();
    _joyButtons.clear();
}

/*************/
void GuiGlobalView::processKeyEvents()
{
    ImGuiIO& io = ImGui::GetIO();

    if (io.KeysDown[' '] && io.KeysDownDuration[' '] == 0.0)
    {
        nextCamera();
        return;
    }
    else if (io.KeysDown['A'] && io.KeysDownDuration['A'] == 0.0)
    {
        showAllCalibrationPoints();
        return;
    }
    else if (io.KeysDown['C'] && io.KeysDownDuration['C'] == 0.0)
    {
        doCalibration();
        return;
    }
    else if (io.KeysDown['H'] && io.KeysDownDuration['H'] == 0.0)
    {
        switchHideOtherCameras();
        return;
    }
    else if (io.KeysDown['O'] && io.KeysDownDuration['O'] == 0.0)
    {
        showAllCamerasCalibrationPoints();
        return;
    }
    // Reset to the previous camera calibration
    else if (io.KeysDown['Z'] && io.KeysDownDuration['Z'] == 0.0)
    {
        if (io.KeyCtrl)
            revertCalibration();
        return;
    }
    // Arrow keys
    else
    {
        auto scene = _scene.lock();

        float delta = 1.f;
        if (io.KeyShift)
            delta = 0.1f;
        else if (io.KeyCtrl)
            delta = 10.f;
            
        if (io.KeysDown[262])
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "moveCalibrationPoint", delta, 0});
            propagateCalibration();
        }
        if (io.KeysDown[263])
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "moveCalibrationPoint", -delta, 0});
            propagateCalibration();
        }
        if (io.KeysDown[264])
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "moveCalibrationPoint", 0, -delta});
            propagateCalibration();
        }
        if (io.KeysDown[265])
        {
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "moveCalibrationPoint", 0, delta});
            propagateCalibration();
        }

        return;
    }
}

/*************/
void GuiGlobalView::processMouseEvents()
{
    ImGuiIO& io = ImGui::GetIO();

    // Get mouse pos
    ImVec2 mousePos = ImVec2((io.MousePos.x - ImGui::GetCursorScreenPos().x) / _camWidth,
                             -(io.MousePos.y - ImGui::GetCursorScreenPos().y) / _camHeight);

    if (io.MouseDown[0])
    {
        // If selected camera is guiCamera, do nothing
        if (_camera == _guiCamera)
            return;

        // Set a calibration point
        auto scene = _scene.lock();
        if (io.KeyCtrl && io.MouseClicked[0])
        {
            auto scene = _scene.lock();
            Values position = _camera->pickCalibrationPoint(mousePos.x, mousePos.y);
            if (position.size() == 3)
                scene->sendMessageToWorld("sendAll", {_camera->getName(), "removeCalibrationPoint", position[0], position[1], position[2]});
        }
        else if (io.KeyShift) // Define the screenpoint corresponding to the selected calibration point
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "setCalibrationPoint", mousePos.x * 2.f - 1.f, mousePos.y * 2.f - 1.f});
        else if (io.MouseClicked[0]) // Add a new calibration point
        {
            Values position = _camera->pickVertexOrCalibrationPoint(mousePos.x, mousePos.y);
            if (position.size() == 3)
            {
                scene->sendMessageToWorld("sendAll", {_camera->getName(), "addCalibrationPoint", position[0], position[1], position[2]});
                _previousPointAdded = position;
            }
            else
                scene->sendMessageToWorld("sendAll", {_camera->getName(), "deselectCalibrationPoint"});
        }
        return;
    }
    if (io.MouseClicked[1])
    {
        float fragDepth = 0.f;
        _newTarget = _camera->pickFragment(mousePos.x, mousePos.y, fragDepth);

        if (fragDepth == 0.f)
            _newTargetDistance = 1.f;
        else
            _newTargetDistance = -fragDepth * 0.1f;
    }
    if (io.MouseDownDuration[1] > 0.0) 
    {
        // Move the camera
        if (!io.KeyCtrl && !io.KeyShift)
        {
            float dx = io.MouseDelta.x;
            float dy = io.MouseDelta.y;
            auto scene = _scene.lock();

            if (_camera != _guiCamera)
            {
                if (_newTarget.size() == 3)
                    scene->sendMessageToWorld("sendAll", {_camera->getName(), "rotateAroundPoint", dx / 100.f, dy / 100.f, 0, _newTarget[0].asFloat(), _newTarget[1].asFloat(), _newTarget[2].asFloat()});
                else
                    scene->sendMessageToWorld("sendAll", {_camera->getName(), "rotateAroundTarget", dx / 100.f, dy / 100.f, 0});
            }
            else
            {
                if (_newTarget.size() == 3)
                    _camera->setAttribute("rotateAroundPoint", {dx / 100.f, dy / 100.f, 0, _newTarget[0].asFloat(), _newTarget[1].asFloat(), _newTarget[2].asFloat()});
                else
                    _camera->setAttribute("rotateAroundTarget", {dx / 100.f, dy / 100.f, 0});
            }
        }
        // Move the target and the camera (in the camera plane)
        else if (io.KeyShift && !io.KeyCtrl)
        {
            float dx = io.MouseDelta.x * _newTargetDistance;
            float dy = io.MouseDelta.y * _newTargetDistance;
            auto scene = _scene.lock();
            if (_camera != _guiCamera)
                scene->sendMessageToWorld("sendAll", {_camera->getName(), "pan", -dx / 100.f, dy / 100.f, 0.f});
            else
                _camera->setAttribute("pan", {-dx / 100.f, dy / 100.f, 0});
        }
        else if (!io.KeyShift && io.KeyCtrl)
        {
            float dy = io.MouseDelta.y * _newTargetDistance / 100.f;
            auto scene = _scene.lock();
            if (_camera != _guiCamera)
                scene->sendMessageToWorld("sendAll", {_camera->getName(), "forward", dy});
            else
                _camera->setAttribute("forward", {dy});
        }
    }
    if (io.MouseWheel != 0)
    {
        Values fov;
        _camera->getAttribute("fov", fov);
        float camFov = fov[0].asFloat();

        camFov += io.MouseWheel;
        camFov = std::max(2.f, std::min(180.f, camFov));

        auto scene = _scene.lock();
        if (_camera != _guiCamera)
            scene->sendMessageToWorld("sendAll", {_camera->getName(), "fov", camFov});
        else
            _camera->setAttribute("fov", {camFov});
    }
}

/*************/
vector<shared_ptr<Camera>> GuiGlobalView::getCameras()
{
    auto cameras = vector<shared_ptr<Camera>>();

    _guiCamera->setAttribute("size", {ImGui::GetWindowWidth(), ImGui::GetWindowWidth() * 3 / 4});

    auto rtMatrices = getCamerasRTMatrices();
    for (auto& matrix : rtMatrices)
        _guiCamera->drawModelOnce("camera", matrix);
    cameras.push_back(_guiCamera);

    auto scene = _scene.lock();
    for (auto& obj : scene->_objects)
        if (obj.second->getType() == "camera")
            cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));
    for (auto& obj : scene->_ghostObjects)
        if (obj.second->getType() == "camera")
            cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));

    return cameras;
}
} // end of namespace
