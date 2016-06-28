#include "camera.h"

#include "cgUtils.h"
#include "image.h"
#include "log.h"
#include "mesh.h"
#include "object.h"
#include "scene.h"
#include "shader.h"
#include "texture.h"
#include "texture_image.h"
#include "timer.h"
#include "threadpool.h"

#include <fstream>
#include <limits>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/simd_mat4.hpp>
#include <glm/gtx/simd_vec4.hpp>
#include <glm/gtx/vector_angle.hpp>

#define SCISSOR_WIDTH 8
#define WORLDMARKER_SCALE 0.0003
#define SCREENMARKER_SCALE 0.05
#define MARKER_SELECTED {0.9, 0.1, 0.1, 1.0}
#define SCREEN_MARKER_SELECTED {0.9, 0.3, 0.1, 1.0}
#define MARKER_ADDED {0.0, 0.5, 1.0, 1.0}
#define MARKER_SET {1.0, 0.5, 0.0, 1.0}
#define SCREEN_MARKER_SET {1.0, 0.7, 0.0, 1.0}
#define OBJECT_MARKER {0.1, 1.0, 0.2, 1.0}
#define CAMERA_FLASH_COLOR {0.6, 0.6, 0.6, 1.0}
#define DEFAULT_COLOR {0.2, 0.2, 1.0, 1.0}

using namespace std;
using namespace glm;

namespace Splash {

/*************/
Camera::Camera(RootObjectWeakPtr root)
       : BaseObject(root)
{
    init();
}

/*************/
void Camera::init()
{
    _type = "camera";
    registerAttributes();

    // If the root object weak_ptr is expired, this means that
    // this object has been created outside of a World or Scene.
    // This is used for getting documentation "offline"
    if (_root.expired())
        return;

    // Intialize FBO, textures and everything OpenGL
    glGetError();
    glGenFramebuffers(1, &_fbo);

    setOutputNbr(1);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);
    GLenum _status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (_status != GL_FRAMEBUFFER_COMPLETE)
	{
        Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - Error while initializing framebuffer object: " << _status << Log::endl;
		return;
	}
    else
        Log::get() << Log::MESSAGE << "Camera::" << __FUNCTION__ << " - Framebuffer object successfully initialized" << Log::endl;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    GLenum error = glGetError();
    if (error)
    {
        Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - Error while binding framebuffer" << Log::endl;
        _isInitialized = false;
    }
    else
    {
        Log::get() << Log::MESSAGE << "Camera::" << __FUNCTION__ << " - Camera correctly initialized" << Log::endl;
        _isInitialized = true;
    }

    // Load some models
    loadDefaultModels();
}

/*************/
Camera::~Camera()
{
#ifdef DEBUG
    Log::get()<< Log::DEBUGGING << "Camera::~Camera - Destructor" << Log::endl;
#endif

    if (!_root.expired())
        glDeleteFramebuffers(1, &_fbo);
}

/*************/
void Camera::computeBlendingMap(ImagePtr& map)
{
    if (map->getSpec().type != ImageBufferSpec::Type::UINT16)
    {
        Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - Input map is not of type UINT16." << Log::endl;
        return;
    }

    // We want to render the object with a specific texture, containing texture coordinates
    vector<Values> shaderFill;
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        Values fill;
        obj->getAttribute("fill", fill);
        obj->setAttribute("fill", {"uv"});
        shaderFill.push_back(fill);
    }

    // We do a "normal" render to ensure everything is correctly set
    // and that no state change is waiting
    render();

    // Increase the render size for more precision
    int width = _width;
    int height = _height;
    int dims[2];
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, dims);
    if (width >= height)
        dims[1] = dims[0] * height / width;
    else
        dims[0] = dims[1] * width / height;

    setOutputSize(dims[0] / 4, dims[1] / 4);

    // Render with the current texture, with no marker or frame
    bool drawFrame = _drawFrame;
    bool displayCalibration = _displayCalibration;
    _drawFrame = _displayCalibration = false;
    render();
    _drawFrame = drawFrame;
    _displayCalibration = displayCalibration;

#ifdef DEBUG
    GLenum error = glGetError();
#endif
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _fbo);
    ImageBuffer img(_outTextures[0]->getSpec());
    glReadPixels(0, 0, img.getSpec().width, img.getSpec().height, GL_RGBA, GL_UNSIGNED_SHORT, img.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // Reset the objects to their initial shader
    int fillIndex {0};
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        obj->setAttribute("fill", shaderFill[fillIndex]);
        fillIndex++;
    }
#ifdef DEBUG
    error = glGetError();
    if (error)
        Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - Error while computing the blending map : " << error << Log::endl;
#endif

    setOutputSize(width, height);

    // Go through the rendered image, fill the map with the "used" pixels from the original texture
    ImageBufferSpec mapSpec = map->getSpec();
    vector<unsigned short> camMap(mapSpec.width * mapSpec.height, 0);
    vector<bool> isSet(mapSpec.width * mapSpec.height); // If a pixel is detected for this camera, only note it once

    uint16_t* imgPtr = reinterpret_cast<uint16_t*>(img.data());
    uint16_t* imageMap = (uint16_t*)map->data();
    
    for (int y = 0; y < img.getSpec().height; ++y)
        for (int x = 0; x < img.getSpec().width; ++x)
        {
            uint16_t* pixel = &imgPtr[(x + y * img.getSpec().width) * 4];
            // UV coordinates are mapped on 2 uchar each
            int destX = (int)floor((pixel[0] + pixel[1] / 256.0) * 0.00001525878906250 * (double)mapSpec.width);
            int destY = (int)floor((pixel[2] + pixel[3] / 256.0) * 0.00001525878906250 * (double)mapSpec.height);

            if (isSet[destY * mapSpec.width + destX] || (destX == 0 && destY == 0))
                continue;
            isSet[destY * mapSpec.width + destX] = true;

            // Blending is computed as by Lancelle et al. 2011, "Soft Edge and Soft Corner Blending"
            double distX = (double)std::min((double)x, (double)img.getSpec().width - 1 - x) / (double)img.getSpec().width / _blendWidth;
            double distY = (double)std::min((double)y, (double)img.getSpec().height - 1 - y) / (double)img.getSpec().height / _blendWidth;
            distX = glm::clamp(distX, 0.0, 1.0);
            distY = glm::clamp(distY, 0.0, 1.0);
            
            unsigned short blendAddition = 0;
            if (_blendWidth > 0.f)
            {
                // Add some smoothness to the transition
                double weight = 1.0 / (1.0 / distX + 1.0 / distY);
                double smoothDist = pow(std::min(std::max(weight, 0.0), 1.0), 2.0) * 256.0;
                int blendValue = smoothDist;
                blendAddition += blendValue; // One more camera displaying this pixel
            }
            else
            {
                blendAddition += 256; // One more camera displaying this pixel
            }

            // We keep the real number of projectors, hidden higher in the shorts
            blendAddition += 4096;
            camMap[destY * mapSpec.width + destX] = blendAddition;
        }

    // Fill the holes
    for (unsigned int y = 0; y < mapSpec.height; ++y)
    {
        unsigned short lastFilledPixel = 0;
        unsigned short nextFilledPixel = 0;
        unsigned int holeStart = 0;
        unsigned int holeEnd = 0;
        bool hole = false;

        for (unsigned int x = 0; x < mapSpec.width; ++x)
        {
            // If we have not yet found a filled pixel
            if (isSet[y * mapSpec.width + x] == false && !hole)
                continue;
            // If this is the first filled pixel (beginning of a hole)
            else if (isSet[y * mapSpec.width + x] == true && !hole)
            {
                // Check if the pixel right next to it is not set too
                if (x < mapSpec.width - 1 && isSet[y * mapSpec.width + x + 1] == true)
                    continue;

                // It is indeed a hole: find its end
                lastFilledPixel = camMap[y * mapSpec.width + x];
                holeStart = x;
                for (unsigned int xx = x + 2; xx < mapSpec.width; ++xx)
                {
                    if (isSet[y * mapSpec.width + xx] == true)
                    {
                        nextFilledPixel = camMap[y * mapSpec.width + xx];
                        holeEnd = xx;
                        hole = true;
                    }
                }
                continue;
            }
            else if (isSet[y * mapSpec.width + x] == true && hole)
            {
                hole = false;
                x -= 1; // Go back one pixel, to detect the next hole
                continue;
            }
            
            // We have the beginning, the end and the size of the hole
            unsigned short step = ((int)nextFilledPixel - (int)lastFilledPixel) * (int)(x - holeStart) / (int)(holeEnd - holeStart);
            unsigned short pixelValue = lastFilledPixel + step;
            camMap[y * mapSpec.width + x] = pixelValue;
            isSet[y * mapSpec.width + x] = true;
        }
    }

    // Add this camera's contribution to the blending map
    for (unsigned int y = 0; y < mapSpec.height; ++y)
        for (unsigned int x = 0; x < mapSpec.width; ++x)
            imageMap[y + mapSpec.width * x] += camMap[y + mapSpec.width * x];
}

/*************/
void Camera::computeBlendingContribution()
{
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        obj->computeCameraContribution(computeViewMatrix(), computeProjectionMatrix(), _blendWidth);
    }
}

/*************/
void Camera::computeVertexVisibility()
{
    // We want to render the object with a specific texture, containing the primitive IDs
    vector<Values> shaderFill;
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();
        obj->resetVisibility();

        Values fill;
        obj->getAttribute("fill", fill);
        obj->setAttribute("fill", {"primitiveId"});
        shaderFill.push_back(fill);
    }

    // Render with the current texture, with no marker or frame
    bool drawFrame = _drawFrame;
    bool displayCalibration = _displayCalibration;
    _drawFrame = _displayCalibration = false;
    render();
    _drawFrame = drawFrame;
    _displayCalibration = displayCalibration;

    // Reset the objects to their initial shader
    int fillIndex {0};
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        obj->setAttribute("fill", shaderFill[fillIndex]);
        fillIndex++;
    }

    // Update the vertices visibility based on the result
    glActiveTexture(GL_TEXTURE0);
    _outTextures[0]->bind();
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        obj->transferVisibilityFromTexToAttr(_width, _height);
    }
    _outTextures[0]->unbind();
}

/*************/
void Camera::blendingTessellateForCurrentCamera()
{
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        obj->tessellateForThisCamera(computeViewMatrix(), computeProjectionMatrix(), _blendWidth, _blendPrecision);
    }
}

/*************/
bool Camera::doCalibration()
{
    int pointsSet = 0;
    for (auto& point : _calibrationPoints)
        if (point.isSet)
            pointsSet++;
    // We need at least 7 points to get a meaningful calibration
    if (pointsSet < 6)
    {
        Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - Calibration needs at least 6 points" << Log::endl;
        return false;
    }
    else if (pointsSet < 7)
    {
        Log::get() << Log::MESSAGE << "Camera::" << __FUNCTION__ << " - For better calibration results, use at least 7 points" << Log::endl;
    }

    _calibrationCalledOnce = true;

    gsl_multimin_function calibrationFunc;
    calibrationFunc.n = 9;
    calibrationFunc.f = &Camera::cameraCalibration_f;
    calibrationFunc.params = (void*)this;

    Log::get() << "Camera::" << __FUNCTION__ << " - Starting calibration..." << Log::endl;

    const gsl_multimin_fminimizer_type* minimizerType;
    minimizerType = gsl_multimin_fminimizer_nmsimplex2rand;

    // Variables we do not want to keep between tries
    dvec3 eyeOriginal = _eye;
    float fovOriginal = _fov;

    double minValue = numeric_limits<double>::max();
    vector<double> selectedValues(9);

    mutex gslMutex;
    vector<unsigned int> threadIds;
    // First step: we try a bunch of starts and keep the best one
    for (int index = 0; index < 4; ++index)
    {
        threadIds.push_back(SThread::pool.enqueue([&]() {
            gsl_multimin_fminimizer* minimizer;
            minimizer = gsl_multimin_fminimizer_alloc(minimizerType, 9);

            for (double s = 0.0; s <= 1.0; s += 0.2)
            for (double t = 0.0; t <= 1.0; t += 0.2)
            {
                gsl_vector* step = gsl_vector_alloc(9);
                gsl_vector_set(step, 0, 10.0);
                gsl_vector_set(step, 1, 0.1);
                gsl_vector_set(step, 2, 0.1);
                for (int i = 3; i < 9; ++i)
                    gsl_vector_set(step, i, 0.1);

                gsl_vector* x = gsl_vector_alloc(9);
                gsl_vector_set(x, 0, 35.0 + ((float)rand() / RAND_MAX * 2.0 - 1.0) * 16.0);
                gsl_vector_set(x, 1, s);
                gsl_vector_set(x, 2, t);
                for (int i = 0; i < 3; ++i)
                {
                    gsl_vector_set(x, i + 3, eyeOriginal[i]);
                    gsl_vector_set(x, i + 6, (float)rand() / RAND_MAX * 360.f);
                }

                gsl_multimin_fminimizer_set(minimizer, &calibrationFunc, x, step);

                size_t iter = 0;
                int status = GSL_CONTINUE;
                double localMinimum = numeric_limits<double>::max();
                while(status == GSL_CONTINUE && iter < 10000 && localMinimum > 0.5)
                {
                    iter++;
                    status = gsl_multimin_fminimizer_iterate(minimizer);
                    if (status)
                    {
                        Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - An error has occured during minimization" << Log::endl;
                        break;
                    }

                    status = gsl_multimin_test_size(minimizer->size, 1e-6);
                    localMinimum = gsl_multimin_fminimizer_minimum(minimizer);
                }

                lock_guard<mutex> lock(gslMutex);
                if (localMinimum < minValue)
                {
                    minValue = localMinimum;
                    for (int i = 0; i < 9; ++i)
                        selectedValues[i] = gsl_vector_get(minimizer->x, i);
                }

                gsl_vector_free(x);
                gsl_vector_free(step);
            }

            gsl_multimin_fminimizer_free(minimizer);
        }));
    }
    SThread::pool.waitThreads(threadIds);

    // Second step: we improve on the best result from the previous step
    for (int index = 0; index < 8; ++index)
    {
        gsl_multimin_fminimizer* minimizer;
        minimizer = gsl_multimin_fminimizer_alloc(minimizerType, 9);

        gsl_vector* step = gsl_vector_alloc(9);
        gsl_vector_set(step, 0, 1.0);
        gsl_vector_set(step, 1, 0.05);
        gsl_vector_set(step, 2, 0.05);
        for (int i = 3; i < 9; ++i)
            gsl_vector_set(step, i, 0.01);

        gsl_vector* x = gsl_vector_alloc(9);
        for (int i = 0; i < 9; ++i)
            gsl_vector_set(x, i, selectedValues[i]);

        gsl_multimin_fminimizer_set(minimizer, &calibrationFunc, x, step);

        size_t iter = 0;
        int status = GSL_CONTINUE;
        double localMinimum = numeric_limits<double>::max();
        while(status == GSL_CONTINUE && iter < 10000 && localMinimum > 0.5)
        {
            iter++;
            status = gsl_multimin_fminimizer_iterate(minimizer);
            if (status)
            {
                Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - An error has occured during minimization" << Log::endl;
                break;
            }

            status = gsl_multimin_test_size(minimizer->size, 1e-6);
            localMinimum = gsl_multimin_fminimizer_minimum(minimizer);
        }

        lock_guard<mutex> lock(gslMutex);
        if (localMinimum < minValue)
        {
            minValue = localMinimum;
            for (int i = 0; i < 9; ++i)
                selectedValues[i] = gsl_vector_get(minimizer->x, i);
        }

        gsl_vector_free(x);
        gsl_vector_free(step);
        gsl_multimin_fminimizer_free(minimizer);
    }

    if (minValue > 1000.0)
    {
        Log::get() << "Camera::" << __FUNCTION__ << " - Minumum found at (fov, cx, cy): " << selectedValues[0] << " " << selectedValues[1] << " " << selectedValues[2] << Log::endl;
        Log::get() << "Camera::" << __FUNCTION__ << " - Minimum value: " << minValue << Log::endl;
        Log::get() << "Camera::" << __FUNCTION__ << " - Calibration not set because the found parameters are not good enough." << Log::endl;
    }
    else
    {
        // Third step: convert the values to camera parameters
        if (!operator[]("fov").isLocked())
            _fov = selectedValues[0];
        if (!operator[]("principalPoint").isLocked())
        {
            _cx = selectedValues[1];
            _cy = selectedValues[2];
        }

        dvec3 euler;
        for (int i = 0; i < 3; ++i)
        {
            _eye[i] = selectedValues[i + 3];
            euler[i] = selectedValues[i + 6];
        }
        dmat4 rotateMat = yawPitchRoll(euler[0], euler[1], euler[2]);
        dvec4 target = rotateMat * dvec4(1.0, 0.0, 0.0, 0.0);
        dvec4 up = rotateMat * dvec4(0.0, 0.0, 1.0, 0.0);
        for (int i = 0; i < 3; ++i)
        {
            _target[i] = target[i];
            _up[i] = up[i];
        }
        _target += _eye;
        _up = normalize(_up);

        Log::get() << "Camera::" << __FUNCTION__ << " - Minumum found at (fov, cx, cy): " << _fov << " " << _cx << " " << _cy << Log::endl;
        Log::get() << "Camera::" << __FUNCTION__ << " - Minimum value: " << minValue << Log::endl;

        // Force camera update with the new parameters
        _updatedParams = true;
    }

    return true;
}

/*************/
void Camera::drawModelOnce(const std::string& modelName, const glm::dmat4& rtMatrix)
{
    _drawables.push_back(Drawable(modelName, rtMatrix));
}

/*************/
bool Camera::linkTo(shared_ptr<BaseObject> obj)
{
    // Mandatory before trying to link
    if (!BaseObject::linkTo(obj))
        return false;

    if (dynamic_pointer_cast<Object>(obj).get() != nullptr)
    {
        ObjectPtr obj3D = dynamic_pointer_cast<Object>(obj);
        _objects.push_back(obj3D);

        sendCalibrationPointsToObjects();
        return true;
    }

    return false;
}

/*************/
void Camera::unlinkFrom(shared_ptr<BaseObject> obj)
{
    auto objIterator = find_if(_objects.begin(), _objects.end(), [&](const std::weak_ptr<Object> o) {
        if (o.expired())
            return false;
        auto object = o.lock();
        if (object == obj)
            return true;
        return false;
    });

    if (objIterator != _objects.end())
        _objects.erase(objIterator);

    BaseObject::unlinkFrom(obj);
}

/*************/
Values Camera::pickVertex(float x, float y)
{
    // Convert the normalized coordinates ([0, 1]) to pixel coordinates
    float realX = x * _width;
    float realY = y * _height;

    // Get the depth at the given point
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _fbo);
    float depth;
    glReadPixels(realX, realY, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    if (depth == 1.f)
        return Values();

    // Unproject the point
    dvec3 screenPoint(realX, realY, depth);

    float distance = numeric_limits<float>::max();
    dvec4 vertex;
    for (auto& o : _objects)
    {
        if (o.expired())
            continue;
        auto obj = o.lock();

        dvec3 point = unProject(screenPoint, lookAt(_eye, _target, _up) * obj->getModelMatrix(),
                               computeProjectionMatrix(), dvec4(0, 0, _width, _height));
        glm::dvec3 closestVertex;
        float tmpDist;
        if ((tmpDist = obj->pickVertex(point, closestVertex)) < distance)
        {
            distance = tmpDist;
            vertex = obj->getModelMatrix() * dvec4(closestVertex, 1.0);
        }
    }

    return {vertex.x, vertex.y, vertex.z};
}

/*************/
Values Camera::pickFragment(float x, float y, float& fragDepth)
{
    // Convert the normalized coordinates ([0, 1]) to pixel coordinates
    float realX = x * _width;
    float realY = y * _height;

    // Get the depth at the given point
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _fbo);
    float depth;
    glReadPixels(realX, realY, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    if (depth == 1.f)
        return Values();

    // Unproject the point in world coordinates
    dvec3 screenPoint(realX, realY, depth);
    dvec3 point = unProject(screenPoint, lookAt(_eye, _target, _up), computeProjectionMatrix(), dvec4(0, 0, _width, _height));

    fragDepth = (lookAt(_eye, _target, _up) * dvec4(point.x, point.y, point.z, 1.0)).z;
    return {point.x, point.y, point.z};
}

/*************/
Values Camera::pickCalibrationPoint(float x, float y)
{
    dvec3 screenPoint(x * _width, y * _height, 0.0);

    dmat4 lookM = lookAt(_eye, _target, _up);
    dmat4 projM = computeProjectionMatrix(_fov, _cx, _cy);
    dvec4 viewport(0, 0, _width, _height);

    double minDist = numeric_limits<double>::max();
    int index = -1;

    for (int i = 0; i < _calibrationPoints.size(); ++i)
    {
        dvec3 projectedPoint = project(_calibrationPoints[i].world, lookM, projM, viewport);
        projectedPoint.z = 0.0;
        if (length(projectedPoint - screenPoint) < minDist)
        {
            minDist = length(projectedPoint - screenPoint);
            index = i;
        }
    }

    if (index != -1)
    {
        dvec3 vertex = _calibrationPoints[index].world;
        return {vertex[0], vertex[1], vertex[2]};
    }
    else
        return Values();
}

/*************/
Values Camera::pickVertexOrCalibrationPoint(float x, float y)
{
    Values vertex = pickVertex(x, y);
    Values point = pickCalibrationPoint(x, y);

    dvec3 screenPoint(x * _width, y * _height, 0.0);

    dmat4 lookM = lookAt(_eye, _target, _up);
    dmat4 projM = computeProjectionMatrix(_fov, _cx, _cy);
    dvec4 viewport(0, 0, _width, _height);

    if (vertex.size() == 0 && point.size() == 0)
        return Values();
    else if (vertex.size() == 0)
        return point;
    else if (point.size() == 0)
        return vertex;
    else
    {
        double vertexDist = length(screenPoint - project(dvec3(vertex[0].asFloat(), vertex[1].asFloat(), vertex[2].asFloat()), lookM, projM, viewport));
        double pointDist = length(screenPoint - project(dvec3(point[0].asFloat(), point[1].asFloat(), point[2].asFloat()), lookM, projM, viewport));

        if (pointDist <= vertexDist)
            return point;
        else
            return vertex;
    }
}

/*************/
bool Camera::render()
{
    if (_updateColorDepth)
        updateColorDepth();

    if (_newWidth != 0 && _newHeight != 0)
    {
        setOutputSize(_newWidth, _newHeight);
        _newWidth = 0;
        _newHeight = 0;
    }

    ImageBufferSpec spec = _outTextures[0]->getSpec();
    if (spec.width != _width || spec.height != _height)
        setOutputSize(spec.width, spec.height);

    if (_outTextures.size() < 1)
        return false;

#ifdef DEBUG
    glGetError();
#endif
    glViewport(0, 0, _width, _height);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);
    GLenum fboBuffers[_outTextures.size()];
    for (int i = 0; i < _outTextures.size(); ++i)
        fboBuffers[i] = GL_COLOR_ATTACHMENT0 + i;
    glDrawBuffers(_outTextures.size(), fboBuffers);
    glEnable(GL_DEPTH_TEST);

    if (_drawFrame)
    {
        glClearColor(1.0, 0.5, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(SCISSOR_WIDTH, SCISSOR_WIDTH, _width - SCISSOR_WIDTH * 2, _height - SCISSOR_WIDTH * 2);
    }

    if (_flashBG)
        glClearColor(_clearColor.r, _clearColor.g, _clearColor.b, _clearColor.a);
    else
        glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!_hidden)
    {
        // Draw the objects
        for (auto& o : _objects)
        {
            if (o.expired())
                continue;
            auto obj = o.lock();

            obj->activate();
            vec2 colorBalance = colorBalanceFromTemperature(_colorTemperature);
            obj->getShader()->setAttribute("uniform", {"_cameraAttributes", _blendWidth, _brightness});
            obj->getShader()->setAttribute("uniform", {"_fovAndColorBalance", _fov * _width / _height * M_PI / 180.0, _fov * M_PI / 180.0, colorBalance.x, colorBalance.y});
            if (_colorLUT.size() == 768 && _isColorLUTActivated)
            {
                obj->getShader()->setAttribute("uniform", {"_colorLUT", _colorLUT});
                obj->getShader()->setAttribute("uniform", {"_isColorLUT", 1});

                Values m(10);
                m[0] = "_colorMixMatrix";
                for (int u = 0; u < 3; ++u)
                    for (int v = 0; v < 3; ++v)
                        m[u*3 + v + 1] = _colorMixMatrix[u][v];
                obj->getShader()->setAttribute("uniform", m);
            }
            else
            {
                obj->getShader()->setAttribute("uniform", {"_isColorLUT", 0});
            }


            obj->setViewProjectionMatrix(computeViewMatrix(), computeProjectionMatrix());
            obj->draw();
            obj->deactivate();
        }

        auto viewMatrix = computeViewMatrix();
        auto projectionMatrix = computeProjectionMatrix();

        // Draw the calibrations points of all the cameras
        if (_displayAllCalibrations)
        {
            for (auto& objWeakPtr : _objects)
            {
                auto object = objWeakPtr.lock();
                auto points = object->getCalibrationPoints();

                auto& worldMarker = _models["3d_marker"];

                for (auto& point : points)
                {
                    glm::dvec4 transformedPoint = projectionMatrix * viewMatrix * glm::dvec4(point.x, point.y, point.z, 1.0);
                    worldMarker->setAttribute("scale", {WORLDMARKER_SCALE * 0.66 * std::max(transformedPoint.z, 1.0) * _fov});
                    worldMarker->setAttribute("position", {point.x, point.y, point.z});
                    worldMarker->setAttribute("color", OBJECT_MARKER);

                    worldMarker->activate();
                    worldMarker->setViewProjectionMatrix(viewMatrix, projectionMatrix);
                    worldMarker->draw();
                    worldMarker->deactivate();
                }
            }
        }

        // Draw the calibration points
        if (_displayCalibration)
        {
            auto& worldMarker = _models["3d_marker"];
            auto& screenMarker = _models["2d_marker"];

            for (int i = 0; i < _calibrationPoints.size(); ++i)
            {
                auto& point = _calibrationPoints[i];

                worldMarker->setAttribute("position", {point.world.x, point.world.y, point.world.z});
                glm::dvec4 transformedPoint = projectionMatrix * viewMatrix * glm::dvec4(point.world.x, point.world.y, point.world.z, 1.0);
                worldMarker->setAttribute("scale", {WORLDMARKER_SCALE * std::max(transformedPoint.z, 1.0) * _fov});
                if (_selectedCalibrationPoint == i)
                    worldMarker->setAttribute("color", MARKER_SELECTED);
                else if (point.isSet)
                    worldMarker->setAttribute("color", MARKER_SET);
                else
                    worldMarker->setAttribute("color", MARKER_ADDED);

                worldMarker->activate();
                worldMarker->setViewProjectionMatrix(viewMatrix, projectionMatrix);
                worldMarker->draw();
                worldMarker->deactivate();

                if ((point.isSet && _selectedCalibrationPoint == i) || _showAllCalibrationPoints) // Draw the target position on screen as well
                {

                    screenMarker->setAttribute("position", {point.screen.x, point.screen.y, 0.f});
                    screenMarker->setAttribute("scale", {SCREENMARKER_SCALE});
                    if (_selectedCalibrationPoint == i)
                        screenMarker->setAttribute("color", SCREEN_MARKER_SELECTED);
                    else
                        screenMarker->setAttribute("color", SCREEN_MARKER_SET);

                    screenMarker->activate();
                    screenMarker->setViewProjectionMatrix(dmat4(1.f), dmat4(1.f));
                    screenMarker->draw();
                    screenMarker->deactivate();
                }
            }
        }

        // Draw the additionals objects
        for (auto& object : _drawables)
        {
            auto modelIt = _models.find(object.model);
            if (modelIt != _models.end())
            {
                auto& model = modelIt->second;
                auto rtMatrix = glm::inverse(object.rtMatrix);

                auto position = glm::column(rtMatrix, 3);
                glm::dvec4 transformedPoint = projectionMatrix * viewMatrix * position;

                model->setAttribute("scale", {0.01 * std::max(transformedPoint.z, 1.0) * _fov});
                model->setAttribute("color", DEFAULT_COLOR);
                model->setModelMatrix(rtMatrix);

                model->activate();
                model->setViewProjectionMatrix(viewMatrix, projectionMatrix);
                model->draw();
                model->deactivate();
            }
        }
        _drawables.clear();
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

#ifdef DEBUG
    GLenum error = glGetError();
    if (error)
        Log::get() << Log::WARNING << _type << "::" << __FUNCTION__ << " - Error while rendering the camera: " << error << Log::endl;
    return error != 0 ? true : false;
#else
    return false;
#endif
}

/*************/
bool Camera::addCalibrationPoint(const Values& worldPoint)
{
    if (worldPoint.size() < 3)
        return false;

    dvec3 world(worldPoint[0].asFloat(), worldPoint[1].asFloat(), worldPoint[2].asFloat());

    // Check if the point is already present
    for (int i = 0; i < _calibrationPoints.size(); ++i)
        if (_calibrationPoints[i].world == world)
        {
                _selectedCalibrationPoint = i;
                return true;
        }

    _calibrationPoints.push_back(CalibrationPoint(world));
    _selectedCalibrationPoint = _calibrationPoints.size() - 1;

    // Set the point as calibrated in all linked objects
    for (auto& objWeakPtr : _objects)
    {
        auto object = objWeakPtr.lock();
        object->addCalibrationPoint(world);
    }

    return true;
}

/*************/
void Camera::deselectCalibrationPoint()
{
    _selectedCalibrationPoint = -1;
}

/*************/
void Camera::moveCalibrationPoint(float dx, float dy)
{
    if (_selectedCalibrationPoint == -1)
        return;

    _calibrationPoints[_selectedCalibrationPoint].screen.x += dx / _width;
    _calibrationPoints[_selectedCalibrationPoint].screen.y += dy / _height;
    _calibrationPoints[_selectedCalibrationPoint].isSet = true;

    float screenX = 0.5f + 0.5f * _calibrationPoints[_selectedCalibrationPoint].screen.x;
    float screenY = 0.5f + 0.5f *_calibrationPoints[_selectedCalibrationPoint].screen.y;

    auto distanceToBorder = std::min(screenX, std::min(screenY, std::min(1.f - screenX, 1.f - screenY)));
    _calibrationPoints[_selectedCalibrationPoint].weight = 1.f - distanceToBorder;

    if (_calibrationCalledOnce)
        doCalibration();
}

/*************/
void Camera::removeCalibrationPoint(const Values& point, bool unlessSet)
{
    if (point.size() == 2)
    {
        dvec3 screenPoint(point[0].asFloat(), point[1].asFloat(), 0.0);

        dmat4 lookM = lookAt(_eye, _target, _up);
        dmat4 projM = computeProjectionMatrix(_fov, _cx, _cy);
        dvec4 viewport(0, 0, _width, _height);

        double minDist = numeric_limits<double>::max();
        int index = -1;

        for (int i = 0; i < _calibrationPoints.size(); ++i)
        {
            dvec3 projectedPoint = project(_calibrationPoints[i].world, lookM, projM, viewport);
            projectedPoint.z = 0.0;
            if (length(projectedPoint - screenPoint) < minDist)
            {
                minDist = length(projectedPoint - screenPoint);
                index = i;
            }
        }

        if (index != -1)
        {
            // Set the point as uncalibrated from all linked objects
            for (auto& objWeakPtr : _objects)
            {
                auto object = objWeakPtr.lock();
                auto pointAsValues = Values({_calibrationPoints[index].world.x, _calibrationPoints[index].world.y, _calibrationPoints[index].world.z});
                object->removeCalibrationPoint(_calibrationPoints[index].world);
            }

            _calibrationPoints.erase(_calibrationPoints.begin() + index);
            _calibrationCalledOnce = false;
        }
    }
    else if (point.size() == 3)
    {
        dvec3 world(point[0].asFloat(), point[1].asFloat(), point[2].asFloat());

        for (int i = 0; i < _calibrationPoints.size(); ++i)
            if (_calibrationPoints[i].world == world)
            {
                if (_calibrationPoints[i].isSet == true && unlessSet)
                    continue;

                // Set the point as uncalibrated from all linked objects
                for (auto& objWeakPtr : _objects)
                {
                    auto object = objWeakPtr.lock();
                    object->removeCalibrationPoint(world);
                }

                _calibrationPoints.erase(_calibrationPoints.begin() + i);
                _selectedCalibrationPoint = -1;
            }

        _calibrationCalledOnce = false;
    }
}

/*************/
bool Camera::setCalibrationPoint(const Values& screenPoint)
{
    if (_selectedCalibrationPoint == -1)
        return false;

    _calibrationPoints[_selectedCalibrationPoint].screen = glm::dvec2(screenPoint[0].asFloat(), screenPoint[1].asFloat());
    _calibrationPoints[_selectedCalibrationPoint].isSet = true;

    _calibrationCalledOnce = false;

    return true;
}

/*************/
void Camera::setOutputNbr(int nbr)
{
    if (nbr < 1 || nbr == _outTextures.size())
        return;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);

    if (!_depthTexture)
    {
        _depthTexture = make_shared<Texture_Image>(_root, GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 512, 512, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _depthTexture->getTexId(), 0);
    }

    if (nbr < _outTextures.size())
    {
        for (int i = nbr; i < _outTextures.size(); ++i)
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);

        _outTextures.resize(nbr);
    }
    else
    {
        for (int i = _outTextures.size(); i < nbr; ++i)
        {
            Texture_ImagePtr texture = make_shared<Texture_Image>(_root);
            texture->setAttribute("clampToEdge", {1});
            texture->setAttribute("filtering", {0});
            texture->reset(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, texture->getTexId(), 0);
            _outTextures.push_back(texture);
        }
    }

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

/*************/
void Camera::updateColorDepth()
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _fbo);

    for (int i = 0; i < _outTextures.size(); ++i)
    {
        auto spec = _outTextures[i]->getSpec();
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);
        if (_render16bits)
            _outTextures[i]->reset(GL_TEXTURE_2D, 0, GL_RGBA16, spec.width, spec.height, 0, GL_RGBA, GL_UNSIGNED_SHORT, nullptr);
        else
            _outTextures[i]->reset(GL_TEXTURE_2D, 0, GL_RGBA, spec.width, spec.height, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, _outTextures[i]->getTexId(), 0);
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    _updateColorDepth = false;
}

/*************/
void Camera::setOutputSize(int width, int height)
{
    if (width == 0 || height == 0)
        return;

    _depthTexture->setResizable(1);
    _depthTexture->setAttribute("size", {width, height});
    _depthTexture->setResizable(_automaticResize);

    for (auto tex : _outTextures)
    {
        tex->setResizable(1);
        tex->setAttribute("size", {width, height});
        tex->setResizable(_automaticResize);
    }

    _width = width;
    _height = height;
}

/*************/
double Camera::cameraCalibration_f(const gsl_vector* v, void* params)
{
    if (params == NULL)
        return 0.0;

    Camera& camera = *(Camera*)params;

    double fov = gsl_vector_get(v, 0);
    double cx = gsl_vector_get(v, 1);
    double cy = gsl_vector_get(v, 2);

    // Check whether the camera parameters are locked
    if (camera["fov"].isLocked())
        fov = camera["fov"]()[0].asFloat();
    if (camera["principalPoint"].isLocked())
    {
        cx = camera["principalPoint"]()[0].asFloat();
        cy = camera["principalPoint"]()[1].asFloat();
    }

    // Some limits for the calibration parameters
    if (fov > 120.0 || abs(cx - 0.5) > 1.0 || abs(cy - 0.5) > 1.0)
        return numeric_limits<double>::max();

    dvec3 eye;
    dvec3 target;
    dvec3 up;
    dvec3 euler;
    for (int i = 0; i < 3; ++i)
    {
        eye[i] = gsl_vector_get(v, i + 3);
        euler[i] = gsl_vector_get(v, i + 6);
    }
    dmat4 rotateMat = yawPitchRoll(euler[0], euler[1], euler[2]);
    dvec4 targetTmp = rotateMat * dvec4(1.0, 0.0, 0.0, 0.0);
    dvec4 upTmp = rotateMat * dvec4(0.0, 0.0, 1.0, 0.0);
    for (int i = 0; i < 3; ++i)
    {
        target[i] = targetTmp[i];
        up[i] = upTmp[i];
    }
    target += eye;

    vector<dvec3> objectPoints;
    vector<dvec3> imagePoints;
    vector<float> pointsWeight;
    for (auto& point : camera._calibrationPoints)
    {
        if (!point.isSet)
            continue;

        objectPoints.emplace_back(dvec3(point.world.x, point.world.y, point.world.z));
        imagePoints.emplace_back(dvec3((point.screen.x + 1.0) / 2.0 * camera._width, (point.screen.y + 1.0) / 2.0 * camera._height, 0.0));
        pointsWeight.push_back(point.weight);
    }

#ifdef DEBUG
    Log::get() << Log::DEBUGGING << "Camera::" << __FUNCTION__ << " - Values for the current iteration (fov, cx, cy): " << fov << " " << camera._width - cx << " " << camera._height - cy << Log::endl;
#endif

    dmat4 lookM = lookAt(eye, target, up);
    dmat4 projM = dmat4(camera.computeProjectionMatrix(fov, cx, cy));
    dmat4 modelM(1.0);
    dvec4 viewport(0, 0, camera._width, camera._height);

    // Project all the object points, and measure the distance between them and the image points
    double summedDistance = 0.0;
    for (int i = 0; i < imagePoints.size(); ++i)
    {
        dvec3 projectedPoint;
        projectedPoint = project(objectPoints[i], lookM, projM, viewport);
        projectedPoint.z = 0.0;

        if (camera._weightedCalibrationPoints)
            summedDistance += pointsWeight[i] * pow(imagePoints[i].x - projectedPoint.x, 2.0) + pow(imagePoints[i].y - projectedPoint.y, 2.0);
        else
            summedDistance += pow(imagePoints[i].x - projectedPoint.x, 2.0) + pow(imagePoints[i].y - projectedPoint.y, 2.0);
    }
    summedDistance /= imagePoints.size();

#ifdef DEBUG
    Log::get() << Log::DEBUGGING << "Camera::" << __FUNCTION__ << " - Actual summed distance: " << summedDistance << Log::endl;
#endif

    return summedDistance;
}

/*************/
dmat4 Camera::computeProjectionMatrix()
{
    return computeProjectionMatrix(_fov, _cx, _cy);
}

/*************/
dmat4 Camera::computeProjectionMatrix(float fov, float cx, float cy)
{
    double l, r, t, b, n, f;
    // Near and far are obvious
    n = _near;
    f = _far;
    // Up and down
    double tTemp = n * tan(fov * M_PI / 360.0);
    double bTemp = -tTemp;
    t = tTemp - (cy - 0.5) * (tTemp - bTemp);
    b = bTemp - (cy - 0.5) * (tTemp - bTemp);
    // Left and right
    double rTemp = tTemp * _width / _height;
    double lTemp = bTemp * _width / _height;
    r = rTemp - (cx - 0.5) * (rTemp - lTemp);
    l = lTemp - (cx - 0.5) * (rTemp - lTemp);

    return frustum(l, r, b, t, n, f);
}

/*************/
dmat4 Camera::computeViewMatrix()
{
    // Eye and target can't be identical
    if (_eye == _target)
    {
        _target[0] = _eye[0] + _up[1];
        _target[1] = _eye[1] + _up[2];
        _target[2] = _eye[2] + _up[0];
    }

    dmat4 viewMatrix = lookAt(_eye, _target, _up);
    return viewMatrix;
}

/*************/
void Camera::loadDefaultModels()
{
    map<string, string> files {{"3d_marker", "3d_marker.obj"},
                               {"2d_marker", "2d_marker.obj"},
                               {"camera", "camera.obj"}};
    
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
                Log::get() << Log::WARNING << "Camera::" << __FUNCTION__ << " - File " << file.second << " does not seem to be readable." << Log::endl;
                continue;
            }
        }

        shared_ptr<Mesh> mesh = make_shared<Mesh>(_root);
        mesh->setName(file.first);
        mesh->setAttribute("file", {file.second});
        _modelMeshes.push_back(mesh);

        shared_ptr<Object> obj = make_shared<Object>(_root);
        obj->setName(file.first);
        obj->setAttribute("scale", {WORLDMARKER_SCALE});
        obj->setAttribute("fill", {"color"});
        obj->setAttribute("color", MARKER_SET);
        obj->linkTo(mesh);

        _models[file.first] = obj;
    }
}

/*************/
void Camera::sendCalibrationPointsToObjects()
{
    for (auto& objWeakPtr : _objects)
    {
        auto object = objWeakPtr.lock();
        for (auto& point : _calibrationPoints)
        {
            object->addCalibrationPoint(point.world);
        }
    }
}

/*************/
void Camera::registerAttributes()
{
    addAttribute("eye", [&](const Values& args) {
        _eye = dvec3(args[0].asFloat(), args[1].asFloat(), args[2].asFloat());
        return true;
    }, [&]() -> Values {
        return {_eye.x, _eye.y, _eye.z};
    }, {'n', 'n', 'n'});
    setAttributeDescription("eye", "Set the camera position");

    addAttribute("target", [&](const Values& args) {
        _target = dvec3(args[0].asFloat(), args[1].asFloat(), args[2].asFloat());
        return true;
    }, [&]() -> Values {
        return {_target.x, _target.y, _target.z};
    }, {'n', 'n', 'n'});
    setAttributeDescription("target", "Set the camera target position");

    addAttribute("fov", [&](const Values& args) {
        _fov = args[0].asFloat();
        return true;
    }, [&]() -> Values {
        return {_fov};
    }, {'n'});
    setAttributeDescription("fov", "Set the camera field of view");

    addAttribute("up", [&](const Values& args) {
        _up = dvec3(args[0].asFloat(), args[1].asFloat(), args[2].asFloat());
        return true;
    }, [&]() -> Values {
        return {_up.x, _up.y, _up.z};
    }, {'n', 'n', 'n'});
    setAttributeDescription("up", "Set the camera up vector");

    addAttribute("size", [&](const Values& args) {
        _newWidth = args[0].asInt();
        _newHeight = args[1].asInt();
        _automaticResize = false; // Automatic resize is disabled when size is specified
        return true;
    }, [&]() -> Values {
        return {_width, _height};
    }, {'n', 'n'});
    setAttributeDescription("size", "Set the render size");

    addAttribute("principalPoint", [&](const Values& args) {
        _cx = args[0].asFloat();
        _cy = args[1].asFloat();
        return true;
    }, [&]() -> Values {
        return {_cx, _cy};
    }, {'n', 'n'});
    setAttributeDescription("principalPoint", "Set the principal point of the lens (for lens shifting)");

    addAttribute("weightedCalibrationPoints", [&](const Values& args) {
        _weightedCalibrationPoints = args[0].asInt();
        return true;
    }, [&]() -> Values {
        return {(int)_weightedCalibrationPoints};
    }, {'n'});
    setAttributeDescription("weightedCalibrationPoints", "If set to 1, calibration points located near the edges are more weight in the calibration");

    // More advanced attributes
    addAttribute("moveEye", [&](const Values& args) {
        _eye.x = _eye.x + args[0].asFloat();
        _eye.y = _eye.y + args[1].asFloat();
        _eye.z = _eye.z + args[2].asFloat();
        return true;
    }, {'n', 'n', 'n'});
    setAttributeDescription("moveEye", "Move the eye by the specified vector");

    addAttribute("moveTarget", [&](const Values& args) {
        _target.x = _target.x + args[0].asFloat();
        _target.y = _target.y + args[1].asFloat();
        _target.z = _target.z + args[2].asFloat();
        return true;
    }, {'n', 'n', 'n'});
    setAttributeDescription("moveTarget", "Move the target by the specified vector");

    addAttribute("rotateAroundTarget", [&](const Values& args) {
        dvec3 direction = _target - _eye;
        dmat4 rotZ = rotate(dmat4(1.f), (double)args[0].asFloat(), dvec3(0.0, 0.0, 1.0));
        dvec4 newDirection = dvec4(direction, 1.0) * rotZ;
        _eye = _target - dvec3(newDirection.x, newDirection.y, newDirection.z);

        direction = _eye - _target;
        direction = rotate(direction, (double)args[1].asFloat(), dvec3(direction[1], -direction[0], 0.0));
        dvec3 newEye = direction + _target;
        if (angle(normalize(dvec3(newEye[0], newEye[1], std::abs(newEye[2]))), dvec3(0.0, 0.0, 1.0)) >= 0.2)
            _eye = direction + _target;

        return true;
    }, {'n', 'n', 'n'});
    setAttributeDescription("rotateAroundTarget", "Rotate around the target point by the given Euler angles");

    addAttribute("rotateAroundPoint", [&](const Values& args) {
        dvec3 point(args[3].asFloat(), args[4].asFloat(), args[5].asFloat());
        dmat4 rotZ = rotate(dmat4(1.f), (double)args[0].asFloat(), dvec3(0.0, 0.0, 1.0));

        // Rotate the target around Z, then the eye
        dvec3 direction = point - _target;
        dvec4 newDirection = dvec4(direction, 1.0) * rotZ;
        _target = point - dvec3(newDirection.x, newDirection.y, newDirection.z);

        direction = point - _eye;
        newDirection = dvec4(direction, 1.0) * rotZ;
        _eye = point - dvec3(newDirection.x, newDirection.y, newDirection.z);

        // Rotate around the X axis
        dvec3 axis = normalize(_eye - _target);
        direction = point - _target;
        dvec3 tmpTarget = rotate(direction, (double)args[1].asFloat(), dvec3(axis[1], -axis[0], 0.0));
        tmpTarget = point - tmpTarget;

        direction = point - _eye;
        dvec3 tmpEye = rotate(direction, (double)args[1].asFloat(), dvec3(axis[1], -axis[0], 0.0));
        tmpEye = point - tmpEye;
        
        direction = tmpEye - tmpTarget;
        if (angle(normalize(dvec3(direction[0], direction[1], std::abs(direction[2]))), dvec3(0.0, 0.0, 1.0)) >= 0.2)
        {
            _eye = tmpEye;
            _target = tmpTarget;
        }

        return true;
    }, {'n', 'n', 'n', 'n', 'n', 'n'});
    setAttributeDescription("rotateAroundPoint", "Rotate around a given point by the given Euler angles");

    addAttribute("pan", [&](const Values& args) {
        dvec4 panV(args[0].asFloat(), args[1].asFloat(), args[2].asFloat(), 0.f);
        dvec3 dirV = normalize(_eye - _target);

        dmat4 rotMat = inverse(computeViewMatrix());
        panV = rotMat * panV;
        _target = _target + dvec3(panV[0], panV[1], panV[2]);
        _eye = _eye + dvec3(panV[0], panV[1], panV[2]);
        panV = normalize(panV);

        return true;
    }, {'n', 'n', 'n'});
    setAttributeDescription("pan", "Move the camera in its focal plane");

    addAttribute("forward", [&](const Values& args) {
        float value = args[0].asFloat();
        dvec3 dirV = normalize(_eye - _target);
        dirV *= value;
        _target += dirV;
        _eye += dirV;
        return true;
    }, {'n'});
    setAttributeDescription("forward", "Move the camera forward along its Z axis");

    addAttribute("addCalibrationPoint", [&](const Values& args) {
        addCalibrationPoint({args[0].asFloat(), args[1].asFloat(), args[2].asFloat()});
        return true;
    }, {'n', 'n', 'n'});
    setAttributeDescription("addCalibrationPoint", "Add a calibration point at the given position");

    addAttribute("deselectedCalibrationPoint", [&](const Values& args) {
        deselectCalibrationPoint();
        return true;
    });
    setAttributeDescription("deselectCalibrationPoint", "Deselect any calibration point");

    addAttribute("moveCalibrationPoint", [&](const Values& args) {
        moveCalibrationPoint(args[0].asFloat(), args[1].asFloat());
        return true;
    }, {'n', 'n'});
    setAttributeDescription("moveCalibrationPoint", "Move the target calibration point in the 2D projection space");

    addAttribute("removeCalibrationPoint", [&](const Values& args) {
        if (args.size() == 3)
            removeCalibrationPoint({args[0].asFloat(), args[1].asFloat(), args[2].asFloat()});
        else
            removeCalibrationPoint({args[0].asFloat(), args[1].asFloat(), args[2].asFloat()}, args[3].asInt());
        return true;
    }, {'n', 'n', 'n'});
    setAttributeDescription("removeCalibrationPoint", "Remove the calibration point given its 3D coordinates");

    addAttribute("setCalibrationPoint", [&](const Values& args) {
        return setCalibrationPoint({args[0].asFloat(), args[1].asFloat()});
    }, {'n', 'n'});
    setAttributeDescription("setCalibrationPoint", "Set the 2D projection of a calibration point");

    addAttribute("selectNextCalibrationPoint", [&](const Values& args) {
        _selectedCalibrationPoint = (_selectedCalibrationPoint + 1) % _calibrationPoints.size();
        return true;
    });
    setAttributeDescription("selectNextCalibrationPoint", "Select the next available calibration point");

    addAttribute("selectPreviousCalibrationPoint", [&](const Values& args) {
        if (_selectedCalibrationPoint == 0)
            _selectedCalibrationPoint = _calibrationPoints.size() - 1;
        else
            _selectedCalibrationPoint--;
        return true;
    });
    setAttributeDescription("selectPreviousCalibrationPoint", "Select the previous available calibration point");

    // Store / restore calibration points
    addAttribute("calibrationPoints", [&](const Values& args) {
        for (auto& arg : args)
        {
            if (arg.getType() != Value::Type::v)
                continue;

            Values v = arg.asValues();
            CalibrationPoint c;
            c.world[0] = v[0].asFloat();
            c.world[1] = v[1].asFloat();
            c.world[2] = v[2].asFloat();
            c.screen[0] = v[3].asFloat();
            c.screen[1] = v[4].asFloat();
            c.isSet = v[5].asInt();

            _calibrationPoints.push_back(c);
        }

        sendCalibrationPointsToObjects();

        return true;
    }, [&]() -> Values {
        Values data;
        for (auto& p : _calibrationPoints)
        {
            Values d {p.world[0], p.world[1], p.world[2], p.screen[0], p.screen[1], p.isSet};
            data.emplace_back(d);
        }
        return data;
    });
    setAttributeDescription("calibrationPoints", "Set multiple calibration points, as an array of 6D vector (position, projection and status)");

    // Rendering options
    addAttribute("16bits", [&](const Values& args) {
        bool render16bits = args[0].asInt();
        if (render16bits != _render16bits)
        {
            _render16bits = render16bits;
            _updateColorDepth = true;
        }
        return true;
    }, [&]() -> Values {
        return {(int)_render16bits};
    }, {'n'});
    setAttributeDescription("16bits", "Set to 1 for the camera to render in 16bits per component (otherwise 8bpc)");

    addAttribute("blendWidth", [&](const Values& args) {
        _blendWidth = args[0].asFloat();
        return true;
    }, [&]() -> Values {
        return {_blendWidth};
    }, {'n'});
    setAttributeDescription("blendWidth", "Set the projectors blending width");

    addAttribute("blendPrecision", [&](const Values& args) {
        _blendPrecision = args[0].asFloat();
        return true;
    }, [&]() -> Values {
        return {_blendPrecision};
    }, {'n'});
    setAttributeDescription("blendPrecision", "Set the blending precision");

    addAttribute("clearColor", [&](const Values& args) {
        if (args.size() == 0)
            _clearColor = CAMERA_FLASH_COLOR;
        else if (args.size() == 4)
            _clearColor = dvec4(args[0].asFloat(), args[1].asFloat(), args[2].asFloat(), args[3].asFloat());
        else
            return false;

        return true;
    });
    setAttributeDescription("clearColor", "Clears the camera, with a default color if no argument is given (as RGBA)");

    addAttribute("colorTemperature", [&](const Values& args) {
        _colorTemperature = args[0].asFloat();
        _colorTemperature = std::max(1000.f, std::min(15000.f, _colorTemperature));
        return true;
    }, [&]() -> Values {
        return {_colorTemperature};
    }, {'n'});
    setAttributeDescription("colorTemperature", "Set the color temperature correction");

    addAttribute("colorLUT", [&](const Values& args) {
        if (args[0].asValues().size() != 768)
            return false;
        for (auto& v : args[0].asValues())
            if (v.getType() != Value::Type::f)
                return false;

        _colorLUT = args[0].asValues();

        return true;
    }, [&]() -> Values {
        if (_colorLUT.size() == 768)
            return {_colorLUT};
        else
            return {};
    }, {'v'});
    setAttributeDescription("colorLUT", "Set the color lookup table");

    addAttribute("activateColorLUT", [&](const Values& args) {
        if (args[0].asInt() == 2)
            _isColorLUTActivated = (_isColorLUTActivated != true);
        else if ((int)_isColorLUTActivated == args[0].asInt())
            return true;
        else
            _isColorLUTActivated = args[0].asInt();

        if (_isColorLUTActivated)
            Log::get() << Log::MESSAGE << "Camera::activateColorLUT - Color lookup table activated for camera " << getName() << Log::endl;
        else
            Log::get() << Log::MESSAGE << "Camera::activateColorLUT - Color lookup table deactivated for camera " << getName() << Log::endl;

        return true;
    }, [&]() -> Values {
        return {(int)_isColorLUTActivated};
    }, {'n'});
    setAttributeDescription("activateColorLUT", "Activate the color lookup table. If set to 2, switches its status");

    addAttribute("colorMixMatrix", [&](const Values& args) {
        if (args[0].asValues().size() != 9)
            return false;

        for (int u = 0; u < 3; ++u)
            for (int v = 0; v < 3; ++v)
                _colorMixMatrix[u][v] = args[0].asValues()[u*3 + v].asFloat();
        return true;
    }, [&]() -> Values {
        Values m(9);
        for (int u = 0; u < 3; ++u)
            for (int v = 0; v < 3; ++v)
                m[u*3 + v] = _colorMixMatrix[u][v];
        return {m};
    }, {'v'});
    setAttributeDescription("colorMixMatrix", "Set the color correction matrix");

    addAttribute("brightness", [&](const Values& args) {
        _brightness = args[0].asFloat();
        return true;
    }, [&]() -> Values {
        return {_brightness};
    }, {'n'});
    setAttributeDescription("brightness", "Set the camera brightness");

    addAttribute("frame", [&](const Values& args) {
        if (args[0].asInt() > 0)
            _drawFrame = true;
        else
            _drawFrame = false;
        return true;
    }, {'n'});
    setAttributeDescription("frame", "If set to 1, draws a frame around the camera");

    addAttribute("hide", [&](const Values& args) {
        if (args[0].asInt() > 0)
            _hidden = true;
        else if (args[0].asInt() == 0)
            _hidden = false;
        else
            _hidden = !_hidden;
        return true;
    }, {'n'});
    setAttributeDescription("hide", "If set to 1, prevent from drawing this camera");

    addAttribute("wireframe", [&](const Values& args) {
        string primitive;
        if (args[0].asInt() == 0)
            primitive = "texture";
        else
            primitive = "wireframe";

        for (auto& o : _objects)
        {
            if (o.expired())
                continue;
            auto obj = o.lock();
            obj->setAttribute("fill", {primitive});
        }
        return true;
    }, {'n'});
    setAttributeDescription("wireframe", "If set to 1, draws all linked objects as wireframes");

    //
    // Various options
    addAttribute("displayCalibration", [&](const Values& args) {
        if (args[0].asInt() > 0)
            _displayCalibration = true;
        else
            _displayCalibration = false;
        return true;
    }, {'n'});
    setAttributeDescription("displayCalibration", "If set to 1, display the calibration points");

    // Shows all calibration points for all cameras linked to the same objects
    addAttribute("displayAllCalibrations", [&](const Values& args) {
        _displayAllCalibrations = (args[0].asInt() > 0) ? true : false;
        return true;
    }, {'n'});
    setAttributeDescription("displayAllCalibrations", "If set to 1, display all calibration points from other cameras");

    addAttribute("switchShowAllCalibrationPoints", [&](const Values& args) {
        _showAllCalibrationPoints = !_showAllCalibrationPoints;
        return true;
    });
    setAttributeDescription("switchShowAllCalibrationPoints", "Switch whether to show all calibration points");

    addAttribute("switchDisplayAllCalibration", [&](const Values& args) {
        _displayAllCalibrations = !_displayAllCalibrations;
        return true;
    });
    setAttributeDescription("switchDisplayAllCalibration", "Switch whether to show all calibration points in this camera");

    addAttribute("flashBG", [&](const Values& args) {
        _flashBG = args[0].asInt();
        return true;
    }, {'n'});
    setAttributeDescription("flashBG", "If set to 1, switch background to light gray");
}

} // end of namespace
