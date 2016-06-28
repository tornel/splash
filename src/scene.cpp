#include "scene.h"

#include <utility>

#include "./camera.h"
#include "./filter.h"
#include "./geometry.h"
#include "./gui.h"
#include "./image.h"
#include "./link.h"
#include "./log.h"
#include "./mesh.h"
#include "./object.h"
#include "./queue.h"
#include "./texture.h"
#include "./texture_image.h"
#include "./threadpool.h"
#include "./timer.h"
#include "./warp.h"
#include "./window.h"

#if HAVE_GPHOTO
    #include "./colorcalibrator.h"
#endif

#if HAVE_OSX
    #include "./texture_syphon.h"
#else
    #define GLFW_EXPOSE_NATIVE_X11
    #define GLFW_EXPOSE_NATIVE_GLX
    #include <GLFW/glfw3native.h>
    #include <GL/glxext.h>
#endif

using namespace std;

namespace Splash {

bool Scene::_isGlfwInitialized {false};

/*************/
Scene::Scene(std::string name, bool autoRun)
{
    _self = ScenePtr(this, [](Scene*){}); // A shared pointer with no deleter, how convenient

    Log::get() << Log::DEBUGGING << "Scene::Scene - Scene created successfully" << Log::endl;

    _type = "scene";
    _isRunning = true;
    _name = name;
    _factory = unique_ptr<Factory>(new Factory(_self));

    registerAttributes();

    init(_name);

    _textureUploadFuture = async(std::launch::async, [&](){textureUploadRun();});
    _joystickUpdateFuture = async(std::launch::async, [&](){joystickUpdateLoop();});

    if (autoRun)
        run();
}

/*************/
Scene::~Scene()
{
    unique_lock<mutex> lockTexture(_textureUploadMutex);
    _textureUploadCondition.notify_all();
    lockTexture.unlock();
    _textureUploadFuture.get();

    _joystickUpdateFuture.get();

    if (_httpServerFuture.valid())
    {
        _httpServer->stop();
        _httpServerFuture.get();
    }

    // Cleanup every object
    _mainWindow->setAsCurrentContext();
    lock_guard<recursive_mutex> lockSet(_setMutex); // We don't want our objects to be set while destroyed
    _objects.clear();
    _ghostObjects.clear();
    _mainWindow->releaseContext();

    Log::get() << Log::DEBUGGING << "Scene::~Scene - Destructor" << Log::endl;
}

/*************/
BaseObjectPtr Scene::add(string type, string name)
{
    Log::get() << Log::DEBUGGING << "Scene::" << __FUNCTION__ << " - Creating object of type " << type << Log::endl;

    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    // Check whether an object of this name already exists
    auto objectIt = _objects.find(name);
    if (objectIt != _objects.end())
        return {};

    // Create the wanted object
    if(!_mainWindow->setAsCurrentContext())
        Log::get() << Log::WARNING << "Scene::" << __FUNCTION__ << " - A previous context has not been released." << Log::endl;

    auto obj = _factory->create(type);
    obj->setRemoteType(type); // Not all objects have remote types, but this doesn't harm

    _mainWindow->releaseContext();

    // Add the object to the objects list
    if (obj.get() != nullptr)
    {
        obj->setId(getId());
        name = obj->setName(name);
        if (name == string())
            _objects[to_string(obj->getId())] = obj;
        else
            _objects[name] = obj;

        // Some objects have to be connected to the gui (if the Scene is master)
        if (_gui != nullptr)
        {
            if (obj->getType() == "object")
                link(obj, dynamic_pointer_cast<BaseObject>(_gui));
            else if (obj->getType() == "window" && !_guiLinkedToWindow)
            {
                link(dynamic_pointer_cast<BaseObject>(_gui), obj);
                _guiLinkedToWindow = true;
            }
        }

        // Special treatment for the windows
        if (type == "window")
            obj->setAttribute("swapInterval", {_swapInterval});
    }

    return obj;
}

/*************/
void Scene::addGhost(string type, string name)
{
    // Currently, only Cameras can be ghosts
    if (type != string("camera") && type != string("warp"))
        return;

    // Check whether an object of this name already exists
    auto objectIt = _ghostObjects.find(name);
    if (objectIt != _ghostObjects.end())
        return;

    Log::get() << Log::DEBUGGING << "Scene::" << __FUNCTION__ << " - Creating ghost object of type " << type << Log::endl;

    // Add the object for real ...
    BaseObjectPtr obj = add(type, name);

    // And move it to _ghostObjects
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);
    _objects.erase(obj->getName());
    _ghostObjects[obj->getName()] = obj;
}

/*************/
Values Scene::getAttributeFromObject(string name, string attribute)
{
    auto objectIt = _objects.find(name);
    
    Values values;
    if (objectIt != _objects.end())
    {
        auto& object = objectIt->second;
        object->getAttribute(attribute, values);
    }
    // Ask the World if it knows more about this object
    else
    {
        auto answer = sendMessageToWorldWithAnswer("getAttribute", {name, attribute});
        for (unsigned int i = 1; i < answer.size(); ++i)
            values.push_back(answer[i]);
    }

    return values;
}

/*************/
Values Scene::getAttributeDescriptionFromObject(string name, string attribute)
{
    auto objectIt = _objects.find(name);
    
    Values values;
    if (objectIt != _objects.end())
    {
        auto& object = objectIt->second;
        values.push_back(object->getAttributeDescription(attribute));
    }

    // Ask the World if it knows more about this object
    if (values.size() == 0 || values[0].asString() == "")
    {
        auto answer = sendMessageToWorldWithAnswer("getAttributeDescription", {name, attribute}, 10000);
        if (answer.size() != 0)
        {
            values.clear();
            values.push_back(answer[1]);
        }
    }

    return values;
}

/*************/
Json::Value Scene::getConfigurationAsJson()
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    Json::Value root;

    root[_name] = BaseObject::getConfigurationAsJson();
    // Save objects attributes
    for (auto& obj : _objects)
        if (obj.second->getSavable())
            root[obj.first] = obj.second->getConfigurationAsJson();

    // Save links
    Values links;
    for (auto& obj : _objects)
    {
        if (!obj.second->getSavable())
            continue;

        auto linkedObjects = obj.second->getLinkedObjects();
        for (auto& linkedObj : linkedObjects)
        {
            if (!linkedObj->getSavable())
                continue;

            links.push_back(Values({linkedObj->getName(), obj.second->getName()}));
        }
    }

    root["links"] = getValuesAsJson(links);

    return root;
}

/*************/
bool Scene::link(string first, string second)
{
    BaseObjectPtr source(nullptr);
    BaseObjectPtr sink(nullptr);

    if (_objects.find(first) != _objects.end())
        source = _objects[first];
    if (_objects.find(second) != _objects.end())
        sink = _objects[second];

    if (source.get() != nullptr && sink.get() != nullptr)
        return link(source, sink);
    else
        return false;
}

/*************/
bool Scene::link(BaseObjectPtr first, BaseObjectPtr second)
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    glfwMakeContextCurrent(_mainWindow->get());
    bool result = second->linkTo(first);
    glfwMakeContextCurrent(NULL);

    return result;
}

/*************/
void Scene::unlink(string first, string second)
{
    BaseObjectPtr source(nullptr);
    BaseObjectPtr sink(nullptr);

    if (_objects.find(first) != _objects.end())
        source = _objects[first];
    if (_objects.find(second) != _objects.end())
        sink = _objects[second];

    if (source.get() != nullptr && sink.get() != nullptr)
        unlink(source, sink);
}

/*************/
void Scene::unlink(BaseObjectPtr first, BaseObjectPtr second)
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    glfwMakeContextCurrent(_mainWindow->get());
    second->unlinkFrom(first);
    glfwMakeContextCurrent(NULL);
}

/*************/
bool Scene::linkGhost(string first, string second)
{
    BaseObjectPtr source(nullptr);
    BaseObjectPtr sink(nullptr);

    if (_ghostObjects.find(first) != _ghostObjects.end())
        source = _ghostObjects[first];
    else if (_objects.find(first) != _objects.end())
        source = _objects[first];
    else
        return false;

    if (_ghostObjects.find(second) != _ghostObjects.end())
        sink = _ghostObjects[second];
    else if (_objects.find(second) != _objects.end())
        sink = _objects[second];
    else
        return false;

    return link(source, sink);
}

/*************/
void Scene::unlinkGhost(string first, string second)
{
    BaseObjectPtr source(nullptr);
    BaseObjectPtr sink(nullptr);

    if (_ghostObjects.find(first) != _ghostObjects.end())
        source = _ghostObjects[first];
    else if (_objects.find(first) != _objects.end())
        source = _objects[first];
    else
        return;

    if (_ghostObjects.find(second) != _ghostObjects.end())
        sink = _ghostObjects[second];
    else if (_objects.find(second) != _objects.end())
        sink = _objects[second];
    else
        return;

    unlink(source, sink);
}

/*************/
void Scene::remove(string name)
{
    BaseObjectPtr obj;

    if (_objects.find(name) != _objects.end())
        _objects.erase(name);
    else if (_ghostObjects.find(name) != _ghostObjects.end())
        _ghostObjects.erase(name);
}

/*************/
void Scene::renderBlending()
{
    if ((_glVersion[0] == 4 && _glVersion[1] >= 3) || _glVersion[0] > 4)
    {
        static bool blendComputedInPreviousFrame = false;
        static bool blendComputedOnce = false;

        if (blendComputedOnce && _computeBlendingOnce)
        {
            // This allows for blending reset if it was computed once
            blendComputedOnce = false;
            _computeBlending = false;
            _computeBlendingOnce = false;
            blendComputedInPreviousFrame = true;
        }

        if (_computeBlending)
        {
            // Check for regular or single computation
            if (_computeBlendingOnce)
            {
                _computeBlending = false;
                _computeBlendingOnce = false;
                blendComputedOnce = true;
            }
            else
            {
                blendComputedInPreviousFrame = true;
            }

            // Only the master scene computes the blending
            if (_isMaster)
            {
                vector<CameraPtr> cameras;
                vector<ObjectPtr> objects;
                for (auto& obj : _objects)
                    if (obj.second->getType() == "camera")
                        cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));
                    else if (obj.second->getType() == "object")
                        objects.push_back(dynamic_pointer_cast<Object>(obj.second));
                for (auto& obj : _ghostObjects)
                    if (obj.second->getType() == "camera")
                        cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));
                    else if (obj.second->getType() == "object")
                        objects.push_back(dynamic_pointer_cast<Object>(obj.second));

                if (cameras.size() != 0)
                {
                    for (auto& object : objects)
                        object->resetTessellation();

                    // Tessellate
                    for (auto& camera : cameras)
                    {
                        camera->computeVertexVisibility();
                        camera->blendingTessellateForCurrentCamera();
                    }

                    for (auto& object : objects)
                        object->resetBlendingAttribute();

                    // Compute each camera contribution
                    for (auto& camera : cameras)
                    {
                        camera->computeVertexVisibility();
                        camera->computeBlendingContribution();
                    }

                    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT);
                }

                for (auto& obj : _objects)
                    if (obj.second->getType() == "object")
                        obj.second->setAttribute("activateVertexBlending", {1});

                // If there are some other scenes, send them the blending
                if (_ghostObjects.size() != 0)
                {
                    for (auto& obj : _objects)
                        if (obj.second->getType() == "geometry")
                        {
                            auto serializedGeometry = dynamic_pointer_cast<Geometry>(obj.second)->serialize();
                            _link->sendBuffer(obj.first, std::move(serializedGeometry));
                        }
                    
                    // Notify the other scenes that the blending has been updated
                    sendMessageToWorld("sendAll", {SPLASH_ALL_PEERS, "blendingUpdated"});
                }
            }
            // The non-master scenes only need to activate blending
            else
            {
                // Wait for the master scene to notify us that the blending was updated
                unique_lock<mutex> updateBlendingLock(_vertexBlendingMutex);
                while (!_vertexBlendingReceptionStatus)
                    _vertexBlendingCondition.wait_for(updateBlendingLock, chrono::seconds(1));
                _vertexBlendingReceptionStatus = false;
                    
                for (auto& obj : _objects)
                    if (obj.second->getType() == "object")
                        obj.second->setAttribute("activateVertexBlending", {1});
                    else if (obj.second->getType() == "geometry")
                        dynamic_pointer_cast<Geometry>(obj.second)->useAlternativeBuffers(true);
            }
        }
        // This deactivates the blending
        else if (blendComputedInPreviousFrame)
        {
            blendComputedInPreviousFrame = false;
            blendComputedOnce = false;

            vector<CameraPtr> cameras;
            vector<ObjectPtr> objects;
            for (auto& obj : _objects)
                if (obj.second->getType() == "camera")
                    cameras.push_back(dynamic_pointer_cast<Camera>(obj.second));
                else if (obj.second->getType() == "object")
                    objects.push_back(dynamic_pointer_cast<Object>(obj.second));
            
            if (_isMaster && cameras.size() != 0)
            {
                for (auto& object : objects)
                {
                    object->resetTessellation();
                    object->resetVisibility();
                }
            }

            for (auto& obj : _objects)
                if (obj.second->getType() == "object")
                    obj.second->setAttribute("activateVertexBlending", {0});
                else if (obj.second->getType() == "geometry")
                    dynamic_pointer_cast<Geometry>(obj.second)->useAlternativeBuffers(false);
        }
    }
}

/*************/
void Scene::render()
{
    bool isError {false};
    vector<unsigned int> threadIds;

    // Compute the blending
    Timer::get() << "blending";
    renderBlending();
    Timer::get() >> "blending";

    unique_lock<mutex> lockTexture(_textureUploadMutex);

    Timer::get() << "queues";
    for (auto& obj : _objects)
        if (obj.second->getType() == "queue")
            dynamic_pointer_cast<QueueSurrogate>(obj.second)->update();
    Timer::get() >> "queues";

    Timer::get() << "filters";
    for (auto& obj : _objects)
        if (obj.second->getType() == "filter")
            dynamic_pointer_cast<Filter>(obj.second)->update();
    Timer::get() >> "filters";
    
    Timer::get() << "cameras";
    // We wait for textures to be uploaded, and we prevent any upload while rendering
    // cameras to prevent tearing
    glFlush();
    glWaitSync(_textureUploadFence, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(_textureUploadFence);

    for (auto& obj : _objects)
        if (obj.second->getType() == "camera")
            isError |= dynamic_pointer_cast<Camera>(obj.second)->render();
    Timer::get() >> "cameras";

    _cameraDrawnFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    Timer::get() << "warps";
    for (auto& obj : _objects)
        if (obj.second->getType() == "warp")
            dynamic_pointer_cast<Warp>(obj.second)->update();
    Timer::get() >> "warps";

    lockTexture.unlock(); // Unlock _textureUploadMutex

    // Update the gui
    Timer::get() << "gui";
    if (_gui != nullptr)
        isError |= _gui->render();
    Timer::get() >> "gui";

    // Update the windows
    Timer::get() << "windows";
    glFinish();
    for (auto& obj : _objects)
        if (obj.second->getType() == "window")
            isError |= dynamic_pointer_cast<Window>(obj.second)->render();
    Timer::get() >> "windows";

    // Swap all buffers at once
    Timer::get() << "swap";
    for (auto& obj : _objects)
        if (obj.second->getType() == "window")
            dynamic_pointer_cast<Window>(obj.second)->swapBuffers();
    Timer::get() >> "swap";
}

/*************/
void Scene::run()
{
    while (_isRunning)
    {
        {
            // Execute waiting tasks
            lock_guard<mutex> lockTask(_taskMutex);
            for (auto& task : _taskQueue)
                task();
            _taskQueue.clear();
        }

        if (!_started)
        {
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }
        
        Timer::get() << "sceneLoop";

        {
            lock_guard<recursive_mutex> lockObjects(_objectsMutex);
            _mainWindow->setAsCurrentContext();
            render();
            _mainWindow->releaseContext();
            updateInputs();
        }

        Timer::get() >> "sceneLoop";
    }
}

/*************/
void Scene::updateInputs()
{
    // Update the user events
    glfwPollEvents();
    // Mouse position
    {
        GLFWwindow* win;
        int xpos, ypos;
        Window::getMousePos(win, xpos, ypos);
        if (_gui != nullptr)
            _gui->mousePosition(xpos, ypos);
    }

    // Mouse events
    while (true)
    {
        GLFWwindow* win;
        int btn, action, mods;
        if (!Window::getMouseBtn(win, btn, action, mods))
            break;
        
        if (_gui != nullptr)
            _gui->mouseButton(btn, action, mods);
    }

    // Scrolling events
    while (true)
    {
        GLFWwindow* win;
        double xoffset, yoffset;
        if (!Window::getScroll(win, xoffset, yoffset))
            break;

        if (_gui != nullptr)
            _gui->mouseScroll(xoffset, yoffset);
    }

    // Keyboard events
    while (true)
    {
        GLFWwindow* win;
        int key, action, mods;
        if (!Window::getKeys(win, key, action, mods))
            break;

        // Find where this action happened
        WindowPtr eventWindow;
        for (auto& w : _objects)
            if (w.second->getType() == "window")
            {
                WindowPtr window = dynamic_pointer_cast<Window>(w.second);
                if (window->isWindow(win))
                    eventWindow = window;
            }

        if (key == GLFW_KEY_F)
        {
            if (mods == GLFW_MOD_ALT && action == GLFW_PRESS)
                if (eventWindow.get() != nullptr)
                {
                    eventWindow->switchFullscreen();
                    continue;
                }
        }

        // Send the action to the GUI
        if (_gui != nullptr)
            _gui->key(key, action, mods);
    }

    // Unicode characters events
    while (true)
    {
        GLFWwindow* win;
        unsigned int unicodeChar;
        if (!Window::getChars(win, unicodeChar))
            break;

        // Find where this action happened
        WindowPtr eventWindow;
        for (auto& w : _objects)
            if (w.second->getType() == "window")
            {
                WindowPtr window = dynamic_pointer_cast<Window>(w.second);
                if (window->isWindow(win))
                    eventWindow = window;
            }

        // Send the action to the GUI
        if (_gui != nullptr)
            _gui->unicodeChar(unicodeChar);
    }

    // Joystick state
    if (_isMaster && glfwJoystickPresent(GLFW_JOYSTICK_1))
    {
        lock_guard<mutex> lockJoystick(_joystickUpdateMutex);
        _gui->setJoystick(_joystickAxes, _joystickButtons);
        _joystickAxes.clear();
    }

    // Any file dropped onto the window? Then load it.
    auto paths = Window::getPathDropped();
    if (paths.size() != 0)
    {
        sendMessageToWorld("loadConfig", {paths[0]});
    }

    // Check if we should quit
    if (Window::getQuitFlag())
    {
        sendMessageToWorld("quit");
    }
}

/*************/
void Scene::textureUploadRun()
{
    while (_isRunning)
    {
        if (!_started)
        {
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }

        unique_lock<mutex> lockTexture(_textureUploadMutex);
        _textureUploadCondition.wait(lockTexture);

        if (!_isRunning)
            break;

        _textureUploadWindow->setAsCurrentContext();
        glFlush();
        glWaitSync(_cameraDrawnFence, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(_cameraDrawnFence);

        Timer::get() << "textureUpload";
        for (auto& obj : _objects)
            if (obj.second->getType().find("texture") != string::npos)
                dynamic_pointer_cast<Texture>(obj.second)->update();
        _textureUploadFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        lockTexture.unlock();

        bool expectedAtomicValue = false;
        if (_objectsCurrentlyUpdated.compare_exchange_strong(expectedAtomicValue, true))
        {
            for (auto& obj : _objects)
                if (obj.second->getType().find("texture") != string::npos)
                {
                    auto texImage = dynamic_pointer_cast<Texture_Image>(obj.second);
                    if (texImage)
                        texImage->flushPbo();
                }
            _objectsCurrentlyUpdated = false;
        }

        _textureUploadWindow->releaseContext();
        Timer::get() >> "textureUpload";
    }
}

/*************/
void Scene::setAsMaster(string configFilePath)
{
    _isMaster = true;

    _gui = make_shared<Gui>(_mainWindow, _self);
    _gui->setName("gui");
    _gui->setConfigFilePath(configFilePath);

#if HAVE_GPHOTO
    // Initialize the color calibration object
    _colorCalibrator = make_shared<ColorCalibrator>(_self);
    _colorCalibrator->setName("colorCalibrator");
    _objects["colorCalibrator"] = dynamic_pointer_cast<BaseObject>(_colorCalibrator);
#endif
}

/*************/
void Scene::setAsWorldScene()
{
    // First we create a single camera linked to a single window
    add("camera", "_camera");
    add("window", "_window");
    link("_camera", "_window");

    // Then we need to connect all Objects to the camera
    for (auto& obj : _objects)
        link(obj.first, "_camera");
}

/*************/
void Scene::sendMessageToWorld(const string& message, const Values& value)
{
    RootObject::sendMessage("world", message, value);
}

/*************/
Values Scene::sendMessageToWorldWithAnswer(const string& message, const Values& value, const unsigned long long timeout)
{
    return sendMessageWithAnswer("world", message, value, timeout);
}

/*************/
void Scene::waitTextureUpload()
{
    lock_guard<mutex> lockTexture(_textureUploadMutex);
    glWaitSync(_textureUploadFence, 0, GL_TIMEOUT_IGNORED);
}

/*************/
void Scene::activateBlendingMap(bool once)
{
    if (_isBlendingComputed)
        return;
    _isBlendingComputed = true;

    if ((_glVersion[0] == 4 && _glVersion[1] >= 3) || _glVersion[0] > 4)
    {
        _computeBlending = true;
        _computeBlendingOnce = once;
    }
    else
    {
        lock_guard<recursive_mutex> lockObjects(_objectsMutex);
        _mainWindow->setAsCurrentContext();

        initBlendingMap();
        // Set the blending map to zero
        _blendingMap->setTo(0.f);
        _blendingMap->setName("blendingMap");

        // Compute the contribution of each camera
        for (auto& obj : _objects)
            if (obj.second->getType() == "camera")
                dynamic_pointer_cast<Camera>(obj.second)->computeBlendingMap(_blendingMap);
        for (auto& obj : _ghostObjects)
            if (obj.second->getType() == "camera")
                dynamic_pointer_cast<Camera>(obj.second)->computeBlendingMap(_blendingMap);

        // Filter the output to fill the blanks (dilate filter)
        ImagePtr buffer = make_shared<Image>(_blendingMap->getSpec());
        unsigned short* pixBuffer = (unsigned short*)buffer->data();
        unsigned short* pixels = (unsigned short*)_blendingMap->data();
        int w = _blendingMap->getSpec().width;
        int h = _blendingMap->getSpec().height;
        for (int x = 0; x < w; ++x)
            for (int y = 0; y < h; ++y)
            {
                unsigned short maxValue = 0;
                for (int xx = -1; xx <= 1; ++xx)
                {
                    if (x + xx < 0 || x + xx >= w)
                        continue;
                    for (int yy = -1; yy <= 1; ++yy)
                    {
                        if (y + yy < 0 || y + yy >= h)
                            continue;
                        maxValue = std::max(maxValue, pixels[(y + yy) * w + x + xx]);
                    }
                }
                pixBuffer[y * w + x] = maxValue;
            }
        swap(_blendingMap, buffer);
        _blendingMap->setSavable(false);
        _blendingMap->updateTimestamp();

        // Small hack to handle the fact that texture transfer uses PBOs.
        // If we send the buffer only once, the displayed PBOs wont be the correct one.
        if (_isMaster)
        {
            _link->sendBuffer("blendingMap", _blendingMap->serialize());
            timespec nap {0, (long int)1e8};
            nanosleep(&nap, NULL);
            _link->sendBuffer("blendingMap", _blendingMap->serialize());
        }

        for (auto& obj : _objects)
            if (obj.second->getType() == "object")
                dynamic_pointer_cast<Object>(obj.second)->setBlendingMap(_blendingTexture);

        _computeBlending = true;

        Log::get() << "Scene::" << __FUNCTION__ << " - Camera blending computed" << Log::endl;
        

        _mainWindow->releaseContext();
    }
}

/*************/
void Scene::deactivateBlendingMap()
{
    _isBlendingComputed = false;
    
    if ((_glVersion[0] == 4 && _glVersion[1] >= 3) || _glVersion[0] > 4)
    {
        _computeBlending = false;
        _computeBlendingOnce = true;
    }
    else
    {
        lock_guard<recursive_mutex> lockObjects(_objectsMutex);
        _mainWindow->setAsCurrentContext();

        for (auto& obj : _objects)
            if (obj.second->getType() == "object")
                dynamic_pointer_cast<Object>(obj.second)->resetBlendingMap();

        _computeBlending = false;

        Log::get() << "Scene::" << __FUNCTION__ << " - Camera blending deactivated" << Log::endl;

        _mainWindow->releaseContext();
    }
}

/*************/
void Scene::computeBlendingMap(const std::string& mode)
{
    if (mode == "once")
        activateBlendingMap(true);
    else if (mode == "continuous")
        activateBlendingMap(false);
    else
        deactivateBlendingMap();
}

/*************/
GlWindowPtr Scene::getNewSharedWindow(string name)
{
    string windowName;
    name.size() == 0 ? windowName = "Splash::Window" : windowName = "Splash::" + name;

    if (!_mainWindow)
    {
        Log::get() << Log::WARNING << __FUNCTION__ << " - Main window does not exist, unable to create new shared window" << Log::endl;
        return GlWindowPtr(nullptr);
    }

    // The GL version is the same as in the initialization, so we don't have to reset it here
    glfwWindowHint(GLFW_SAMPLES, SPLASH_SAMPLES);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, false);
    GLFWwindow* window = glfwCreateWindow(512, 512, windowName.c_str(), NULL, _mainWindow->get());
    if (!window)
    {
        Log::get() << Log::WARNING << __FUNCTION__ << " - Unable to create new shared window" << Log::endl;
        return GlWindowPtr(nullptr);
    }
    GlWindowPtr glWindow = make_shared<GlWindow>(window, _mainWindow->get());

    glWindow->setAsCurrentContext();
#if not HAVE_OSX
#ifdef DEBUGGL
    glDebugMessageCallback(Scene::glMsgCallback, (void*)this);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM, 0, nullptr, GL_TRUE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, nullptr, GL_TRUE);
#endif
#endif

#if not HAVE_OSX
#ifdef GLX_NV_swap_group
    if (_maxSwapGroups)
    {
        PFNGLXJOINSWAPGROUPNVPROC nvGLJoinSwapGroup = (PFNGLXJOINSWAPGROUPNVPROC)glfwGetProcAddress("glXJoinSwapGroupNV");
        PFNGLXBINDSWAPBARRIERNVPROC nvGLBindSwapBarrier = (PFNGLXBINDSWAPBARRIERNVPROC)glfwGetProcAddress("glXBindSwapBarrierNV");

        bool nvResult = true;
        nvResult &= nvGLJoinSwapGroup(glfwGetX11Display(), glfwGetX11Window(window), 1);
        nvResult &= nvGLBindSwapBarrier(glfwGetX11Display(), 1, 1);
        if (nvResult)
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Window " << name << " successfully joined the NV swap group" << Log::endl;
        else
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Window " << name << " couldn't join the NV swap group" << Log::endl;
    }
#endif
#endif
    glWindow->releaseContext();

    return glWindow;
}

/*************/
Values Scene::getObjectsNameByType(string type)
{
    Values list;
    for (auto& obj : _objects)
        if (obj.second->getType() == type)
            list.push_back(obj.second->getName());
    for (auto& obj : _ghostObjects)
        if (obj.second->getType() == type)
            list.push_back(obj.second->getName());
    return list;
}

/*************/
vector<int> Scene::findGLVersion()
{
    vector<vector<int>> glVersionList {{4, 3}, {3, 3}, {3, 2}};
    vector<int> detectedVersion {0, 0};

    for (auto version : glVersionList)
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, version[0]);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, version[1]);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        #if HAVE_OSX
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        #endif
        glfwWindowHint(GLFW_SAMPLES, SPLASH_SAMPLES);
        glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);
        glfwWindowHint(GLFW_DEPTH_BITS, 24);
        glfwWindowHint(GLFW_VISIBLE, false);
        GLFWwindow* window = glfwCreateWindow(512, 512, "test_window", NULL, NULL);

        if (window)
        {
            detectedVersion = version;
            glfwDestroyWindow(window);
            break;
        }
    }

    return detectedVersion;
}

/*************/
void Scene::init(std::string name)
{
    glfwSetErrorCallback(Scene::glfwErrorCallback);

    // GLFW stuff
    if (!glfwInit())
    {
        Log::get() << Log::ERROR << "Scene::" << __FUNCTION__ << " - Unable to initialize GLFW" << Log::endl;
        _isInitialized = false;
        return;
    }

    auto glVersion = findGLVersion();
    if (glVersion[0] == 0)
    {
        Log::get() << Log::ERROR << "Scene::" << __FUNCTION__ << " - Unable to find a suitable GL version (higher than 3.2)" << Log::endl;
        _isInitialized = false;
        return;
    }

    _glVersion = glVersion;
    Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - GL version: " << glVersion[0] << "." << glVersion[1] << Log::endl;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, glVersion[0]);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, glVersion[1]);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef DEBUGGL
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, true);
#else
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, false);
#endif
    glfwWindowHint(GLFW_SAMPLES, SPLASH_SAMPLES);
    glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_VISIBLE, false);
#if HAVE_OSX
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(512, 512, name.c_str(), NULL, NULL);

    if (!window)
    {
        Log::get() << Log::WARNING << "Scene::" << __FUNCTION__ << " - Unable to create a GLFW window" << Log::endl;
        _isInitialized = false;
        return;
    }

    _mainWindow = make_shared<GlWindow>(window, window);
    _isInitialized = true;

    _mainWindow->setAsCurrentContext();
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    // Activate GL debug messages
#if not HAVE_OSX
#ifdef DEBUGGL
    glDebugMessageCallback(Scene::glMsgCallback, (void*)this);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM, 0, nullptr, GL_TRUE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, nullptr, GL_TRUE);
#endif
#endif

    // Check for swap groups
#if not HAVE_OSX
#ifdef GLX_NV_swap_group
    if (glfwExtensionSupported("GLX_NV_swap_group"))
    {
        PFNGLXQUERYMAXSWAPGROUPSNVPROC nvGLQueryMaxSwapGroups = (PFNGLXQUERYMAXSWAPGROUPSNVPROC)glfwGetProcAddress("glXQueryMaxSwapGroupsNV");
        if (!nvGLQueryMaxSwapGroups(glfwGetX11Display(), 0, &_maxSwapGroups, &_maxSwapBarriers))
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Unable to get NV max swap groups / barriers" << Log::endl;
        else
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - NV max swap groups: " << _maxSwapGroups << " / barriers: " << _maxSwapBarriers << Log::endl;
    }
#endif
#endif
    _mainWindow->releaseContext();

    _textureUploadWindow = getNewSharedWindow();

    // Create the link and connect to the World
    _link = make_shared<Link>(weak_ptr<Scene>(_self), name);
    _link->connectTo("world");
    sendMessageToWorld("sceneLaunched", {});
}

/*************/
void Scene::initBlendingMap()
{
    _blendingMap = make_shared<Image>(_self);
    _blendingMap->set(_blendingResolution, _blendingResolution, 1, ImageBufferSpec::Type::UINT16);
    _objects["blendingMap"] = _blendingMap;

    _blendingTexture = make_shared<Texture_Image>(_self);
    _blendingTexture->setAttribute("filtering", {0});
    *_blendingTexture = _blendingMap;
}

/*************/
void Scene::joystickUpdateLoop()
{
    while (_isRunning)
    {
        if (_isMaster && glfwJoystickPresent(GLFW_JOYSTICK_1))
        {
            int count;
            const float* bufferAxes = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &count);
            auto axes = vector<float>(bufferAxes, bufferAxes + count);

            for (auto& v : axes)
                if (abs(v) < 0.2f)
                    v = 0.f;

            const uint8_t* bufferButtons = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &count);
            auto buttons = vector<uint8_t>(bufferButtons, bufferButtons + count);

            lock_guard<mutex> lockJoystick(_joystickUpdateMutex);

            // We accumulate values until they are used by the render loop
            _joystickAxes.swap(axes);
            for (int i = 0; i < std::min(_joystickAxes.size(), axes.size()); ++i)
                _joystickAxes[i] += axes[i];

            _joystickButtons.swap(buttons);
        }

        this_thread::sleep_for(chrono::microseconds(16667));
    }
}

/*************/
void Scene::glfwErrorCallback(int code, const char* msg)
{
    Log::get() << Log::WARNING << "Scene::glfwErrorCallback - " << msg << Log::endl;
}

/*************/
#ifdef HAVE_OSX
void Scene::glMsgCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
#else
void Scene::glMsgCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, void* userParam)
#endif
{
    string typeString {""};
    Log::Priority logType;
    switch (type)
    {
    case GL_DEBUG_TYPE_ERROR:
        typeString = "Error";
        logType = Log::ERROR;
        break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        typeString = "Deprecated behavior";
        logType = Log::WARNING;
        break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        typeString = "Undefined behavior";
        logType = Log::ERROR;
        break;
    case GL_DEBUG_TYPE_PORTABILITY:
        typeString = "Portability";
        logType = Log::WARNING;
        break;
    case GL_DEBUG_TYPE_PERFORMANCE:
        typeString = "Performance";
        logType = Log::WARNING;
        break;
    case GL_DEBUG_TYPE_OTHER:
        typeString = "Other";
        logType = Log::MESSAGE;
        break;
    }

    Log::get() << logType << "GL::debug - [" << typeString << "] - " << message << Log::endl;
}

/*************/
void Scene::registerAttributes()
{
    addAttribute("add", [&](const Values& args) {
        addTask([=]() {
            string type = args[0].asString();
            string name = args[1].asString();
            add(type, name);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("add", "Add an object of the given name and type");

    addAttribute("addGhost", [&](const Values& args) {
        addTask([=]() {
            string type = args[0].asString();
            string name = args[1].asString();
            addGhost(type, name);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("addGhost", "Add a ghost object of the given name and type. Only useful in the master Scene");

    addAttribute("blendingResolution", [&](const Values& args) {
        addTask([=]() {
            int resolution = args[0].asInt();
            if (resolution >= 64)
                _blendingResolution = resolution;
        });

        return true;
    }, [&]() -> Values {
        return {(int)_blendingResolution};
    }, {'n'});
    setAttributeDescription("blendingResolution", "Set the resolution of the blending map");

    addAttribute("blendingUpdated", [&](const Values& args) {
        _vertexBlendingReceptionStatus = true;
        _vertexBlendingCondition.notify_one();
        return true;
    });
    setAttributeDescription("blendingUpdated", "Message sent by the master Scene to notify that a new blending has been computed");

    addAttribute("bufferUploaded", [&](const Values& args) {
        _textureUploadCondition.notify_all();
        return true;
    });
    setAttributeDescription("bufferUploaded", "Message sent by the World to notify that new textures have been sent");

    addAttribute("computeBlending", [&](const Values& args) {
        std::string blendingMode = args[0].asString();
        addTask([=]() -> void {
            computeBlendingMap(blendingMode);
        });
        return true;
    }, {'s'});
    setAttributeDescription("computeBlending", "Ask for blending computation. Parameter can be: once, continuous, or anything else to deactivate blending");

    addAttribute("activateBlendingMap", [&](const Values& args) {
        addTask([=]() -> void {
            activateBlendingMap();
        });

        return true;
    });
    setAttributeDescription("activateBlendingMap", "Activate the blending map");

    addAttribute("deactivateBlendingMap", [&](const Values& args) {
        addTask([=]() -> void {
            deactivateBlendingMap();
        });

        return true;
    });
    setAttributeDescription("deactivateBlendingMap", "Deactivate the blending map");

    addAttribute("config", [&](const Values& args) {
        addTask([&]() -> void {
            setlocale(LC_NUMERIC, "C"); // Needed to make sure numbers are written with commas
            Json::Value config = getConfigurationAsJson();
            string configStr = config.toStyledString();
            sendMessageToWorld("answerMessage", {"config", _name, configStr});
        });
        return true;
    });
    setAttributeDescription("config", "Ask the Scene for a JSON describing its configuration");

    addAttribute("deleteObject", [&](const Values& args) {
        addTask([=]() -> void {
            lock_guard<recursive_mutex> lockObjects(_objectsMutex);

            // We wait until we can indeed deleted the object
            bool expectedAtomicValue = false;
            while (!_objectsCurrentlyUpdated.compare_exchange_strong(expectedAtomicValue, true))
                this_thread::sleep_for(chrono::milliseconds(1));

            auto objectName = args[0].asString();

            auto objectIt = _objects.find(objectName);
            for (auto& localObject : _objects)
                unlink(objectIt->second, localObject.second);
            if (objectIt != _objects.end())
                _objects.erase(objectIt);

            objectIt = _ghostObjects.find(objectName);
            if (objectIt != _ghostObjects.end())
            {
                for (auto& ghostObject : _ghostObjects)
                    unlink(objectIt->second, ghostObject.second);
                _ghostObjects.erase(objectIt);
            }

            _objectsCurrentlyUpdated = false;
        });

        return true;
    }, {'s'});
    setAttributeDescription("deleteObject", "Delete an object given its name");

    addAttribute("duration", [&](const Values& args) {
        Timer::get().setDuration(args[0].asString(), args[1].asInt());
        return true;
    }, {'s', 'n'});
    setAttributeDescription("duration", "Set the duration of the given timer");

    addAttribute("masterClock", [&](const Values& args) {
        Timer::get().setMasterClock(args);
        return true;
    }, {'n', 'n', 'n', 'n', 'n', 'n', 'n'});
    setAttributeDescription("masterClock", "Set the timing of the master clock");
 
    addAttribute("flashBG", [&](const Values& args) {
        addTask([=]() {
            for (auto& obj : _objects)
                if (dynamic_pointer_cast<Camera>(obj.second).get() != nullptr)
                    dynamic_pointer_cast<Camera>(obj.second)->setAttribute("flashBG", {(int)(args[0].asInt())});
        });

        return true;
    }, {'n'});
    setAttributeDescription("flashBG", "Switches the background color from black to light grey");

    addAttribute("getObjectsNameByType", [&](const Values& args) {
        addTask([=]() {
            string type = args[0].asString();
            Values list = getObjectsNameByType(type);
            sendMessageToWorld("answerMessage", {"getObjectsNameByType", _name, list});
        });

        return true;
    }, {'s'});
    setAttributeDescription("getObjectsNameByType", "Get a list of the objects having the given type");

    addAttribute("httpServer", [&](const Values& args) {
        string address = args[0].asString();
        string port = args[1].asString();

        _httpServer = make_shared<HttpServer>(address, port, _self);
        if (_httpServer)
        {
            _httpServerFuture = async(std::launch::async, [&](){
                _httpServer->run();
            });
        }

        return true;
    }, [&]() -> Values {
        if (_httpServer)
            return {_httpServer->getAddress(), _httpServer->getPort()};
        else
            return {};
    }, {'s', 's'});
    setAttributeDescription("httpServer", "Create an HTTP server given its address and port");
   
    addAttribute("link", [&](const Values& args) {
        addTask([=]() {
            string src = args[0].asString();
            string dst = args[1].asString();
            link(src, dst);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("link", "Link the two given objects");

    addAttribute("linkGhost", [&](const Values& args) {
        addTask([=]() {
            string src = args[0].asString();
            string dst = args[1].asString();
            linkGhost(src, dst);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("linkGhost", "Link the two given ghost objects");

    addAttribute("log", [&](const Values& args) {
        Log::get().setLog(args[0].asString(), (Log::Priority)args[1].asInt());
        return true;
    }, {'s', 'n'});
    setAttributeDescription("log", "Add an entry to the logs, given its message and priority");

    addAttribute("ping", [&](const Values& args) {
        _textureUploadCondition.notify_all();
        sendMessageToWorld("pong", {_name});
        return true;
    });
    setAttributeDescription("ping", "Ping the World");

    addAttribute("remove", [&](const Values& args) {
        addTask([=]() {
            string name = args[1].asString();
            remove(name);
        });

        return true;
    }, {'s'});
    setAttributeDescription("remove", "Remove the object of the given name");

    addAttribute("renameObject", [&](const Values& args) {
        auto name = args[0].asString();
        auto newName = args[1].asString();

        addTask([=]() {
            lock_guard<recursive_mutex> lock(_objectsMutex);

            auto objIt = _objects.find(name);
            if (objIt != _objects.end())
            {
                auto object = objIt->second;
                object->setName(newName);
                _objects[newName] = object;
                _objects.erase(objIt);
            }
        });

        return true;
    }, {'s', 's'});

    addAttribute("setGhost", [&](const Values& args) {
        addTask([=]() {
            string name = args[0].asString();
            string attr = args[1].asString();
            Values values;
            for (int i = 2; i < args.size(); ++i)
                values.push_back(args[i]);

            if (_ghostObjects.find(name) != _ghostObjects.end())
                _ghostObjects[name]->setAttribute(attr, values);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("setGhost", "Set a given object the given attribute");

    addAttribute("setMaster", [&](const Values& args) {
        if (args.size() == 0)
            setAsMaster();
        else
            setAsMaster(args[0].asString());
        return true;
    });
    setAttributeDescription("setMaster", "Set this Scene as master, can give the configuration file path as a parameter");

    addAttribute("start", [&](const Values& args) {
        _started = true;
        sendMessageToWorld("answerMessage", {"start", _name});
        return true;
    });
    setAttributeDescription("start", "Start the Scene main loop");

    addAttribute("stop", [&](const Values& args) {
        _started = false;
        return true;
    });
    setAttributeDescription("stop", "Stop the Scene main loop");

    addAttribute("swapInterval", [&](const Values& args) {
        _swapInterval = max(-1, args[0].asInt());
        return true;
    }, [&]() -> Values {
        return {(int)_swapInterval};
    }, {'n'});
    setAttributeDescription("swapInterval", "Set the interval between two video frames. 1 is synced, 0 is not");

    addAttribute("swapTest", [&](const Values& args) {
        for (auto& obj : _objects)
            if (obj.second->getType() == "window")
                dynamic_pointer_cast<Window>(obj.second)->setAttribute("swapTest", args);
        return true;
    });
    setAttributeDescription("swapTest", "Activate video swap test if set to 1");

    addAttribute("swapTestColor", [&](const Values& args) {
        for (auto& obj : _objects)
            if (obj.second->getType() == "window")
                dynamic_pointer_cast<Window>(obj.second)->setAttribute("swapTestColor", args);
        return true;
    });
    setAttributeDescription("swapTestColor", "Set the swap test color");

    addAttribute("quit", [&](const Values& args) {
        addTask([=]() {
            _started = false;
            _isRunning = false;
        });
        return true;
    });
    setAttributeDescription("quit", "Ask the Scene to quit");
   
    addAttribute("unlink", [&](const Values& args) {
        addTask([=]() {
            string src = args[0].asString();
            string dst = args[1].asString();
            unlink(src, dst);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("unlink", "Unlink the two given objects");

    addAttribute("unlinkGhost", [&](const Values& args) {
        if (args.size() < 2)
            return false;

        addTask([=]() {
            string src = args[0].asString();
            string dst = args[1].asString();
            unlinkGhost(src, dst);
        });

        return true;
    }, {'s', 's'});
    setAttributeDescription("unlinkGhost", "Unlink the two given ghost objects");
 
    addAttribute("wireframe", [&](const Values& args) {
        addTask([=]() {
            for (auto& obj : _objects)
                if (obj.second->getType() == "camera")
                    dynamic_pointer_cast<Camera>(obj.second)->setAttribute("wireframe", {(int)(args[0].asInt())});
            for (auto& obj : _ghostObjects)
                if (obj.second->getType() == "camera")
                    dynamic_pointer_cast<Camera>(obj.second)->setAttribute("wireframe", {(int)(args[0].asInt())});
        });

        return true;
    }, {'n'});
    setAttributeDescription("wireframe", "Show all meshes as wireframes if set to 1");

#if HAVE_GPHOTO
    addAttribute("calibrateColor", [&](const Values& args) {
        if (_colorCalibrator == nullptr)
            return false;
        // This needs to be launched in another thread, as the set mutex is already locked
        // (and we will need it later)
        SThread::pool.enqueue([&]() {
            _colorCalibrator->update();
        });
        return true;
    });
    setAttributeDescription("calibrateColor", "Launch projectors color calibration");

    addAttribute("calibrateColorResponseFunction", [&](const Values& args) {
        if (_colorCalibrator == nullptr)
            return false;
        // This needs to be launched in another thread, as the set mutex is already locked
        // (and we will need it later)
        SThread::pool.enqueue([&]() {
            _colorCalibrator->updateCRF();
        });
        return true;
    });
    setAttributeDescription("calibrateColorResponseFunction", "Launch the camera color calibration");
#endif

}

} // end of namespace
