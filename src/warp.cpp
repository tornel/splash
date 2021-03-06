#include "warp.h"

#include <fstream>

#include "cgUtils.h"
#include "log.h"
#include "scene.h"
#include "timer.h"
#include "texture_image.h"

#define CONTROL_POINT_SCALE 0.02
#define WORLDMARKER_SCALE 0.0003
#define MARKER_SET {1.0, 0.5, 0.0, 1.0}

using namespace std;

namespace Splash {

/*************/
Warp::Warp(RootObjectWeakPtr root)
       : Texture(root)
{
    init();
}

/*************/
void Warp::init()
{
    _type = "warp";
    registerAttributes();

    // If the root object weak_ptr is expired, this means that
    // this object has been created outside of a World or Scene.
    // This is used for getting documentation "offline"
    if (_root.expired())
        return;

    // Intialize FBO, textures and everything OpenGL
    glGetError();
    glGenFramebuffers(1, &_fbo);

    setOutput();

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);
    GLenum _status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (_status != GL_FRAMEBUFFER_COMPLETE)
	{
        Log::get() << Log::WARNING << "Warp::" << __FUNCTION__ << " - Error while initializing framebuffer object: " << _status << Log::endl;
		return;
	}
    else
        Log::get() << Log::MESSAGE << "Warp::" << __FUNCTION__ << " - Framebuffer object successfully initialized" << Log::endl;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    GLenum error = glGetError();
    if (error)
    {
        Log::get() << Log::WARNING << "Warp::" << __FUNCTION__ << " - Error while binding framebuffer" << Log::endl;
        _isInitialized = false;
    }
    else
    {
        Log::get() << Log::MESSAGE << "Warp::" << __FUNCTION__ << " - Warp correctly initialized" << Log::endl;
        _isInitialized = true;
    }

    loadDefaultModels();
}

/*************/
Warp::~Warp()
{
    if (_root.expired())
        return;

#ifdef DEBUG
    Log::get()<< Log::DEBUGGING << "Warp::~Warp - Destructor" << Log::endl;
#endif

    glDeleteFramebuffers(1, &_fbo);
}

/*************/
void Warp::bind()
{
    _outTexture->bind();
}

/*************/
unordered_map<string, Values> Warp::getShaderUniforms() const
{
    unordered_map<string, Values> uniforms;
    return uniforms;
}

/*************/
bool Warp::linkTo(std::shared_ptr<BaseObject> obj)
{
    // Mandatory before trying to link
    if (!obj || !Texture::linkTo(obj))
        return false;

    if (dynamic_pointer_cast<Camera>(obj).get() != nullptr)
    {
        auto camera = _inCamera.lock();
        if (camera)
        {
            auto textures = camera->getTextures();
            for (auto& tex : textures)
                _screen->removeTexture(tex);
        }

        camera = dynamic_pointer_cast<Camera>(obj);
        auto textures = camera->getTextures();
        for (auto& tex : textures)
            _screen->addTexture(tex);
        _inCamera = camera;

        return true;
    }

    return true;
}

/*************/
void Warp::unbind()
{
    _outTexture->unbind();
}

/*************/
void Warp::unlinkFrom(std::shared_ptr<BaseObject> obj)
{
    if (dynamic_pointer_cast<Camera>(obj).get() != nullptr)
    {
        if (!_inCamera.expired())
        {
            auto inCamera = _inCamera.lock();
            auto camera = dynamic_pointer_cast<Camera>(obj);

            if (inCamera == camera)
            {
                auto textures = camera->getTextures();
                for (auto& tex : textures)
                    _screen->removeTexture(tex);

                if (camera->getName() == inCamera->getName())
                    _inCamera.reset();
            }
        }
    }

    Texture::unlinkFrom(obj);
}

/*************/
void Warp::update()
{
    if (_inCamera.expired())
        return;

    auto camera = _inCamera.lock();
    auto input = camera->getTextures()[0];

    _outTextureSpec = input->getSpec();
    _outTexture->resize(_outTextureSpec.width, _outTextureSpec.height);
    glViewport(0, 0, _outTextureSpec.width, _outTextureSpec.height);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);
    GLenum fboBuffers[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, fboBuffers);
    glDisable(GL_DEPTH_TEST);

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    _screen->activate();
    updateUniforms();
    _screen->draw();
    _screen->deactivate();

    if (_showControlPoints)
    {
        _screen->setAttribute("fill", {"warpControl"});
        _screenMesh->switchMeshes(true);

        _screen->activate();
        updateUniforms();
        _screen->draw();
        _screen->deactivate();

        _screen->setAttribute("fill", {"warp"});
        _screenMesh->switchMeshes(false);

        if (_selectedControlPointIndex != -1)
        {
            auto& pointModel = _models["3d_marker"];

            auto controlPoints = _screenMesh->getControlPoints();
            auto point = controlPoints[_selectedControlPointIndex];

            pointModel->setAttribute("position", {point.x, point.y, 0.f});
            pointModel->setAttribute("rotation", {0.f, 90.f, 0.f});
            pointModel->setAttribute("scale", {CONTROL_POINT_SCALE});
            pointModel->activate();
            pointModel->setViewProjectionMatrix(glm::dmat4(1.f), glm::dmat4(1.f));
            pointModel->draw();
            pointModel->deactivate();
        }
    }

    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    _outTexture->generateMipmap();
}

/*************/
void Warp::updateUniforms()
{
    auto shader = _screen->getShader();
}
/*************/
int Warp::pickControlPoint(glm::vec2 p, glm::vec2& v)
{
    float distance = numeric_limits<float>::max();
    glm::vec2 closestVertex;

    _screenMesh->switchMeshes(true);
    _screenMesh->update();

    auto vertices = _screenMesh->getControlPoints();
    int index = -1;
    for (int i = 0; i < vertices.size(); ++i)
    {
        float dist = glm::length(p - vertices[i]);
        if (dist < distance)
        {
            closestVertex = vertices[i];
            distance = dist;
            index = i;
        }
    }

    v = closestVertex;

    _screenMesh->switchMeshes(false);

    return index;
}

/*************/
void Warp::loadDefaultModels()
{
    map<string, string> files {{"3d_marker", "3d_marker.obj"}};
    
    for (auto& file : files)
    {
        if (!ifstream(file.second, ios::in | ios::binary))
        {
            if (ifstream(string(DATADIR) + file.second, ios::in | ios::binary))
                file.second = string(DATADIR) + file.second;
#if HAVE_OSX
            else if (ifstream("../Resources/" + file.second, ios::in | ios::binary))
                file.second = "../Resources/" + file.second;
#endif
            else
            {
                Log::get() << Log::WARNING << "Warp::" << __FUNCTION__ << " - File " << file.second << " does not seem to be readable." << Log::endl;
                continue;
            }
        }

        shared_ptr<Mesh> mesh = make_shared<Mesh>(_root);
        mesh->setName(file.first);
        mesh->setAttribute("file", {file.second});
        _modelMeshes.push_back(mesh);

        GeometryPtr geom = make_shared<Geometry>(_root);
        geom->setName(file.first);
        geom->linkTo(mesh);
        _modelGeometries.push_back(geom);

        shared_ptr<Object> obj = make_shared<Object>(_root);
        obj->setName(file.first);
        obj->setAttribute("scale", {WORLDMARKER_SCALE});
        obj->setAttribute("fill", {"color"});
        obj->setAttribute("color", MARKER_SET);
        obj->linkTo(geom);

        _models[file.first] = obj;
    }
}

/*************/
void Warp::setOutput()
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);

    _outTexture = make_shared<Texture_Image>(_root);
    _outTexture->reset(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _outTexture->getTexId(), 0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // Setup the virtual screen
    _screen = make_shared<Object>(_root);
    _screen->setAttribute("fill", {"warp"});
    GeometryPtr virtualScreen = make_shared<Geometry>(_root);
    _screenMesh = make_shared<Mesh_BezierPatch>(_root);
    virtualScreen->linkTo(_screenMesh);
    _screen->addGeometry(virtualScreen);
}

/*************/
void Warp::registerAttributes()
{
    addAttribute("patchControl", [&](const Values& args) {
        if (!_screenMesh)
            return false;
        return _screenMesh->setAttribute("patchControl", args);
    }, [&]() -> Values {
        if (!_screenMesh)
            return {};

        Values v;
        _screenMesh->getAttribute("patchControl", v);
        return v;
    });
    setAttributeDescription("patchControl", "Set the control points positions");

    addAttribute("patchResolution", [&](const Values& args) {
        if (!_screenMesh)
            return false;
        return _screenMesh->setAttribute("patchResolution", args);
    }, [&]() -> Values {
        if (!_screenMesh)
            return {};

        Values v;
        _screenMesh->getAttribute("patchResolution", v);
        return v;
    });
    setAttributeDescription("patchResolution", "Set the Bezier patch final resolution");

    addAttribute("patchSize", [&](const Values& args) {
        if (!_screenMesh)
            return false;
        return _screenMesh->setAttribute("patchSize", args);
    }, [&]() -> Values {
        if (!_screenMesh)
            return {};

        Values v;
        _screenMesh->getAttribute("patchSize", v);
        return v;
    });
    setAttributeDescription("patchSize", "Set the Bezier patch control resolution");

    // Show the Bezier patch describing the warp
    // Also resets the selected control point if hidden
    addAttribute("showControlLattice", [&](const Values& args) {
        _showControlPoints = args[0].asInt();
        if (!_showControlPoints)
            _selectedControlPointIndex = -1;
        return true;
    }, {'n'});
    setAttributeDescription("showControlLattice", "If set to 1, show the control lattice");

    // Show a single control point
    addAttribute("showControlPoint", [&](const Values& args) {
        auto index = args[0].asInt();
        if (index < 0 || index >= _screenMesh->getControlPoints().size())
            _selectedControlPointIndex = -1;
        else
            _selectedControlPointIndex = index;
        return true;
    }, {'n'});
    setAttributeDescription("showControlPoint", "Show the control point given its index");
}

} // end of namespace
