// render.cpp
//
// Copyright (C) 2001-2009, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#define DEBUG_COALESCE               0
#define DEBUG_SECONDARY_ILLUMINATION 0
#define DEBUG_ORBIT_CACHE            0

//#define DEBUG_HDR
#ifdef DEBUG_HDR
//#define DEBUG_HDR_FILE
//#define DEBUG_HDR_ADAPT
//#define DEBUG_HDR_TONEMAP
#endif
#ifdef DEBUG_HDR_FILE
#include <fstream>
std::ofstream hdrlog;
#define HDR_LOG     hdrlog
#else
#define HDR_LOG     cout
#endif

#ifdef USE_HDR
#define BLUR_PASS_COUNT     2
#define BLUR_SIZE           128
#define DEFAULT_EXPOSURE    -23.35f
#define EXPOSURE_HALFLIFE   0.4f
//#define USE_BLOOM_LISTS
#endif

// #define ENABLE_SELF_SHADOW

#include <config.h>
#include "render.h"
#include "boundaries.h"
#include "asterism.h"
#include "astro.h"
#include "vecgl.h"
#include "glshader.h"
#include "shadermanager.h"
#include "spheremesh.h"
#include "lodspheremesh.h"
#include "geometry.h"
#include "texmanager.h"
#include "meshmanager.h"
#include "renderinfo.h"
#include "renderglsl.h"
#include "axisarrow.h"
#include "frametree.h"
#include "timelinephase.h"
#include "skygrid.h"
#include "modelgeometry.h"
#include "curveplot.h"
#include "shadermanager.h"
#include "rectangle.h"
#include <celutil/debug.h>
#include <celmath/frustum.h>
#include <celmath/distance.h>
#include <celmath/intersect.h>
#include <celmath/geomutil.h>
#include <celutil/utf8.h>
#include <celutil/util.h>
#include <celutil/timer.h>
#include <GL/glew.h>
#ifdef VIDEO_SYNC
#ifdef _WIN32
#include <GL/wglew.h>
#else
#ifndef __APPLE__
#include <GL/glxew.h>
#endif // __APPLE__
#endif //_WIN32
#endif // VIDEO_SYNC
#include <algorithm>
#include <cstring>
#include <cassert>
#include <sstream>
#include <iomanip>
#include <numeric>

using namespace cmod;
using namespace Eigen;
using namespace std;
using namespace celmath;

#define FOV           45.0f
#define NEAR_DIST      0.5f
#define FAR_DIST       1.0e9f

static const float  STAR_DISTANCE_LIMIT  = 1.0e6f;
static const int REF_DISTANCE_TO_SCREEN  = 400; //[mm]

// Contribution from planetshine beyond this distance (in units of object radius)
// is considered insignificant.
static const float PLANETSHINE_DISTANCE_LIMIT_FACTOR = 100.0f;

// Planetshine from objects less than this pixel size is treated as insignificant
// and will be ignored.
static const float PLANETSHINE_PIXEL_SIZE_LIMIT      =   0.1f;

// Distance from the Sun at which comet tails will start to fade out
static const float COMET_TAIL_ATTEN_DIST_SOL = astro::AUtoKilometers(5.0f);

// Fractional pixel offset used when rendering text as texture mapped
// quads to ensure consistent mapping of texels to pixels.
static const float PixelOffset = 0.125f;

// These two values constrain the near and far planes of the view frustum
// when rendering planet and object meshes.  The near plane will never be
// closer than MinNearPlaneDistance, and the far plane is set so that far/near
// will not exceed MaxFarNearRatio.
static const float MinNearPlaneDistance = 0.0001f; // km
static const float MaxFarNearRatio      = 2000000.0f;

static const float RenderDistance       = 50.0f;

// Star disc size in pixels
static const float BaseStarDiscSize      = 5.0f;
static const float MaxScaledDiscStarSize = 8.0f;
static const float GlareOpacity = 0.65f;

static const float MinRelativeOccluderRadius = 0.005f;

static const float CubeCornerToCenterDistance = (float) sqrt(3.0);


// The minimum apparent size of an objects orbit in pixels before we display
// a label for it.  This minimizes label clutter.
static const float MinOrbitSizeForLabel = 20.0f;

// The minimum apparent size of a surface feature in pixels before we display
// a label for it.
static const float MinFeatureSizeForLabel = 20.0f;

/* The maximum distance of the observer to the origin of coordinates before
   asterism lines and labels start to linearly fade out (in light years) */
static const float MaxAsterismLabelsConstDist  = 6.0f;
static const float MaxAsterismLinesConstDist   = 600.0f;

/* The maximum distance of the observer to the origin of coordinates before
   asterisms labels and lines fade out completely (in light years) */
static const float MaxAsterismLabelsDist = 20.0f;
static const float MaxAsterismLinesDist  = 6.52e4f;

// Static meshes and textures used by all instances of Simulation

static bool commonDataInitialized = false;


LODSphereMesh* g_lodSphere = nullptr;

static Texture* normalizationTex = nullptr;

static Texture* starTex = nullptr;
static Texture* glareTex = nullptr;
static Texture* shadowTex = nullptr;
static Texture* gaussianDiscTex = nullptr;
static Texture* gaussianGlareTex = nullptr;

static Texture* eclipseShadowTextures[4];
static Texture* shadowMaskTexture = nullptr;
static Texture* penumbraFunctionTexture = nullptr;

Texture* rectToSphericalTexture = nullptr;

static const Color compassColor(0.4f, 0.4f, 1.0f);

static const float CoronaHeight = 0.2f;

static const int MaxSkyRings = 32;
static const int MaxSkySlices = 180;
static const int MinSkySlices = 30;

// Size at which the orbit cache will be flushed of old orbit paths
static const unsigned int OrbitCacheCullThreshold = 200;
// Age in frames at which unused orbit paths may be eliminated from the cache
static const uint32_t OrbitCacheRetireAge = 16;

Color Renderer::StarLabelColor          (0.471f, 0.356f, 0.682f);
Color Renderer::PlanetLabelColor        (0.407f, 0.333f, 0.964f);
Color Renderer::DwarfPlanetLabelColor   (0.557f, 0.235f, 0.576f);
Color Renderer::MoonLabelColor          (0.231f, 0.733f, 0.792f);
Color Renderer::MinorMoonLabelColor     (0.231f, 0.733f, 0.792f);
Color Renderer::AsteroidLabelColor      (0.596f, 0.305f, 0.164f);
Color Renderer::CometLabelColor         (0.768f, 0.607f, 0.227f);
Color Renderer::SpacecraftLabelColor    (0.93f,  0.93f,  0.93f);
Color Renderer::LocationLabelColor      (0.24f,  0.89f,  0.43f);
Color Renderer::GalaxyLabelColor        (0.0f,   0.45f,  0.5f);
Color Renderer::GlobularLabelColor      (0.8f,   0.45f,  0.5f);
Color Renderer::NebulaLabelColor        (0.541f, 0.764f, 0.278f);
Color Renderer::OpenClusterLabelColor   (0.239f, 0.572f, 0.396f);
Color Renderer::ConstellationLabelColor (0.225f, 0.301f, 0.36f);
Color Renderer::EquatorialGridLabelColor(0.64f,  0.72f,  0.88f);
Color Renderer::PlanetographicGridLabelColor(0.8f, 0.8f, 0.8f);
Color Renderer::GalacticGridLabelColor  (0.88f,  0.72f,  0.64f);
Color Renderer::EclipticGridLabelColor  (0.72f,  0.64f,  0.88f);
Color Renderer::HorizonGridLabelColor   (0.72f,  0.72f,  0.72f);

Color Renderer::StarOrbitColor          (0.5f,   0.5f,   0.8f);
Color Renderer::PlanetOrbitColor        (0.3f,   0.323f, 0.833f);
Color Renderer::DwarfPlanetOrbitColor   (0.557f, 0.235f, 0.576f);
Color Renderer::MoonOrbitColor          (0.08f,  0.407f, 0.392f);
Color Renderer::MinorMoonOrbitColor     (0.08f,  0.407f, 0.392f);
Color Renderer::AsteroidOrbitColor      (0.58f,  0.152f, 0.08f);
Color Renderer::CometOrbitColor         (0.639f, 0.487f, 0.168f);
Color Renderer::SpacecraftOrbitColor    (0.4f,   0.4f,   0.4f);
Color Renderer::SelectionOrbitColor     (1.0f,   0.0f,   0.0f);

Color Renderer::ConstellationColor      (0.0f,   0.24f,  0.36f);
Color Renderer::BoundaryColor           (0.24f,  0.10f,  0.12f);
Color Renderer::EquatorialGridColor     (0.28f,  0.28f,  0.38f);
Color Renderer::PlanetographicGridColor (0.8f,   0.8f,   0.8f);
Color Renderer::PlanetEquatorColor      (0.5f,   1.0f,   1.0f);
Color Renderer::GalacticGridColor       (0.38f,  0.38f,  0.28f);
Color Renderer::EclipticGridColor       (0.38f,  0.28f,  0.38f);
Color Renderer::HorizonGridColor        (0.38f,  0.38f,  0.38f);
Color Renderer::EclipticColor           (0.5f,   0.1f,   0.1f);

Color Renderer::SelectionCursorColor    (1.0f,   0.0f,   0.0f);

// Solar system objects
constexpr const uint64_t ShowSSO = Renderer::ShowPlanets      |
                                   Renderer::ShowDwarfPlanets |
                                   Renderer::ShowMoons        |
                                   Renderer::ShowMinorMoons   |
                                   Renderer::ShowAsteroids    |
                                   Renderer::ShowComets       |
                                   Renderer::ShowSpacecrafts;
// Deep Sky Objects
constexpr const uint64_t ShowDSO = Renderer::ShowGalaxies     |
                                   Renderer::ShowGlobulars    |
                                   Renderer::ShowNebulae      |
                                   Renderer::ShowOpenClusters;

#ifdef ENABLE_SELF_SHADOW
static FramebufferObject* shadowFbo = nullptr;
#endif


// Some useful unit conversions
inline float mmToInches(float mm)
{
    return mm * (1.0f / 25.4f);
}

inline float inchesToMm(float in)
{
    return in * 25.4f;
}


// Fade function for objects that shouldn't be shown when they're too small
// on screen such as orbit paths and some object labels. The will fade linearly
// from invisible at minSize pixels to full visibility at opaqueScale*minSize.
inline float sizeFade(float screenSize, float minScreenSize, float opaqueScale)
{
    return min(1.0f, (screenSize - minScreenSize) / (minScreenSize * (opaqueScale - 1)));
}


// Calculate the cosine of half the maximum field of view. We'll use this for
// fast testing of object visibility.  The function takes the vertical FOV (in
// degrees) as an argument. When computing the view cone, we want the field of
// view as measured on the diagonal between viewport corners.
double computeCosViewConeAngle(double verticalFOV, double width, double height)
{
    double h = tan(degToRad(verticalFOV / 2));
    double diag = sqrt(1.0 + square(h) + square(h * width / height));
    return 1.0 / diag;
}

// PointStarVertexBuffer is used when hardware supports point sprites.
class PointStarVertexBuffer
{
public:
    PointStarVertexBuffer(const Renderer& _renderer, unsigned int _capacity);
    ~PointStarVertexBuffer();
    void startPoints();
    void startSprites();
    void render();
    void finish();
    inline void addStar(const Eigen::Vector3f& pos, const Color&, float);
    void setTexture(Texture* /*_texture*/);

private:
    struct StarVertex
    {
        Eigen::Vector3f position;
        float size;
        unsigned char color[4];
        float pad;
    };

    const Renderer& renderer;
    unsigned int capacity;
    unsigned int nStars{ 0 };
    StarVertex* vertices{ nullptr };
    bool useSprites{ false };
    Texture* texture{ nullptr };
};

PointStarVertexBuffer::PointStarVertexBuffer(const Renderer& _renderer,
                                             unsigned int _capacity) :
    renderer(_renderer),
    capacity(_capacity)
{
    vertices = new StarVertex[capacity];
}

PointStarVertexBuffer::~PointStarVertexBuffer()
{
    delete[] vertices;
}

void PointStarVertexBuffer::startSprites()
{
    CelestiaGLProgram* prog = renderer.getShaderManager().getShader("star");
    if (prog == nullptr)
        return;

    prog->use();
    prog->samplerParam("starTex") = 0;

    unsigned int stride = sizeof(StarVertex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, stride, &vertices[0].position);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, stride, &vertices[0].color);

    glEnableVertexAttribArray(CelestiaGLProgram::PointSizeAttributeIndex);
    glVertexAttribPointer(CelestiaGLProgram::PointSizeAttributeIndex,
                          1, GL_FLOAT, GL_FALSE,
                          stride, &vertices[0].size);

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);

    glEnable(GL_POINT_SPRITE);

    useSprites = true;
}

void PointStarVertexBuffer::startPoints()
{
    unsigned int stride = sizeof(StarVertex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, stride, &vertices[0].position);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, stride, &vertices[0].color);

    // An option to control the size of the stars would be helpful.
    // Which size looks best depends a lot on the resolution and the
    // type of display device.
    // glPointSize(2.0f);
    // glEnable(GL_POINT_SMOOTH);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);

    glDisableClientState(GL_NORMAL_ARRAY);

    useSprites = false;
}

void PointStarVertexBuffer::render()
{
    if (nStars != 0)
    {
        unsigned int stride = sizeof(StarVertex);
        if (useSprites)
        {
            glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
            glEnable(GL_TEXTURE_2D);
        }
        else
        {
            glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
            glDisable(GL_TEXTURE_2D);
            glPointSize(1.0f);
        }
        glVertexPointer(3, GL_FLOAT, stride, &vertices[0].position);
        glColorPointer(4, GL_UNSIGNED_BYTE, stride, &vertices[0].color);

        if (useSprites)
        {
            glVertexAttribPointer(CelestiaGLProgram::PointSizeAttributeIndex,
                                  1, GL_FLOAT, GL_FALSE,
                                  stride, &vertices[0].size);
        }

        if (texture != nullptr)
            texture->bind();
        glDrawArrays(GL_POINTS, 0, nStars);
        nStars = 0;
    }
}

void PointStarVertexBuffer::finish()
{
    render();
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    if (useSprites)
    {
        glDisableVertexAttribArray(CelestiaGLProgram::PointSizeAttributeIndex);
        glUseProgram(0);
        glDisable(GL_POINT_SPRITE);
    }
    else
    {
        glEnable(GL_TEXTURE_2D);
    }
}

inline void PointStarVertexBuffer::addStar(const Eigen::Vector3f& pos,
                                           const Color& color,
                                           float size)
{
    if (nStars < capacity)
    {
        vertices[nStars].position = pos;
        vertices[nStars].size = size;
        color.get(vertices[nStars].color);
        nStars++;
    }

    if (nStars == capacity)
    {
        render();
        nStars = 0;
    }
}

void PointStarVertexBuffer::setTexture(Texture* _texture)
{
  texture = _texture;
}

/**** End star vertex buffer classes ****/


Renderer::Renderer() :
    windowWidth(0),
    windowHeight(0),
    fov(FOV),
    cosViewConeAngle(computeCosViewConeAngle(fov, 1, 1)),
    screenDpi(96),
    corrFac(1.12f),
    faintestAutoMag45deg(8.0f), //def. 7.0f
    renderMode(GL_FILL),
    labelMode(LocationLabels), //def. NoLabels
    renderFlags(DefaultRenderFlags),
    orbitMask(Body::Planet | Body::Moon | Body::Stellar),
    ambientLightLevel(0.1f),
    brightnessBias(0.0f),
    saturationMagNight(1.0f),
    saturationMag(1.0f),
    starStyle(FuzzyPointStars),
    pointStarVertexBuffer(nullptr),
    glareVertexBuffer(nullptr),
    textureResolution(medres),
    frameCount(0),
    lastOrbitCacheFlush(0),
    minOrbitSize(MinOrbitSizeForLabel),
    distanceLimit(1.0e6f),
    minFeatureSize(MinFeatureSizeForLabel),
    locationFilter(~0ull),
    colorTemp(nullptr),
#ifdef USE_HDR
    sceneTexture(0),
    blurFormat(GL_RGBA),
    useLuminanceAlpha(false),
    bloomEnabled(true),
    maxBodyMag(100.0f),
    exposure(1.0f),
    exposurePrev(1.0f),
    brightPlus(0.0f),
#endif
    videoSync(false),
    settingsChanged(true),
    objectAnnotationSetOpen(false)
{
    pointStarVertexBuffer = new PointStarVertexBuffer(*this, 2048);
    glareVertexBuffer = new PointStarVertexBuffer(*this, 2048);
    skyVertices = new SkyVertex[MaxSkySlices * (MaxSkyRings + 1)];
    skyIndices = new uint32_t[(MaxSkySlices + 1) * 2 * MaxSkyRings];
    skyContour = new SkyContourPoint[MaxSkySlices + 1];
    colorTemp = GetStarColorTable(ColorTable_Blackbody_D65);
#ifdef DEBUG_HDR_FILE
    HDR_LOG.open("hdr.log", ios_base::app);
#endif
#ifdef USE_HDR
    blurTextures = new Texture*[BLUR_PASS_COUNT];
    blurTempTexture = nullptr;
    for (size_t i = 0; i < BLUR_PASS_COUNT; ++i)
    {
        blurTextures[i] = nullptr;
    }
#endif
#ifdef USE_BLOOM_LISTS
    for (size_t i = 0; i < (sizeof gaussianLists/sizeof(GLuint)); ++i)
    {
        gaussianLists[i] = 0;
    }
#endif

    for (int i = 0; i < (int) FontCount; i++)
    {
        font[i] = nullptr;
    }

    shaderManager = new ShaderManager();
}


Renderer::~Renderer()
{
    delete pointStarVertexBuffer;
    delete glareVertexBuffer;
    delete[] skyVertices;
    delete[] skyIndices;
    delete[] skyContour;
#ifdef USE_BLOOM_LISTS
    for (size_t i = 0; i < (sizeof gaussianLists/sizeof(GLuint)); ++i)
    {
        if (gaussianLists[i] != 0)
            glDeleteLists(gaussianLists[i], 1);
    }
#endif
#ifdef USE_HDR
    for (size_t i = 0; i < BLUR_PASS_COUNT; ++i)
    {
        if (blurTextures[i] != nullptr)
            delete blurTextures[i];
    }
    delete [] blurTextures;
    if (blurTempTexture)
        delete blurTempTexture;

    if (sceneTexture != 0)
        glDeleteTextures(1, &sceneTexture);
#endif

    delete shadowTex;
    delete shadowMaskTexture;
    delete penumbraFunctionTexture;
    delete normalizationTex;

    for(const auto tex : eclipseShadowTextures)
        delete tex;

    delete shaderManager;
}


Renderer::DetailOptions::DetailOptions() :
    orbitPathSamplePoints(100),
    shadowTextureSize(256),
    eclipseTextureSize(128),
    orbitWindowEnd(0.5),
    orbitPeriodsShown(1.0),
    linearFadeFraction(0.0)
{
}


static void StarTextureEval(float u, float v, float /*unused*/,
                            unsigned char *pixel)
{
    float r = 1 - (float) sqrt(u * u + v * v);
    if (r < 0)
        r = 0;
    else if (r < 0.5f)
        r = 2.0f * r;
    else
        r = 1;

    auto pixVal = (int) (r * 255.99f);
    pixel[0] = pixVal;
    pixel[1] = pixVal;
    pixel[2] = pixVal;
}

static void GlareTextureEval(float u, float v, float /*unused*/,
                             unsigned char *pixel)
{
    float r = 0.9f - (float) sqrt(u * u + v * v);
    if (r < 0)
        r = 0;

    auto pixVal = (int) (r * 255.99f);
    pixel[0] = 65;
    pixel[1] = 64;
    pixel[2] = 65;
    pixel[3] = pixVal;
}

static void ShadowTextureEval(float u, float v, float /*unused*/,
                              unsigned char *pixel)
{
    auto r = (float) sqrt(u * u + v * v);

    // Leave some white pixels around the edges to the shadow doesn't
    // 'leak'.  We'll also set the maximum mip map level for this texture to 3
    // so we don't have problems with the edge texels at high mip map levels.
    int pixVal = r < 15.0f / 16.0f ? 0 : 255;
    pixel[0] = pixVal;
    pixel[1] = pixVal;
    pixel[2] = pixVal;
}


//! Lookup function for eclipse penumbras--the input is the amount of overlap
//  between the occluder and sun disc, and the output is the fraction of
//  full brightness.
static void PenumbraFunctionEval(float u, float /*unused*/, float /*unused*/,
                                 unsigned char *pixel)
{
    u = (u + 1.0f) * 0.5f;

    // Using the cube root produces a good visual result
    auto pixVal = (unsigned char) (::pow((double) u, 0.33) * 255.99);

    pixel[0] = pixVal;
}


// ShadowTextureFunction is a function object for creating shadow textures
// used for rendering eclipses.
class ShadowTextureFunction : public TexelFunctionObject
{
public:
    ShadowTextureFunction(float _umbra) : umbra(_umbra) {};
    void operator()(float u, float v, float w, unsigned char* pixel) override;
    float umbra;
};

void ShadowTextureFunction::operator()(float u, float v, float /*w*/,
                                       unsigned char* pixel)
{
    auto r = (float) sqrt(u * u + v * v);
    int pixVal = 255;

    // Leave some white pixels around the edges to the shadow doesn't
    // 'leak'.  We'll also set the maximum mip map level for this texture to 3
    // so we don't have problems with the edge texels at high mip map levels.
    r = r / (15.0f / 16.0f);
    if (r < 1)
    {
        // The pixel value should depend on the area of the sun which is
        // occluded.  We just fudge it here and use the square root of the
        // radius.
        if (r <= umbra)
            pixVal = 0;
        else
            pixVal = (int) (sqrt((r - umbra) / (1 - umbra)) * 255.99f);
    }

    pixel[0] = pixVal;
    pixel[1] = pixVal;
    pixel[2] = pixVal;
};


class ShadowMaskTextureFunction : public TexelFunctionObject
{
public:
    ShadowMaskTextureFunction() = default;
    void operator()(float u, float v, float w, unsigned char* pixel) override;
    float dummy;
};

void ShadowMaskTextureFunction::operator()(float u, float /*v*/, float /*w*/,
                                           unsigned char* pixel)
{
    unsigned char a = u > 0.0f ? 255 : 0;
    pixel[0] = a;
    pixel[1] = a;
    pixel[2] = a;
    pixel[3] = a;
}


static void IllumMapEval(float x, float y, float z,
                         unsigned char* pixel)
{
    pixel[0] = 128 + (int) (127 * x);
    pixel[1] = 128 + (int) (127 * y);
    pixel[2] = 128 + (int) (127 * z);
}


#if 0
// Not used yet.

// The RectToSpherical map converts XYZ coordinates to UV coordinates
// via a cube map lookup. However, a lot of GPUs don't support linear
// interpolation of textures with > 8 bits per component, which is
// inadequate precision for storing texture coordinates. To work around
// this, we'll store the u and v texture coordinates with two 8 bit
// coordinates each: rg for u, ba for v. The coordinates are unpacked
// as: u = r * 255/256 + g * 1/255
//     v = b * 255/256 + a * 1/255
// This gives an effective precision of 16 bits for each texture coordinate.
static void RectToSphericalMapEval(float x, float y, float z,
                                   unsigned char* pixel)
{
    // Compute spherical coodinates (r is always 1)
    double phi = asin(y);
    double theta = atan2(z, -x);

    // Convert to texture coordinates
    double u = (theta / PI + 1.0) * 0.5;
    double v = (-phi / PI + 0.5);

    // Pack texture coordinates in red/green and blue/alpha
    // u = red + green/256
    // v = blue* + alpha/256
    uint16_t rg = (uint16_t) (u * 65535.99);
    uint16_t ba = (uint16_t) (v * 65535.99);
    pixel[0] = rg >> 8;
    pixel[1] = rg & 0xff;
    pixel[2] = ba >> 8;
    pixel[3] = ba & 0xff;
}
#endif


static void BuildGaussianDiscMipLevel(unsigned char* mipPixels,
                                      unsigned int log2size,
                                      float fwhm,
                                      float power)
{
    unsigned int size = 1 << log2size;
    float sigma = fwhm / 2.3548f;
    float isig2 = 1.0f / (2.0f * sigma * sigma);
    float s = 1.0f / (sigma * (float) sqrt(2.0 * PI));

    for (unsigned int i = 0; i < size; i++)
    {
        float y = (float) i - size / 2;
        for (unsigned int j = 0; j < size; j++)
        {
            float x = (float) j - size / 2;
            float r2 = x * x + y * y;
            float f = s * (float) exp(-r2 * isig2) * power;

            mipPixels[i * size + j] = (unsigned char) (255.99f * min(f, 1.0f));
        }
    }
}


static void BuildGlareMipLevel(unsigned char* mipPixels,
                               unsigned int log2size,
                               float scale,
                               float base)
{
    unsigned int size = 1 << log2size;

    for (unsigned int i = 0; i < size; i++)
    {
        float y = (float) i - size / 2;
        for (unsigned int j = 0; j < size; j++)
        {
            float x = (float) j - size / 2;
            auto r = (float) sqrt(x * x + y * y);
            auto f = (float) pow(base, r * scale);
            mipPixels[i * size + j] = (unsigned char) (255.99f * min(f, 1.0f));
        }
    }
}


#if 0
// An alternate glare function, based roughly on results in Spencer, G. et al,
// 1995, "Physically-Based Glare Effects for Digital Images"
static void BuildGlareMipLevel2(unsigned char* mipPixels,
                                unsigned int log2size,
                                float scale)
{
    unsigned int size = 1 << log2size;

    for (unsigned int i = 0; i < size; i++)
    {
        float y = (float) i - size / 2;
        for (unsigned int j = 0; j < size; j++)
        {
            float x = (float) j - size / 2;
            float r = (float) sqrt(x * x + y * y);
            float f = 0.3f / (0.3f + r * r * scale * scale * 100);
            /*
            if (i == 0 || j == 0 || i == size - 1 || j == size - 1)
                f = 1.0f;
            */
            mipPixels[i * size + j] = (unsigned char) (255.99f * min(f, 1.0f));
        }
    }
}
#endif


static Texture* BuildGaussianDiscTexture(unsigned int log2size)
{
    unsigned int size = 1 << log2size;
    Image* img = new Image(GL_LUMINANCE, size, size, log2size + 1);

    for (unsigned int mipLevel = 0; mipLevel <= log2size; mipLevel++)
    {
        float fwhm = (float) pow(2.0f, (float) (log2size - mipLevel)) * 0.3f;
        BuildGaussianDiscMipLevel(img->getMipLevel(mipLevel),
                                  log2size - mipLevel,
                                  fwhm,
                                  (float) pow(2.0f, (float) (log2size - mipLevel)));
    }

    ImageTexture* texture = new ImageTexture(*img,
                                             Texture::BorderClamp,
                                             Texture::DefaultMipMaps);
    texture->setBorderColor(Color(0.0f, 0.0f, 0.0f, 0.0f));

    delete img;

    return texture;
}


static Texture* BuildGaussianGlareTexture(unsigned int log2size)
{
    unsigned int size = 1 << log2size;
    Image* img = new Image(GL_LUMINANCE, size, size, log2size + 1);

    for (unsigned int mipLevel = 0; mipLevel <= log2size; mipLevel++)
    {
        /*
        // Optional gaussian glare
        float fwhm = (float) pow(2.0f, (float) (log2size - mipLevel)) * 0.15f;
        float power = (float) pow(2.0f, (float) (log2size - mipLevel)) * 0.15f;
        BuildGaussianDiscMipLevel(img->getMipLevel(mipLevel),
                                  log2size - mipLevel,
                                  fwhm,
                                  power);
        */
        BuildGlareMipLevel(img->getMipLevel(mipLevel),
                           log2size - mipLevel,
                           25.0f / (float) pow(2.0f, (float) (log2size - mipLevel)),
                           0.66f);
        /*
        BuildGlareMipLevel2(img->getMipLevel(mipLevel),
                            log2size - mipLevel,
                            1.0f / (float) pow(2.0f, (float) (log2size - mipLevel)));
        */
    }

    ImageTexture* texture = new ImageTexture(*img,
                                             Texture::BorderClamp,
                                             Texture::DefaultMipMaps);
    texture->setBorderColor(Color(0.0f, 0.0f, 0.0f, 0.0f));

    delete img;

    return texture;
}


static int translateLabelModeToClassMask(int labelMode)
{
    int classMask = 0;

    if (labelMode & Renderer::PlanetLabels)
        classMask |= Body::Planet;
    if (labelMode & Renderer::DwarfPlanetLabels)
        classMask |= Body::DwarfPlanet;
    if (labelMode & Renderer::MoonLabels)
        classMask |= Body::Moon;
    if (labelMode & Renderer::MinorMoonLabels)
        classMask |= Body::MinorMoon;
    if (labelMode & Renderer::AsteroidLabels)
        classMask |= Body::Asteroid;
    if (labelMode & Renderer::CometLabels)
        classMask |= Body::Comet;
    if (labelMode & Renderer::SpacecraftLabels)
        classMask |= Body::Spacecraft;

    return classMask;
}


// Depth comparison function for render list entries
bool operator<(const RenderListEntry& a, const RenderListEntry& b)
{
    // Operation is reversed because -z axis points into the screen
    return a.centerZ - a.radius > b.centerZ - b.radius;
}


// Depth comparison for labels
// Note that it's essential to declare this operator as a member
// function of Renderer::Label; if it's not a class member, C++'s
// argument dependent lookup will not find the operator when it's
// used as a predicate for STL algorithms.
bool Renderer::Annotation::operator<(const Annotation& a) const
{
    // Operation is reversed because -z axis points into the screen
    return position.z() > a.position.z();
}

// Depth comparison for orbit paths
bool Renderer::OrbitPathListEntry::operator<(const Renderer::OrbitPathListEntry& o) const
{
    // Operation is reversed because -z axis points into the screen
    return centerZ - radius > o.centerZ - o.radius;
}


#ifdef USE_GLCONTEXT
bool Renderer::init(GLContext* _context,
#else
bool Renderer::init(
#endif
                    int winWidth, int winHeight,
                    DetailOptions& _detailOptions)
{
#ifdef USE_GLCONTEXT
    context = _context;
#endif
    detailOptions = _detailOptions;

    // Initialize static meshes and textures common to all instances of Renderer
    if (!commonDataInitialized)
    {
        g_lodSphere = new LODSphereMesh();

        starTex = CreateProceduralTexture(64, 64, GL_RGB, StarTextureEval);

        glareTex = LoadTextureFromFile("textures/flare.jpg");
        if (glareTex == nullptr)
            glareTex = CreateProceduralTexture(64, 64, GL_RGB, GlareTextureEval);

        // Max mipmap level doesn't work reliably on all graphics
        // cards.  In particular, Rage 128 and TNT cards resort to software
        // rendering when this feature is enabled.  The only workaround is to
        // disable mipmapping completely unless texture border clamping is
        // supported, which solves the problem much more elegantly than all
        // the mipmap level nonsense.
        // shadowTex->setMaxMipMapLevel(3);
        Texture::AddressMode shadowTexAddress = Texture::BorderClamp;
        Texture::MipMapMode shadowTexMip = Texture::DefaultMipMaps;

        shadowTex = CreateProceduralTexture(detailOptions.shadowTextureSize,
                                            detailOptions.shadowTextureSize,
                                            GL_RGB,
                                            ShadowTextureEval,
                                            shadowTexAddress, shadowTexMip);
        shadowTex->setBorderColor(Color::White);

        if (gaussianDiscTex == nullptr)
            gaussianDiscTex = BuildGaussianDiscTexture(8);
        if (gaussianGlareTex == nullptr)
            gaussianGlareTex = BuildGaussianGlareTexture(9);

        // Create the eclipse shadow textures
        {
            for (int i = 0; i < 4; i++)
            {
                ShadowTextureFunction func(i * 0.25f);
                eclipseShadowTextures[i] =
                    CreateProceduralTexture(detailOptions.eclipseTextureSize,
                                            detailOptions.eclipseTextureSize,
                                            GL_RGB, func,
                                            shadowTexAddress, shadowTexMip);
                if (eclipseShadowTextures[i] != nullptr)
                {
                    // eclipseShadowTextures[i]->setMaxMipMapLevel(2);
                    eclipseShadowTextures[i]->setBorderColor(Color::White);
                }
            }
        }

        // Create the shadow mask texture
        {
            ShadowMaskTextureFunction func;
            shadowMaskTexture = CreateProceduralTexture(128, 2, GL_RGBA, func);
            //shadowMaskTexture->bindName();
        }

        // Create a function lookup table in a texture for use with
        // fragment program eclipse shadows.
        penumbraFunctionTexture = CreateProceduralTexture(512, 1, GL_LUMINANCE,
                                                          PenumbraFunctionEval,
                                                          Texture::EdgeClamp);

         normalizationTex = CreateProceduralCubeMap(64, GL_RGB, IllumMapEval);
#if ADVANCED_CLOUD_SHADOWS
         rectToSphericalTexture = CreateProceduralCubeMap(128, GL_RGBA, RectToSphericalMapEval);
#endif

#ifdef USE_HDR
        genSceneTexture();
        genBlurTextures();
#endif

#ifdef ENABLE_SELF_SHADOW
        if (GLEW_EXT_framebuffer_object)
        {
            shadowFbo = new FramebufferObject(1024, 1024, FramebufferObject::DepthAttachment);
            if (!shadowFbo->isValid())
            {
                clog << "Error creating shadow FBO.\n";
            }
        }
#endif

        commonDataInitialized = true;
    }

#if 0
    int nSamples = 0;
    int sampleBuffers = 0;
    int enabled = (int) glIsEnabled(GL_MULTISAMPLE);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &sampleBuffers);
    glGetIntegerv(GL_SAMPLES, &nSamples);
    clog << "AA samples: " << nSamples
         << ", enabled=" << (int) enabled
         << ", sample buffers=" << (sampleBuffers)
         << "\n";
    glEnable(GL_MULTISAMPLE);
#endif

    glEnable(GL_RESCALE_NORMAL_EXT);

    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL_EXT, GL_SEPARATE_SPECULAR_COLOR_EXT);

#ifdef USE_HDR
    Image        *testImg = new Image(GL_LUMINANCE_ALPHA, 1, 1);
    ImageTexture *testTex = new ImageTexture(*testImg,
                                             Texture::EdgeClamp,
                                             Texture::NoMipMaps);
    delete testImg;
    GLint actualTexFormat = 0;
    glEnable(GL_TEXTURE_2D);
    testTex->bind();
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &actualTexFormat);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    switch (actualTexFormat)
    {
    case 2:
    case GL_LUMINANCE_ALPHA:
    case GL_LUMINANCE4_ALPHA4:
    case GL_LUMINANCE6_ALPHA2:
    case GL_LUMINANCE8_ALPHA8:
    case GL_LUMINANCE12_ALPHA4:
    case GL_LUMINANCE12_ALPHA12:
    case GL_LUMINANCE16_ALPHA16:
        useLuminanceAlpha = true;
        break;
    default:
        useLuminanceAlpha = false;
        break;
    }

    blurFormat = useLuminanceAlpha ? GL_LUMINANCE_ALPHA : GL_RGBA;
    delete testTex;
#endif

    glLoadIdentity();

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_LIGHTING);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);

    // LEQUAL rather than LESS required for multipass rendering
    glDepthFunc(GL_LEQUAL);

    resize(winWidth, winHeight);

    return true;
}


void Renderer::resize(int width, int height)
{
#ifdef USE_HDR
    if (width == windowWidth && height == windowHeight)
        return;
#endif
    windowWidth = width;
    windowHeight = height;
    cosViewConeAngle = computeCosViewConeAngle(fov, windowWidth, windowHeight);
    // glViewport(windowWidth, windowHeight);

#ifdef USE_HDR
    if (commonDataInitialized)
    {
        genSceneTexture();
        genBlurTextures();
    }
#endif
}

float Renderer::calcPixelSize(float fovY, float windowHeight)
{
    return 2 * (float) tan(degToRad(fovY / 2.0)) / (float) windowHeight;
}

void Renderer::setFieldOfView(float _fov)
{
    fov = _fov;
    corrFac = (0.12f * fov/FOV * fov/FOV + 1.0f);
    cosViewConeAngle = computeCosViewConeAngle(fov, windowWidth, windowHeight);
}

int Renderer::getScreenDpi() const
{
    return screenDpi;
}

void Renderer::setScreenDpi(int _dpi)
{
    screenDpi = _dpi;
}

void Renderer::setFaintestAM45deg(float _faintestAutoMag45deg)
{
    faintestAutoMag45deg = _faintestAutoMag45deg;
    markSettingsChanged();
}

float Renderer::getFaintestAM45deg() const
{
    return faintestAutoMag45deg;
}

unsigned int Renderer::getResolution() const
{
    return textureResolution;
}


void Renderer::setResolution(unsigned int resolution)
{
    if (resolution < TEXTURE_RESOLUTION)
        textureResolution = resolution;
    markSettingsChanged();
}


TextureFont* Renderer::getFont(FontStyle fs) const
{
    return font[(int) fs];
}

void Renderer::setFont(FontStyle fs, TextureFont* txf)
{
    font[(int) fs] = txf;
    markSettingsChanged();
}

void Renderer::setRenderMode(int _renderMode)
{
    renderMode = _renderMode;
    markSettingsChanged();
}

uint64_t Renderer::getRenderFlags() const
{
    return renderFlags;
}

void Renderer::setRenderFlags(uint64_t _renderFlags)
{
    renderFlags = _renderFlags;
    updateBodyVisibilityMask();
    markSettingsChanged();
}

int Renderer::getLabelMode() const
{
    return labelMode;
}

void Renderer::setLabelMode(int _labelMode)
{
    labelMode = _labelMode;
    markSettingsChanged();
}

int Renderer::getOrbitMask() const
{
    return orbitMask;
}

void Renderer::setOrbitMask(int mask)
{
    orbitMask = mask;
    markSettingsChanged();
}


const ColorTemperatureTable*
Renderer::getStarColorTable() const
{
    return colorTemp;
}


void
Renderer::setStarColorTable(const ColorTemperatureTable* ct)
{
    colorTemp = ct;
    markSettingsChanged();
}


bool Renderer::getVideoSync() const
{
    return videoSync;
}

void Renderer::setVideoSync(bool sync)
{
    videoSync = sync;
    markSettingsChanged();
}


float Renderer::getAmbientLightLevel() const
{
    return ambientLightLevel;
}


void Renderer::setAmbientLightLevel(float level)
{
    ambientLightLevel = level;
    markSettingsChanged();
}


float Renderer::getMinimumFeatureSize() const
{
    return minFeatureSize;
}


void Renderer::setMinimumFeatureSize(float pixels)
{
    minFeatureSize = pixels;
    markSettingsChanged();
}


float Renderer::getMinimumOrbitSize() const
{
    return minOrbitSize;
}

// Orbits and labels are only rendered when the orbit of the object
// occupies some minimum number of pixels on screen.
void Renderer::setMinimumOrbitSize(float pixels)
{
    minOrbitSize = pixels;
    markSettingsChanged();
}


float Renderer::getDistanceLimit() const
{
    return distanceLimit;
}


void Renderer::setDistanceLimit(float distanceLimit_)
{
    distanceLimit = distanceLimit_;
    markSettingsChanged();
}


void Renderer::addAnnotation(vector<Annotation>& annotations,
                             const MarkerRepresentation* markerRep,
                             const string& labelText,
                             Color color,
                             const Vector3f& pos,
                             LabelAlignment halign,
                             LabelVerticalAlignment valign,
                             float size,
                             bool special)
{
    GLint view[4] = { 0, 0, windowWidth, windowHeight };
    Vector3d win;
    Vector3d posd = pos.cast<double>();
    if (Project(posd, modelMatrix, projMatrix, view, win))
    {
        double depth = pos.x() * modelMatrix(2, 0) +
                       pos.y() * modelMatrix(2, 1) +
                       pos.z() * modelMatrix(2, 2);
        win.z() = -depth;

        Annotation a;
        if (!special || markerRep == nullptr)
             a.labelText = labelText;
        a.markerRep = markerRep;
        a.color = color;
        a.position = win.cast<float>();
        a.halign = halign;
        a.valign = valign;
        a.size = size;
        annotations.push_back(a);
    }
}


void Renderer::addForegroundAnnotation(const MarkerRepresentation* markerRep,
                                       const string& labelText,
                                       Color color,
                                       const Vector3f& pos,
                                       LabelAlignment halign,
                                       LabelVerticalAlignment valign,
                                       float size)
{
    addAnnotation(foregroundAnnotations, markerRep, labelText, color, pos, halign, valign, size);
}


void Renderer::addBackgroundAnnotation(const MarkerRepresentation* markerRep,
                                       const string& labelText,
                                       Color color,
                                       const Vector3f& pos,
                                       LabelAlignment halign,
                                       LabelVerticalAlignment valign,
                                       float size)
{
    addAnnotation(backgroundAnnotations, markerRep, labelText, color, pos, halign, valign, size);
}


void Renderer::addSortedAnnotation(const MarkerRepresentation* markerRep,
                                   const string& labelText,
                                   Color color,
                                   const Vector3f& pos,
                                   LabelAlignment halign,
                                   LabelVerticalAlignment valign,
                                   float size)
{
    addAnnotation(depthSortedAnnotations, markerRep, labelText, color, pos, halign, valign, size, true);
}


void Renderer::clearAnnotations(vector<Annotation>& annotations)
{
    annotations.clear();
}


void Renderer::clearSortedAnnotations()
{
    depthSortedAnnotations.clear();
}


// Return the orientation of the camera used to render the current
// frame. Available only while rendering a frame.
Quaternionf Renderer::getCameraOrientation() const
{
    return m_cameraOrientation;
}


float Renderer::getNearPlaneDistance() const
{
    return depthPartitions[currentIntervalIndex].nearZ;
}


void Renderer::beginObjectAnnotations()
{
    // It's an error to call beginObjectAnnotations a second time
    // without first calling end.
    assert(!objectAnnotationSetOpen);
    assert(objectAnnotations.empty());

    objectAnnotations.clear();
    objectAnnotationSetOpen = true;
}


void Renderer::endObjectAnnotations()
{
    objectAnnotationSetOpen = false;

    if (!objectAnnotations.empty())
    {
        renderAnnotations(objectAnnotations.begin(),
                          objectAnnotations.end(),
                          -depthPartitions[currentIntervalIndex].nearZ,
                          -depthPartitions[currentIntervalIndex].farZ,
                          FontNormal);

        objectAnnotations.clear();
    }
}


void Renderer::addObjectAnnotation(const MarkerRepresentation* markerRep,
                                   const string& labelText,
                                   Color color,
                                   const Vector3f& pos)
{
    assert(objectAnnotationSetOpen);
    if (objectAnnotationSetOpen)
    {
        addAnnotation(objectAnnotations, markerRep, labelText, color, pos, AlignCenter, VerticalAlignCenter);
    }
}


static void enableSmoothLines()
{
    // glEnable(GL_BLEND);
#ifdef USE_HDR
    glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
#else
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#endif
    glEnable(GL_LINE_SMOOTH);
    glLineWidth(1.5f);
}

static void disableSmoothLines()
{
    // glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDisable(GL_LINE_SMOOTH);
    glLineWidth(1.0f);
}


inline void enableSmoothLines(uint64_t renderFlags)
{
    if ((renderFlags & Renderer::ShowSmoothLines) != 0)
        enableSmoothLines();
}


inline void disableSmoothLines(uint64_t renderFlags)
{
    if ((renderFlags & Renderer::ShowSmoothLines) != 0)
        disableSmoothLines();
}

class OrbitSampler : public OrbitSampleProc
{
public:
    vector<CurvePlotSample> samples;

    OrbitSampler() = default;

    void sample(double t, const Vector3d& position, const Vector3d& velocity)
    {
        CurvePlotSample samp;
        samp.t = t;
        samp.position = position;
        samp.velocity = velocity;
        samples.push_back(samp);
    }

    void insertForward(CurvePlot* plot)
    {
        for (const auto& sample : samples)
        {
            plot->addSample(sample);
        }
    }

    void insertBackward(CurvePlot* plot)
    {
        for (auto iter = samples.rbegin(); iter != samples.rend(); ++iter)
        {
            plot->addSample(*iter);
        }
    }
};


Vector4f renderOrbitColor(const Body *body, bool selected, float opacity)
{
    Color orbitColor;

    if (selected)
    {
        // Highlight the orbit of the selected object in red
        orbitColor = Renderer::SelectionOrbitColor;
    }
    else if (body != nullptr && body->isOrbitColorOverridden())
    {
        orbitColor = body->getOrbitColor();
    }
    else
    {
        int classification;
        if (body != nullptr)
            classification = body->getOrbitClassification();
        else
            classification = Body::Stellar;

        switch (classification)
        {
        case Body::Moon:
            orbitColor = Renderer::MoonOrbitColor;
            break;
        case Body::MinorMoon:
            orbitColor = Renderer::MinorMoonOrbitColor;
            break;
        case Body::Asteroid:
            orbitColor = Renderer::AsteroidOrbitColor;
            break;
        case Body::Comet:
            orbitColor = Renderer::CometOrbitColor;
            break;
        case Body::Spacecraft:
            orbitColor = Renderer::SpacecraftOrbitColor;
            break;
        case Body::Stellar:
            orbitColor = Renderer::StarOrbitColor;
            break;
        case Body::DwarfPlanet:
            orbitColor = Renderer::DwarfPlanetOrbitColor;
            break;
        case Body::Planet:
        default:
            orbitColor = Renderer::PlanetOrbitColor;
            break;
        }
    }

#ifdef USE_HDR
    return Vector4f(orbitColor.red(), orbitColor.green(), orbitColor.blue(), 1.0f - opacity * orbitColor.alpha());
#else
    return Vector4f(orbitColor.red(), orbitColor.green(), orbitColor.blue(), opacity * orbitColor.alpha());
#endif
}


static int orbitsRendered = 0;
static int orbitsSkipped = 0;
static int sectionsCulled = 0;

void Renderer::renderOrbit(const OrbitPathListEntry& orbitPath,
                           double t,
                           const Quaterniond& cameraOrientation,
                           const Frustum& frustum,
                           float nearDist,
                           float farDist)
{
    auto *prog = shaderManager->getShader(ShaderProperties::PerVertexColor);
    if (prog == nullptr)
        return;

    Body* body = orbitPath.body;
    double nearZ = -nearDist;  // negate, becase z is into the screen in camera space
    double farZ = -farDist;

    const Orbit* orbit = nullptr;
    if (body != nullptr)
        orbit = body->getOrbit(t);
    else
        orbit = orbitPath.star->getOrbit();

    CurvePlot* cachedOrbit = nullptr;
    OrbitCache::iterator cached = orbitCache.find(orbit);
    if (cached != orbitCache.end())
    {
        cachedOrbit = cached->second;
        cachedOrbit->setLastUsed(frameCount);
    }

    // If it's not in the cache already
    if (cachedOrbit == nullptr)
    {
        double startTime = t;
        int nSamples = detailOptions.orbitPathSamplePoints;

        // Adjust the number of samples used for aperiodic orbits--these aren't
        // true orbits, but are sampled trajectories, generally of spacecraft.
        // Better control is really needed--some sort of adaptive sampling would
        // be ideal.
        if (!orbit->isPeriodic())
        {
            double begin = 0.0, end = 0.0;
            orbit->getValidRange(begin, end);

            if (begin != end)
            {
                startTime = begin;
                nSamples = (int) (orbit->getPeriod() * 100.0);
                nSamples = max(min(nSamples, 1000), 100);
            }
            else
            {
                // If the orbit is aperiodic and doesn't have a
                // finite duration, we don't render it. A compromise
                // would be to pick some time window centered at the
                // current time, but we'd have to pick some arbitrary
                // duration.
                nSamples = 0;
            }
        }
        else
        {
            startTime = t - orbit->getPeriod();
        }

        cachedOrbit = new CurvePlot();
        cachedOrbit->setLastUsed(frameCount);

        OrbitSampler sampler;
        orbit->sample(startTime,
                      startTime + orbit->getPeriod(),
                      sampler);
        sampler.insertForward(cachedOrbit);

        // If the orbit cache is full, first try and eliminate some old orbits
        if (orbitCache.size() > OrbitCacheCullThreshold)
        {
            // Check for old orbits at most once per frame
            if (lastOrbitCacheFlush != frameCount)
            {
                for (auto iter = orbitCache.begin(); iter != orbitCache.end();)
                {
                    // Tricky code to eliminate a node in the orbit cache without screwing
                    // up the iterator. Should work in all STL implementations.
                    if (frameCount - iter->second->lastUsed() > OrbitCacheRetireAge)
                        orbitCache.erase(iter++);
                    else
                        ++iter;
                }
                lastOrbitCacheFlush = frameCount;
            }
        }

        orbitCache[orbit] = cachedOrbit;
    }

    if (cachedOrbit->empty())
        return;

    //*** Orbit rendering parameters

    // The 'window' is the interval of time for which the orbit will be drawn.

    // End of the orbit window relative to the current simulation time. Units
    // are orbital periods. The default value is 0.5.
    const double OrbitWindowEnd     = detailOptions.orbitWindowEnd;

    // Number of orbit periods shown. The orbit window is:
    //    [ t + (OrbitWindowEnd - OrbitPeriodsShown) * T, t + OrbitWindowEnd * T ]
    // where t is the current simulation time and T is the orbital period.
    // The default value is 1.0.
    const double OrbitPeriodsShown  = detailOptions.orbitPeriodsShown;

    // Fraction of the window over which the orbit fades from opaque to transparent.
    // Fading is disabled when this value is zero.
    // The default value is 0.0.
    const double LinearFadeFraction = detailOptions.linearFadeFraction;

    // Extra size of the internal sample cache.
    const double WindowSlack        = 0.2;

    //***

    // 'Periodic' orbits are generally not strictly periodic because of perturbations
    // from other bodies. Here we update the trajectory samples to make sure that the
    // orbit covers a time range centered at the current time and covering a full revolution.
    if (orbit->isPeriodic())
    {
        double period = orbit->getPeriod();
        double endTime = t + period * OrbitWindowEnd;
        double startTime = endTime - period * OrbitPeriodsShown;

        double currentWindowStart = cachedOrbit->startTime();
        double currentWindowEnd = cachedOrbit->endTime();
        double newWindowStart = startTime - period * WindowSlack;
        double newWindowEnd = endTime + period * WindowSlack;

        if (startTime < currentWindowStart)
        {
            // Remove samples at the end of the time window
            cachedOrbit->removeSamplesAfter(newWindowEnd);

            // Trim the first sample (because it will be duplicated when we sample the orbit.)
            cachedOrbit->removeSamplesBefore(cachedOrbit->startTime() * (1.0 + 1.0e-15));

            // Add the new samples
            OrbitSampler sampler;
            orbit->sample(newWindowStart, min(currentWindowStart, newWindowEnd), sampler);
            sampler.insertBackward(cachedOrbit);
#if DEBUG_ORBIT_CACHE
            clog << "new sample count: " << cachedOrbit->sampleCount() << endl;
#endif
        }
        else if (endTime > currentWindowEnd)
        {
            // Remove samples at the beginning of the time window
            cachedOrbit->removeSamplesBefore(newWindowStart);

            // Trim the last sample (because it will be duplicated when we sample the orbit.)
            cachedOrbit->removeSamplesAfter(cachedOrbit->endTime() * (1.0 - 1.0e-15));

            // Add the new samples
            OrbitSampler sampler;
            orbit->sample(max(currentWindowEnd, newWindowStart), newWindowEnd, sampler);
            sampler.insertForward(cachedOrbit);
#if DEBUG_ORBIT_CACHE
            clog << "new sample count: " << cachedOrbit->sampleCount() << endl;
#endif
        }
    }

    // We perform vertex tranformations on the CPU because double precision is necessary to
    // render orbits properly. Start by computing the modelview matrix, to transform orbit
    // vertices into camera space.
    Affine3d modelview;
    {
        Quaterniond orientation = Quaterniond::Identity();
        if (body)
        {
            orientation = body->getOrbitFrame(t)->getOrientation(t);
        }

        modelview = cameraOrientation * Translation3d(orbitPath.origin) * orientation.conjugate();
    }

    glPushMatrix();
    glLoadIdentity();

    bool highlight;
    if (body != nullptr)
        highlight = highlightObject.body() == body;
    else
        highlight = highlightObject.star() == orbitPath.star;
    Vector4f orbitColor = renderOrbitColor(body, highlight, orbitPath.opacity);

#ifdef STIPPLED_LINES
    glLineStipple(3, 0x5555);
    glEnable(GL_LINE_STIPPLE);
#endif

    double subdivisionThreshold = pixelSize * 40.0;

    Eigen::Vector3d viewFrustumPlaneNormals[4];
    for (int i = 0; i < 4; i++)
    {
        viewFrustumPlaneNormals[i] = frustum.plane(i).normal().cast<double>();
    }

    prog->use();
    if (orbit->isPeriodic())
    {
        double period = orbit->getPeriod();
        double windowEnd = t + period * OrbitWindowEnd;
        double windowStart = windowEnd - period * OrbitPeriodsShown;
        double windowDuration = windowEnd - windowStart;

        if (LinearFadeFraction == 0.0f || (renderFlags & ShowFadingOrbits) == 0)
        {
            cachedOrbit->render(modelview,
                                nearZ, farZ, viewFrustumPlaneNormals,
                                subdivisionThreshold,
                                windowStart, windowEnd,
                                orbitColor);
        }
        else
        {
            cachedOrbit->renderFaded(modelview,
                                     nearZ, farZ, viewFrustumPlaneNormals,
                                     subdivisionThreshold,
                                     windowStart, windowEnd,
                                     orbitColor,
                                     windowStart,
                                     windowEnd - windowDuration * (1.0 - LinearFadeFraction));
        }
    }
    else
    {
        if ((renderFlags & ShowPartialTrajectories) != 0)
        {
            // Show the trajectory from the start time until the current simulation time
            cachedOrbit->render(modelview,
                                nearZ, farZ, viewFrustumPlaneNormals,
                                subdivisionThreshold,
                                cachedOrbit->startTime(), t,
                                orbitColor);
        }
        else
        {
            // Show the entire trajectory
            cachedOrbit->render(modelview,
                                nearZ, farZ, viewFrustumPlaneNormals,
                                subdivisionThreshold,
                                orbitColor);
        }
    }

#ifdef STIPPLED_LINES
    glDisable(GL_LINE_STIPPLE);
#endif

    glUseProgram(0);
    glPopMatrix();
}


// Convert a position in the universal coordinate system to astrocentric
// coordinates, taking into account possible orbital motion of the star.
static Vector3d astrocentricPosition(const UniversalCoord& pos,
                                     const Star& star,
                                     double t)
{
    return pos.offsetFromKm(star.getPosition(t));
}


void Renderer::autoMag(float& faintestMag)
{
      float fieldCorr = 2.0f * FOV/(fov + FOV);
      faintestMag = (float) (faintestAutoMag45deg * sqrt(fieldCorr));
      saturationMag = saturationMagNight * (1.0f + fieldCorr * fieldCorr);
}


// Set up the light sources for rendering a solar system.  The positions of
// all nearby stars are converted from universal to viewer-centered
// coordinates.
static void
setupLightSources(const vector<const Star*>& nearStars,
                  const UniversalCoord& observerPos,
                  double t,
                  vector<LightSource>& lightSources,
                  uint64_t renderFlags)
{
    lightSources.clear();

    for (const auto star : nearStars)
    {
        if (star->getVisibility())
        {
            Vector3d v = star->getPosition(t).offsetFromKm(observerPos);
            LightSource ls;
            ls.position = v;
            ls.luminosity = star->getLuminosity();
            ls.radius = star->getRadius();

            if (renderFlags & Renderer::ShowTintedIllumination)
            {
                // If the star is sufficiently cool, change the light color
                // from white.  Though our sun appears yellow, we still make
                // it and all hotter stars emit white light, as this is the
                // 'natural' light to which our eyes are accustomed.  We also
                // assign a slight bluish tint to light from O and B type stars,
                // though these will almost never have planets for their light
                // to shine upon.
                float temp = star->getTemperature();
                if (temp > 30000.0f)
                    ls.color = Color(0.8f, 0.8f, 1.0f);
                else if (temp > 10000.0f)
                    ls.color = Color(0.9f, 0.9f, 1.0f);
                else if (temp > 5400.0f)
                    ls.color = Color(1.0f, 1.0f, 1.0f);
                else if (temp > 3900.0f)
                    ls.color = Color(1.0f, 0.9f, 0.8f);
                else if (temp > 2000.0f)
                    ls.color = Color(1.0f, 0.7f, 0.7f);
                else
                    ls.color = Color(1.0f, 0.4f, 0.4f);
            }
            else
            {
                ls.color = Color(1.0f, 1.0f, 1.0f);
            }

            lightSources.push_back(ls);
        }
    }
}


// Set up the potential secondary light sources for rendering solar system
// bodies.
static void
setupSecondaryLightSources(vector<SecondaryIlluminator>& secondaryIlluminators,
                           const vector<LightSource>& primaryIlluminators)
{
    float au2 = square(astro::kilometersToAU(1.0f));

    for (auto& i : secondaryIlluminators)
    {
        i.reflectedIrradiance = 0.0f;

        for (const auto& j : primaryIlluminators)
        {
            i.reflectedIrradiance += j.luminosity / ((float) (i.position_v - j.position).squaredNorm() * au2);
        }

        i.reflectedIrradiance *= i.body->getAlbedo();
    }
}


// Render an item from the render list
void Renderer::renderItem(const RenderListEntry& rle,
                          const Observer& observer,
                          const Quaternionf& cameraOrientation,
                          float nearPlaneDistance,
                          float farPlaneDistance)
{
    switch (rle.renderableType)
    {
    case RenderListEntry::RenderableStar:
        renderStar(*rle.star,
                   rle.position,
                   rle.distance,
                   rle.appMag,
                   cameraOrientation,
                   observer.getTime(),
                   nearPlaneDistance, farPlaneDistance);
        break;

    case RenderListEntry::RenderableBody:
        renderPlanet(*rle.body,
                     rle.position,
                     rle.distance,
                     rle.appMag,
                     observer,
                     cameraOrientation,
                     nearPlaneDistance, farPlaneDistance);
        break;

    case RenderListEntry::RenderableCometTail:
        renderCometTail(*rle.body,
                        rle.position,
                        observer,
                        rle.discSizeInPixels);
        break;

    case RenderListEntry::RenderableReferenceMark:
        renderReferenceMark(*rle.refMark,
                            rle.position,
                            rle.distance,
                            observer.getTime(),
                            nearPlaneDistance);
        break;

    default:
        break;
    }
}


#ifdef USE_HDR
void Renderer::genBlurTextures()
{
    for (size_t i = 0; i < BLUR_PASS_COUNT; ++i)
    {
        if (blurTextures[i] != nullptr)
        {
            delete blurTextures[i];
            blurTextures[i] = nullptr;
        }
    }
    if (blurTempTexture)
    {
        delete blurTempTexture;
        blurTempTexture = nullptr;
    }

    blurBaseWidth = sceneTexWidth, blurBaseHeight = sceneTexHeight;

    if (blurBaseWidth > blurBaseHeight)
    {
        while (blurBaseWidth > BLUR_SIZE)
        {
            blurBaseWidth  >>= 1;
            blurBaseHeight >>= 1;
        }
    }
    else
    {
        while (blurBaseHeight > BLUR_SIZE)
        {
            blurBaseWidth  >>= 1;
            blurBaseHeight >>= 1;
        }
    }
    genBlurTexture(0);
    genBlurTexture(1);

    Image *tempImg;
    ImageTexture *tempTexture;
    tempImg = new Image(GL_LUMINANCE, blurBaseWidth, blurBaseHeight);
    tempTexture = new ImageTexture(*tempImg, Texture::EdgeClamp, Texture::DefaultMipMaps);
    delete tempImg;
    if (tempTexture && tempTexture->getName() != 0)
        blurTempTexture = tempTexture;
}

void Renderer::genBlurTexture(int blurLevel)
{
    Image *img;
    ImageTexture *texture;

#ifdef DEBUG_HDR
    HDR_LOG <<
        "Window width = "    << windowWidth << ", " <<
        "Window height = "   << windowHeight << ", " <<
        "Blur tex width = "  << (blurBaseWidth>>blurLevel) << ", " <<
        "Blur tex height = " << (blurBaseHeight>>blurLevel) << endl;
#endif
    img = new Image(blurFormat,
                    blurBaseWidth>>blurLevel,
                    blurBaseHeight>>blurLevel);
    texture = new ImageTexture(*img,
                               Texture::EdgeClamp,
                               Texture::NoMipMaps);
    delete img;

    if (texture && texture->getName() != 0)
        blurTextures[blurLevel] = texture;
}

void Renderer::genSceneTexture()
{
    unsigned int *data;
    if (sceneTexture != 0)
        glDeleteTextures(1, &sceneTexture);

    sceneTexWidth  = 1;
    sceneTexHeight = 1;
    while (sceneTexWidth < windowWidth)
        sceneTexWidth <<= 1;
    while (sceneTexHeight < windowHeight)
        sceneTexHeight <<= 1;
    sceneTexWScale = (windowWidth > 0)  ? (GLfloat)sceneTexWidth  / (GLfloat)windowWidth :
        1.0f;
    sceneTexHScale = (windowHeight > 0) ? (GLfloat)sceneTexHeight / (GLfloat)windowHeight :
        1.0f;
    data = (unsigned int* )malloc(sceneTexWidth*sceneTexHeight*4*sizeof(unsigned int));
    memset(data, 0, sceneTexWidth*sceneTexHeight*4*sizeof(unsigned int));

    glGenTextures(1, &sceneTexture);
    glBindTexture(GL_TEXTURE_2D, sceneTexture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sceneTexWidth, sceneTexHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);

    free(data);
#ifdef DEBUG_HDR
    static int genSceneTexCounter = 1;
    HDR_LOG <<
        "[" << genSceneTexCounter++ << "] " <<
        "Window width = "  << windowWidth << ", " <<
        "Window height = " << windowHeight << ", " <<
        "Tex width = "  << sceneTexWidth << ", " <<
        "Tex height = " << sceneTexHeight << endl;
#endif
}

void Renderer::renderToBlurTexture(int blurLevel)
{
    if (blurTextures[blurLevel] == nullptr)
        return;
    GLsizei blurTexWidth  = blurBaseWidth>>blurLevel;
    GLsizei blurTexHeight = blurBaseHeight>>blurLevel;
    GLsizei blurDrawWidth = (GLfloat)windowWidth/(GLfloat)sceneTexWidth * blurTexWidth;
    GLsizei blurDrawHeight = (GLfloat)windowHeight/(GLfloat)sceneTexHeight * blurTexHeight;
    GLfloat blurWScale = 1.f;
    GLfloat blurHScale = 1.f;
    GLfloat savedWScale = 1.f;
    GLfloat savedHScale = 1.f;

    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT);
    glClearColor(0, 0, 0, 1.f);
    glViewport(0, 0, blurDrawWidth, blurDrawHeight);
    glBindTexture(GL_TEXTURE_2D, sceneTexture);

    glBegin(GL_QUADS);
    drawBlendedVertices(0.0f, 0.0f, 1.0f);
    glEnd();
    // Do not need to scale alpha so mask it off
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_BLEND);
    savedWScale = sceneTexWScale;
    savedHScale = sceneTexHScale;

    // Remove ldr part of image
    {
    const GLfloat bias  = -0.5f;
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquationEXT(GL_FUNC_REVERSE_SUBTRACT_EXT);
    glColor4f(-bias, -bias, -bias, 0.0f);

    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glVertex2f(0.0f, 0.0f);
    glVertex2f(1.f,  0.0f);
    glVertex2f(1.f,  1.f);
    glVertex2f(0.0f, 1.f);
    glEnd();

    glEnable(GL_TEXTURE_2D);
    blurTextures[blurLevel]->bind();
    glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                     blurTexWidth, blurTexHeight, 0);
    }

    // Scale back up hdr part
    {
    glBlendEquationEXT(GL_FUNC_ADD_EXT);
    glBlendFunc(GL_DST_COLOR, GL_ONE);

    glBegin(GL_QUADS);
    drawBlendedVertices(0.f, 0.f, 1.f); //x2
    drawBlendedVertices(0.f, 0.f, 1.f); //x2
    glEnd();
    }

    glDisable(GL_BLEND);

    if (!useLuminanceAlpha)
    {
        blurTempTexture->bind();
        glCopyTexImage2D(GL_TEXTURE_2D, blurLevel, GL_LUMINANCE, 0, 0,
                         blurTexWidth, blurTexHeight, 0);
        // Erase color, replace with luminance image
        glBegin(GL_QUADS);
        glColor4f(0.f, 0.f, 0.f, 1.f);
        glVertex2f(0.0f, 0.0f);
        glVertex2f(1.0f, 0.0f);
        glVertex2f(1.0f, 1.0f);
        glVertex2f(0.0f, 1.0f);
        glEnd();
        glBegin(GL_QUADS);
        drawBlendedVertices(0.f, 0.f, 1.f);
        glEnd();
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    blurTextures[blurLevel]->bind();
    glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                     blurTexWidth, blurTexHeight, 0);
// blending end

    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GLfloat xdelta = 1.0f / (GLfloat)blurTexWidth;
    GLfloat ydelta = 1.0f / (GLfloat)blurTexHeight;
    blurWScale = ((GLfloat)blurTexWidth / (GLfloat)blurDrawWidth);
    blurHScale = ((GLfloat)blurTexHeight / (GLfloat)blurDrawHeight);
    sceneTexWScale = blurWScale;
    sceneTexHScale = blurHScale;

    // Butterworth low pass filter to reduce flickering dots
    {
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f,    0.0f, .5f*1.f);
        drawBlendedVertices(-xdelta, 0.0f, .5f*0.333f);
        drawBlendedVertices( xdelta, 0.0f, .5f*0.25f);
        glEnd();
        glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                         blurTexWidth, blurTexHeight, 0);
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, -ydelta, .5f*0.667f);
        drawBlendedVertices(0.0f,  ydelta, .5f*0.333f);
        glEnd();
        glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                         blurTexWidth, blurTexHeight, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Gaussian blur
    switch (blurLevel)
    {
/*
    case 0:
        drawGaussian3x3(xdelta, ydelta, blurTexWidth, blurTexHeight, 1.f);
        break;
*/
#ifdef __APPLE__
    case 0:
        drawGaussian5x5(xdelta, ydelta, blurTexWidth, blurTexHeight, 1.f);
        break;
    case 1:
        drawGaussian9x9(xdelta, ydelta, blurTexWidth, blurTexHeight, .3f);
        break;
#else
    // Gamma correct: windows=(mac^1.8)^(1/2.2)
    case 0:
        drawGaussian5x5(xdelta, ydelta, blurTexWidth, blurTexHeight, 1.f);
        break;
    case 1:
        drawGaussian9x9(xdelta, ydelta, blurTexWidth, blurTexHeight, .373f);
        break;
#endif
    default:
        break;
    }

    blurTextures[blurLevel]->bind();
    glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                     blurTexWidth, blurTexHeight, 0);

    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);
    glPopAttrib();
    sceneTexWScale = savedWScale;
    sceneTexHScale = savedHScale;
}

void Renderer::renderToTexture(const Observer& observer,
                               const Universe& universe,
                               float faintestMagNight,
                               const Selection& sel)
{
    if (sceneTexture == 0)
        return;
    glPushAttrib(GL_COLOR_BUFFER_BIT);

    draw(observer, universe, faintestMagNight, sel);

    glBindTexture(GL_TEXTURE_2D, sceneTexture);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0,
                     sceneTexWidth, sceneTexHeight, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPopAttrib();
}

void Renderer::drawSceneTexture()
{
    if (sceneTexture == 0)
        return;
    glBindTexture(GL_TEXTURE_2D, sceneTexture);
    glBegin(GL_QUADS);
    drawBlendedVertices(0.0f, 0.0f, 1.0f);
    glEnd();
}

void Renderer::drawBlendedVertices(float xdelta, float ydelta, float blend)
{
    glColor4f(1.0f, 1.0f, 1.0f, blend);
    glTexCoord2i(0, 0); glVertex2f(xdelta,                ydelta);
    glTexCoord2i(1, 0); glVertex2f(sceneTexWScale+xdelta, ydelta);
    glTexCoord2i(1, 1); glVertex2f(sceneTexWScale+xdelta, sceneTexHScale+ydelta);
    glTexCoord2i(0, 1); glVertex2f(xdelta,                sceneTexHScale+ydelta);
}

void Renderer::drawGaussian3x3(float xdelta, float ydelta, GLsizei width, GLsizei height, float blend)
{
#ifdef USE_BLOOM_LISTS
    if (gaussianLists[0] == 0)
    {
        gaussianLists[0] = glGenLists(1);
        glNewList(gaussianLists[0], GL_COMPILE);
#endif
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, 0.0f, blend);
        drawBlendedVertices(-xdelta, 0.0f, 0.25f*blend);
        drawBlendedVertices( xdelta, 0.0f, 0.20f*blend);
        glEnd();

        // Take result of horiz pass and apply vertical pass
        glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                         width, height, 0);
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, -ydelta, 0.429f);
        drawBlendedVertices(0.0f,  ydelta, 0.300f);
        glEnd();
#ifdef USE_BLOOM_LISTS
        glEndList();
    }
    glCallList(gaussianLists[0]);
#endif
}

void Renderer::drawGaussian5x5(float xdelta, float ydelta, GLsizei width, GLsizei height, float blend)
{
#ifdef USE_BLOOM_LISTS
    if (gaussianLists[1] == 0)
    {
        gaussianLists[1] = glGenLists(1);
        glNewList(gaussianLists[1], GL_COMPILE);
#endif
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, 0.0f, blend);
        drawBlendedVertices(-xdelta,      0.0f, 0.475f*blend);
        drawBlendedVertices( xdelta,      0.0f, 0.475f*blend);
        drawBlendedVertices(-2.0f*xdelta, 0.0f, 0.075f*blend);
        drawBlendedVertices( 2.0f*xdelta, 0.0f, 0.075f*blend);
        glEnd();
        glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                         width, height, 0);
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, -ydelta,      0.475f);
        drawBlendedVertices(0.0f,  ydelta,      0.475f);
        drawBlendedVertices(0.0f, -2.0f*ydelta, 0.075f);
        drawBlendedVertices(0.0f,  2.0f*ydelta, 0.075f);
        glEnd();
#ifdef USE_BLOOM_LISTS
        glEndList();
    }
    glCallList(gaussianLists[1]);
#endif
}

void Renderer::drawGaussian9x9(float xdelta, float ydelta, GLsizei width, GLsizei height, float blend)
{
#ifdef USE_BLOOM_LISTS
    if (gaussianLists[2] == 0)
    {
        gaussianLists[2] = glGenLists(1);
        glNewList(gaussianLists[2], GL_COMPILE);
#endif
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, 0.0f, blend);
        drawBlendedVertices(-xdelta,      0.0f, 0.632f*blend);
        drawBlendedVertices( xdelta,      0.0f, 0.632f*blend);
        drawBlendedVertices(-2.0f*xdelta, 0.0f, 0.159f*blend);
        drawBlendedVertices( 2.0f*xdelta, 0.0f, 0.159f*blend);
        drawBlendedVertices(-3.0f*xdelta, 0.0f, 0.016f*blend);
        drawBlendedVertices( 3.0f*xdelta, 0.0f, 0.016f*blend);
        glEnd();

        glCopyTexImage2D(GL_TEXTURE_2D, 0, blurFormat, 0, 0,
                         width, height, 0);
        glBegin(GL_QUADS);
        drawBlendedVertices(0.0f, -ydelta,      0.632f);
        drawBlendedVertices(0.0f,  ydelta,      0.632f);
        drawBlendedVertices(0.0f, -2.0f*ydelta, 0.159f);
        drawBlendedVertices(0.0f,  2.0f*ydelta, 0.159f);
        drawBlendedVertices(0.0f, -3.0f*ydelta, 0.016f);
        drawBlendedVertices(0.0f,  3.0f*ydelta, 0.016f);
        glEnd();
#ifdef USE_BLOOM_LISTS
        glEndList();
    }
    glCallList(gaussianLists[2]);
#endif
}

void Renderer::drawBlur()
{
    blurTextures[0]->bind();
    glBegin(GL_QUADS);
    drawBlendedVertices(0.0f, 0.0f, 1.0f);
    glEnd();
    blurTextures[1]->bind();
    glBegin(GL_QUADS);
    drawBlendedVertices(0.0f, 0.0f, 1.0f);
    glEnd();
}

bool Renderer::getBloomEnabled()
{
    return bloomEnabled;
}

void Renderer::setBloomEnabled(bool aBloomEnabled)
{
    bloomEnabled = aBloomEnabled;
}

void Renderer::increaseBrightness()
{
    brightPlus += 1.0f;
}

void Renderer::decreaseBrightness()
{
    brightPlus -= 1.0f;
}

float Renderer::getBrightness()
{
    return brightPlus;
}
#endif // USE_HDR

void Renderer::render(const Observer& observer,
                      const Universe& universe,
                      float faintestMagNight,
                      const Selection& sel)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

#ifdef USE_HDR
    renderToTexture(observer, universe, faintestMagNight, sel);

    //------------- Post processing from here ------------//
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrix(Ortho2D(0.0f, 1.0f, 0.0f, 1.0f));
    glMatrixMode (GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    if (bloomEnabled)
    {
        renderToBlurTexture(0);
        renderToBlurTexture(1);
//        renderToBlurTexture(2);
    }

    drawSceneTexture();

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

#ifdef HDR_COMPRESS
    // Assume luminance 1.0 mapped to 128 previously
    // Compositing a 2nd copy doubles 128->255
    drawSceneTexture();
#endif

    if (bloomEnabled)
    {
        drawBlur();
    }

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
#else
    draw(observer, universe, faintestMagNight, sel);
#endif
}

void Renderer::draw(const Observer& observer,
                    const Universe& universe,
                    float faintestMagNight,
                    const Selection& sel)
{
    // Get the observer's time
    double now = observer.getTime();
    realTime = observer.getRealTime();

    frameCount++;
    settingsChanged = false;

    // Compute the size of a pixel
    setFieldOfView(radToDeg(observer.getFOV()));
    pixelSize = calcPixelSize(fov, (float) windowHeight);

    // Set up the projection we'll use for rendering stars.
    glMatrix(Perspective(fov, getAspectRatio(), NEAR_DIST, FAR_DIST));

    // Set the modelview matrix
    glMatrixMode(GL_MODELVIEW);

    // Get the displayed surface texture set to use from the observer
    displayedSurface = observer.getDisplayedSurface();

    locationFilter = observer.getLocationFilter();

    // Highlight the selected object
    highlightObject = sel;

    m_cameraOrientation = observer.getOrientationf();

    // Get the view frustum used for culling in camera space.
    Frustum frustum(degToRad(fov),
                    getAspectRatio(),
                    MinNearPlaneDistance);

    // Get the transformed frustum, used for culling in the astrocentric coordinate
    // system.
    Frustum xfrustum(frustum);
    xfrustum.transform(observer.getOrientationf().conjugate().toRotationMatrix());

    // Set up the camera for star rendering; the units of this phase
    // are light years.
    Vector3f observerPosLY = observer.getPosition().offsetFromLy(Vector3f::Zero());
    glPushMatrix();
    glRotate(m_cameraOrientation);

    // Get the model matrix *before* translation.  We'll use this for
    // positioning star and planet labels.
    glGetDoublev(GL_MODELVIEW_MATRIX, modelMatrix.data());
    glGetDoublev(GL_PROJECTION_MATRIX, projMatrix.data());

    clearSortedAnnotations();

    // Put all solar system bodies into the render list.  Stars close and
    // large enough to have discernible surface detail are also placed in
    // renderList.
    renderList.clear();
    orbitPathList.clear();
    lightSourceList.clear();
    secondaryIlluminators.clear();

    // See if we want to use AutoMag.
    if ((renderFlags & ShowAutoMag) != 0)
    {
        autoMag(faintestMag);
    }
    else
    {
        faintestMag = faintestMagNight;
        saturationMag = saturationMagNight;
    }

    faintestPlanetMag = faintestMag;
#ifdef USE_HDR
    float maxBodyMagPrev = saturationMag;
    maxBodyMag = min(maxBodyMag, saturationMag);
    vector<RenderListEntry>::iterator closestBody;
    const Star *brightestStar = nullptr;
    bool foundClosestBody   = false;
    bool foundBrightestStar = false;
#endif

    if ((renderFlags & (ShowSSO | ShowOrbits)) != 0)
    {
        nearStars.clear();
        universe.getNearStars(observer.getPosition(), SolarSystemMaxDistance, nearStars);

        // Set up direct light sources (i.e. just stars at the moment)
        // Skip if only star orbits to be shown
        if ((renderFlags & ShowSSO) != 0)
            setupLightSources(nearStars, observer.getPosition(), now, lightSourceList, renderFlags);

        // Traverse the frame trees of each nearby solar system and
        // build the list of objects to be rendered.
        for (const auto sun : nearStars)
        {
            addStarOrbitToRenderList(*sun, observer, now);
            // Skip if only star orbits to be shown
            if ((renderFlags & ShowSSO) == 0)
                continue;

            SolarSystem* solarSystem = universe.getSolarSystem(sun);
            if (solarSystem == nullptr)
                continue;

            FrameTree* solarSysTree = solarSystem->getFrameTree();
            if (solarSysTree == nullptr)
                continue;

            if (solarSysTree->updateRequired())
            {
                // Tree has changed, so we must recompute bounding spheres.
                solarSysTree->recomputeBoundingSphere();
                solarSysTree->markUpdated();
            }

            // Compute the position of the observer in astrocentric coordinates
            Vector3d astrocentricObserverPos = astrocentricPosition(observer.getPosition(), *sun, now);

            // Build render lists for bodies and orbits paths
            buildRenderLists(astrocentricObserverPos,
                             xfrustum,
                             observer.getOrientation().conjugate() * -Vector3d::UnitZ(),
                             Vector3d::Zero(),
                             solarSysTree,
                             observer,
                             now);
            if ((renderFlags & ShowOrbits) != 0)
            {
                buildOrbitLists(astrocentricObserverPos,
                                observer.getOrientation(),
                                xfrustum,
                                solarSysTree,
                                now);
            }
        }

        if ((labelMode & BodyLabelMask) != 0)
            buildLabelLists(xfrustum, now);

        starTex->bind();
    }

    setupSecondaryLightSources(secondaryIlluminators, lightSourceList);

#ifdef USE_HDR
    Matrix3f viewMat = observer.getOrientationf().conjugate().toRotationMatrix();
    float maxSpan = (float) sqrt(square((float) windowWidth) +
                                 square((float) windowHeight));
    float nearZcoeff = (float) cos(degToRad(fov / 2)) *
        ((float) windowHeight / maxSpan);

    // Remove objects from the render list that lie completely outside the
    // view frustum.
    auto notCulled = renderList.begin();
    for (const auto& render_item : renderList)
    {
        Vector3f center = viewMat * render_item.position;

        bool convex = true;
        float radius = 1.0f;
        float cullRadius = 1.0f;
        float cloudHeight = 0.0f;

        switch (render_item.renderableType)
        {
        case RenderListEntry::RenderableStar:
            continue;

        case RenderListEntry::RenderableCometTail:
        case RenderListEntry::RenderableReferenceMark:
            radius = render_item.radius;
            cullRadius = radius;
            convex = false;
            break;

        case RenderListEntry::RenderableBody:
        default:
            radius = render_item.body->getBoundingRadius();
            if (render_item.body->getRings() != nullptr)
            {
                radius = render_item.body->getRings()->outerRadius;
                convex = false;
            }

            if (!render_item.body->isEllipsoid())
                convex = false;

            cullRadius = radius;
            if (render_item.body->getAtmosphere() != nullptr)
            {
                cullRadius += render_item.body->getAtmosphere()->height;
                cloudHeight = max(render_item.body->getAtmosphere()->cloudHeight,
                                  render_item.body->getAtmosphere()->mieScaleHeight * (float) -log(AtmosphereExtinctionThreshold));
            }
            break;
        }

        // Test the object's bounding sphere against the view frustum
        if (frustum.testSphere(center, cullRadius) != Frustum::Outside)
        {
            float nearZ = center.norm() - radius;
            nearZ = -nearZ * nearZcoeff;

            if (nearZ > -MinNearPlaneDistance)
                render_item.nearZ = -max(MinNearPlaneDistance, radius / 2000.0f);
            else
                render_item.nearZ = nearZ;

            if (!convex)
            {
                render_item.farZ = center.z() - radius;
                if (render_item.farZ / render_item.nearZ > MaxFarNearRatio * 0.5f)
                    render_item.nearZ = render_item.farZ / (MaxFarNearRatio * 0.5f);
            }
            else
            {
                // Make the far plane as close as possible
                float d = center.norm();
                // Account for ellipsoidal objects
                float eradius = radius;
                if (render_item.body != nullptr) // XXX: not checked before
                {
                    Vector3f semiAxes = render_item.body->getSemiAxes();
                    float minSemiAxis = min(semiAxes.x(), min(semiAxes.y(), semiAxes.z()));
                    eradius *= minSemiAxis / radius;
                }

                if (d > eradius)
                {
                    render_item.farZ = render_item.centerZ - render_item.radius;
                }
                else
                {
                    // We're inside the bounding sphere (and, if the planet
                    // is spherical, inside the planet.)
                    render_item.farZ = render_item.nearZ * 2.0f;
                }

                if (cloudHeight > 0.0f)
                {
                    // If there's a cloud layer, we need to move the
                    // far plane out so that the clouds aren't clipped
                    float cloudLayerRadius = eradius + cloudHeight;
                    render_item.farZ -= (float) sqrt(square(cloudLayerRadius) -
                                               square(eradius));
                }
            }

            *notCulled = render_item;
            notCulled++;

            maxBodyMag = min(maxBodyMag, render_item.appMag);
            foundClosestBody = true;
        }
    }

    renderList.resize(notCulled - renderList.begin());
    saturationMag = maxBodyMag;
#endif // USE_HDR

    Color skyColor(0.0f, 0.0f, 0.0f);

    // Scan through the render list to see if we're inside a planetary
    // atmosphere.  If so, we need to adjust the sky color as well as the
    // limiting magnitude of stars (so stars aren't visible in the daytime
    // on planets with thick atmospheres.)
    if (renderFlags & ShowAtmospheres)
    {
        for (const auto& render_item : renderList)
        {
            if (render_item.renderableType != RenderListEntry::RenderableBody || render_item.body->getAtmosphere() == nullptr)
                continue;

            // Compute the density of the atmosphere, and from that
            // the amount light scattering.  It's complicated by the
            // possibility that the planet is oblate and a simple distance
            // to sphere calculation will not suffice.
            const Atmosphere* atmosphere = render_item.body->getAtmosphere();
            if (atmosphere->height <= 0.0f)
                continue;

            float radius = render_item.body->getRadius();
            Vector3f semiAxes = render_item.body->getSemiAxes() / radius;

            Vector3f recipSemiAxes = semiAxes.cwiseInverse();
            Vector3f eyeVec = render_item.position / radius;

            // Compute the orientation of the planet before axial rotation
            Quaterniond qd = render_item.body->getEclipticToEquatorial(now);
            Quaternionf q = qd.cast<float>();
            eyeVec = q * eyeVec;

            // ellipDist is not the true distance from the surface unless
            // the planet is spherical.  The quantity that we do compute
            // is the distance to the surface along a line from the eye
            // position to the center of the ellipsoid.
            float ellipDist = (eyeVec.cwiseProduct(recipSemiAxes)).norm() - 1.0f;
            if (ellipDist >= atmosphere->height / radius)
                continue;

            float density = 1.0f - ellipDist / (atmosphere->height / radius);
            if (density > 1.0f)
                density = 1.0f;

            Vector3f sunDir = render_item.sun.normalized();
            Vector3f normal = -render_item.position.normalized();
#ifdef USE_HDR
            // Ignore magnitude of planet underneath when lighting atmosphere
            // Could be changed to simulate light pollution, etc
            maxBodyMag = maxBodyMagPrev;
            saturationMag = maxBodyMag;
#endif
            float illumination = clamp(sunDir.dot(normal) + 0.2f);

            float lightness = illumination * density;
            faintestMag = faintestMag  - 15.0f * lightness;
            saturationMag = saturationMag - 15.0f * lightness;
        }
    }

    // Now we need to determine how to scale the brightness of stars.  The
    // brightness will be proportional to the apparent magnitude, i.e.
    // a logarithmic function of the stars apparent brightness.  This mimics
    // the response of the human eye.  We sort of fudge things here and
    // maintain a minimum range of six magnitudes between faintest visible
    // and saturation; this keeps stars from popping in or out as the sun
    // sets or rises.
#ifdef USE_HDR
    brightnessScale = 1.0f / (faintestMag -  saturationMag);
#else
    if (faintestMag - saturationMag >= 6.0f)
        brightnessScale = 1.0f / (faintestMag -  saturationMag);
    else
        brightnessScale = 0.1667f;
#endif

#ifdef USE_HDR
    exposurePrev = exposure;
    float exposureNow = 1.f / (1.f+exp((faintestMag - saturationMag + DEFAULT_EXPOSURE)/2.f));
    exposure = exposurePrev + (exposureNow - exposurePrev) * (1.f - exp(-1.f/(15.f * EXPOSURE_HALFLIFE)));
    brightnessScale /= exposure;
#endif

#ifdef DEBUG_HDR_TONEMAP
    HDR_LOG <<
//        "brightnessScale = " << brightnessScale <<
        "faint = "    << faintestMag << ", " <<
        "sat = "      << saturationMag << ", " <<
        "exposure = " << (exposure+brightPlus) << endl;
#endif

#ifdef HDR_COMPRESS
    ambientColor = Color(ambientLightLevel*.5f, ambientLightLevel*.5f, ambientLightLevel*.5f);
#else
    ambientColor = Color(ambientLightLevel, ambientLightLevel, ambientLightLevel);
#endif

    // Create the ambient light source.  For realistic scenes in space, this
    // should be black.
    glAmbientLightColor(ambientColor);

#ifdef USE_HDR
    glClearColor(skyColor.red(), skyColor.green(), skyColor.blue(), 0.0f);
#else
    glClearColor(skyColor.red(), skyColor.green(), skyColor.blue(), 1);
#endif
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glDisable(GL_LIGHTING);
    glDepthMask(GL_FALSE);

    // Render sky grids first--these will always be in the background
    enableSmoothLines(renderFlags);
    renderSkyGrids(observer);
    disableSmoothLines(renderFlags);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);

    // Render deep sky objects
    if ((renderFlags & ShowDSO) != 0 && universe.getDSOCatalog() != nullptr)
    {
        renderDeepSkyObjects(universe, observer, faintestMag);
    }

    // Translate the camera before rendering the stars
    glPushMatrix();

    // Render stars
#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
#endif
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    if ((renderFlags & ShowStars) != 0 && universe.getStarCatalog() != nullptr)
    {
        // Disable multisample rendering when drawing point stars
        bool toggleAA = (starStyle == Renderer::PointStars && glIsEnabled(GL_MULTISAMPLE));
        if (toggleAA)
            glDisable(GL_MULTISAMPLE);

        renderPointStars(*universe.getStarCatalog(), faintestMag, observer);

        if (toggleAA)
            glEnable(GL_MULTISAMPLE);
    }

#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#endif

    glTranslatef(-observerPosLY.x(), -observerPosLY.y(), -observerPosLY.z());


    float dist = observerPosLY.norm() * 1.6e4f;
    renderAsterisms(universe, dist);
    renderBoundaries(universe, dist);

    // Render star and deep sky object labels
    renderBackgroundAnnotations(FontNormal);

    // Render constellations labels
    if ((labelMode & ConstellationLabels) != 0 && universe.getAsterisms() != nullptr)
    {
        labelConstellations(*universe.getAsterisms(), observer);
        renderBackgroundAnnotations(FontLarge);
    }

    // Pop observer translation
    glPopMatrix();

    if ((renderFlags & ShowMarkers) != 0)
    {
        renderMarkers(*universe.getMarkers(),
                      observer.getPosition(),
                      observer.getOrientation(),
                      now);

        // Render background markers; rendering of other markers is deferred until
        // solar system objects are rendered.
        renderBackgroundAnnotations(FontNormal);
    }

    // Draw the selection cursor
    bool selectionVisible = false;
    if (!sel.empty() && (renderFlags & ShowMarkers) != 0)
    {
        Vector3d offset = sel.getPosition(now).offsetFromKm(observer.getPosition());

        static MarkerRepresentation cursorRep(MarkerRepresentation::Crosshair);
        selectionVisible = xfrustum.testSphere(offset, sel.radius()) != Frustum::Outside;

        if (selectionVisible)
        {
            double distance = offset.norm();
            float symbolSize = (float) (sel.radius() / distance) / pixelSize;

            // Modify the marker position so that it is always in front of the marked object.
            double boundingRadius;
            if (sel.body() != nullptr)
                boundingRadius = sel.body()->getBoundingRadius();
            else
                boundingRadius = sel.radius();
            offset *= (1.0 - boundingRadius * 1.01 / distance);

            // The selection cursor is only partially visible when the selected object is obscured. To implement
            // this behavior we'll draw two markers at the same position: one that's always visible, and another one
            // that's depth sorted. When the selection is occluded, only the foreground marker is visible. Otherwise,
            // both markers are drawn and cursor appears much brighter as a result.
            if (distance < astro::lightYearsToKilometers(1.0))
            {
                addSortedAnnotation(&cursorRep, "", Color(SelectionCursorColor, 1.0f),
                                    offset.cast<float>(),
                                    AlignLeft, VerticalAlignTop, symbolSize);
            }
            else
            {
                addAnnotation(backgroundAnnotations, &cursorRep, "", Color(SelectionCursorColor, 1.0f),
                              offset.cast<float>(),
                              AlignLeft, VerticalAlignTop, symbolSize);
                renderBackgroundAnnotations(FontNormal);
            }

            Color occludedCursorColor(SelectionCursorColor.red(), SelectionCursorColor.green() + 0.3f, SelectionCursorColor.blue());
            addAnnotation(foregroundAnnotations,
                          &cursorRep, "", Color(occludedCursorColor, 0.4f),
                          offset.cast<float>(),
                          AlignLeft, VerticalAlignTop, symbolSize);
        }
    }

    glPolygonMode(GL_FRONT, (GLenum) renderMode);
    glPolygonMode(GL_BACK, (GLenum) renderMode);

    {
        Matrix3f viewMat = observer.getOrientationf().conjugate().toRotationMatrix();

        // Remove objects from the render list that lie completely outside the
        // view frustum.
        auto notCulled = renderList.begin();
#ifdef USE_HDR
        maxBodyMag = maxBodyMagPrev;
        float starMaxMag = maxBodyMagPrev;
#endif
        for (auto& render_item : renderList)
        {
#ifdef USE_HDR
            switch (render_item.renderableType)
            {
            case RenderListEntry::RenderableStar:
                break;
            default:
                *notCulled = render_item;
                notCulled++;
                continue;
            }
#endif
            Vector3f center = viewMat.transpose() * render_item.position;

            bool convex = true;
            float radius = 1.0f;
            float cullRadius = 1.0f;
            float cloudHeight = 0.0f;

#ifndef USE_HDR
            switch (render_item.renderableType)
            {
            case RenderListEntry::RenderableStar:
                radius = render_item.star->getRadius();
                cullRadius = radius * (1.0f + CoronaHeight);
                break;

            case RenderListEntry::RenderableCometTail:
                radius = render_item.radius;
                cullRadius = radius;
                convex = false;
                break;

            case RenderListEntry::RenderableBody:
                radius = render_item.body->getBoundingRadius();
                if (render_item.body->getRings() != nullptr)
                {
                    radius = render_item.body->getRings()->outerRadius;
                    convex = false;
                }

                if (!render_item.body->isEllipsoid())
                    convex = false;

                cullRadius = radius;
                if (render_item.body->getAtmosphere() != nullptr)
                {
                    cullRadius += render_item.body->getAtmosphere()->height;
                    cloudHeight = max(render_item.body->getAtmosphere()->cloudHeight,
                                      render_item.body->getAtmosphere()->mieScaleHeight * (float) -log(AtmosphereExtinctionThreshold));
                }
                break;

            case RenderListEntry::RenderableReferenceMark:
                radius = render_item.radius;
                cullRadius = radius;
                convex = false;
                break;

            default:
                break;
            }
#else
            radius = render_item.star->getRadius();
            cullRadius = radius * (1.0f + CoronaHeight);
#endif // USE_HDR

            // Test the object's bounding sphere against the view frustum
            if (frustum.testSphere(center, cullRadius) != Frustum::Outside)
            {
                float nearZ = center.norm() - radius;
#ifdef USE_HDR
                nearZ = -nearZ * nearZcoeff;
#else
                float maxSpan = (float) sqrt(square((float) windowWidth) +
                                             square((float) windowHeight));

                nearZ = -nearZ * (float) cos(degToRad(fov / 2)) *
                    ((float) windowHeight / maxSpan);
#endif
                if (nearZ > -MinNearPlaneDistance)
                    render_item.nearZ = -max(MinNearPlaneDistance, radius / 2000.0f);
                else
                    render_item.nearZ = nearZ;

                if (!convex)
                {
                    render_item.farZ = center.z() - radius;
                    if (render_item.farZ / render_item.nearZ > MaxFarNearRatio * 0.5f)
                        render_item.nearZ = render_item.farZ / (MaxFarNearRatio * 0.5f);
                }
                else
                {
                    // Make the far plane as close as possible
                    float d = center.norm();

                    // Account for ellipsoidal objects
                    float eradius = radius;
                    if (render_item.renderableType == RenderListEntry::RenderableBody)
                    {
                        float minSemiAxis = render_item.body->getSemiAxes().minCoeff();
                        eradius *= minSemiAxis / radius;
                    }

                    if (d > eradius)
                    {
                        render_item.farZ = render_item.centerZ - render_item.radius;
                    }
                    else
                    {
                        // We're inside the bounding sphere (and, if the planet
                        // is spherical, inside the planet.)
                        render_item.farZ = render_item.nearZ * 2.0f;
                    }

                    if (cloudHeight > 0.0f)
                    {
                        // If there's a cloud layer, we need to move the
                        // far plane out so that the clouds aren't clipped
                        float cloudLayerRadius = eradius + cloudHeight;
                        render_item.farZ -= (float) sqrt(square(cloudLayerRadius) -
                                                   square(eradius));
                    }
                }

                *notCulled = render_item;
                notCulled++;
#ifdef USE_HDR
                if (render_item.discSizeInPixels > 1.0f &&
                    render_item.appMag < starMaxMag)
                {
                    starMaxMag = render_item.appMag;
                    brightestStar = render_item.star;
                    foundBrightestStar = true;
                }
#endif
            }
        }

        renderList.resize(notCulled - renderList.begin());

        // The calls to buildRenderLists/renderStars filled renderList
        // with visible bodies.  Sort it front to back, then
        // render each entry in reverse order (TODO: convenient, but not
        // ideal for performance; should render opaque objects front to
        // back, then translucent objects back to front. However, the
        // amount of overdraw in Celestia is typically low.)
        sort(renderList.begin(), renderList.end());

        // Sort the annotations
        sort(depthSortedAnnotations.begin(), depthSortedAnnotations.end());

        // Sort the orbit paths
        sort(orbitPathList.begin(), orbitPathList.end());

        int nEntries = renderList.size();

#ifdef USE_HDR
        // Compute 1 eclipse between eye - closest body - brightest star
        // This prevents an eclipsed star from increasing exposure
        bool eyeNotEclipsed = true;
        closestBody = renderList.empty() ? renderList.end() : renderList.begin();
        if (foundClosestBody &&
            closestBody != renderList.end() &&
            closestBody->renderableType == RenderListEntry::RenderableBody &&
            closestBody->body && brightestStar)
        {
            const Body *body = closestBody->body;
            double scale = astro::microLightYearsToKilometers(1.0);
            Vector3d posBody = body->getAstrocentricPosition(now);
            Vector3d posStar;
            Vector3d posEye = astrocentricPosition(observer.getPosition(), *brightestStar, now);

            if (body->getSystem() &&
                body->getSystem()->getStar() &&
                body->getSystem()->getStar() != brightestStar)
            {
                UniversalCoord center = body->getSystem()->getStar()->getPosition(now);
                posStar = brightestStar->getPosition(now) - center;
            }
            else
            {
                posStar = brightestStar->getPosition(now);
            }

            posStar /= scale;
            Vec3d lightToBodyDir = posBody - posStar;
            Vec3d bodyToEyeDir = posEye - posBody;

            if (lightToBodyDir * bodyToEyeDir > 0.0)
            {
                double dist = distance(posEye,
                                       Ray3d(posBody, lightToBodyDir));
                if (dist < body->getRadius())
                    eyeNotEclipsed = false;
            }
        }

        if (eyeNotEclipsed)
        {
            maxBodyMag = min(maxBodyMag, starMaxMag);
        }
#endif

        // Since we're rendering objects of a huge range of sizes spread over
        // vast distances, we can't just rely on the hardware depth buffer to
        // handle hidden surface removal without a little help. We'll partition
        // the depth buffer into spans that can be rendered without running
        // into terrible depth buffer precision problems. Typically, each body
        // with an apparent size greater than one pixel is allocated its own
        // depth buffer interval. However, this will not correctly handle
        // overlapping objects.  If two objects overlap in depth, we must
        // assign them to the same interval.

        depthPartitions.clear();
        int nIntervals = 0;
        float prevNear = -1e12f;  // ~ 1 light year
        if (nEntries > 0)
            prevNear = renderList[nEntries - 1].farZ * 1.01f;

        int i;

        // Completely partition the depth buffer. Scan from back to front
        // through all the renderable items that passed the culling test.
        for (i = nEntries - 1; i >= 0; i--)
        {
            // Only consider renderables that will occupy more than one pixel.
            if (renderList[i].discSizeInPixels > 1)
            {
                if (nIntervals == 0 || renderList[i].farZ >= depthPartitions[nIntervals - 1].nearZ)
                {
                    // This object spans a depth interval that's disjoint with
                    // the current interval, so create a new one for it, and
                    // another interval to fill the gap between the last
                    // interval.
                    DepthBufferPartition partition;
                    partition.index = nIntervals;
                    partition.nearZ = renderList[i].farZ;
                    partition.farZ = prevNear;

                    // Omit null intervals
                    // TODO: Is this necessary? Shouldn't the >= test prevent this?
                    if (partition.nearZ != partition.farZ)
                    {
                        depthPartitions.push_back(partition);
                        nIntervals++;
                    }

                    partition.index = nIntervals;
                    partition.nearZ = renderList[i].nearZ;
                    partition.farZ = renderList[i].farZ;
                    depthPartitions.push_back(partition);
                    nIntervals++;

                    prevNear = partition.nearZ;
                }
                else
                {
                    // This object overlaps the current span; expand the
                    // interval so that it completely contains the object.
                    DepthBufferPartition& partition = depthPartitions[nIntervals - 1];
                    partition.nearZ = max(partition.nearZ, renderList[i].nearZ);
                    partition.farZ = min(partition.farZ, renderList[i].farZ);
                    prevNear = partition.nearZ;
                }
            }
        }

        // Scan the list of orbit paths and find the closest one. We'll need
        // adjust the nearest interval to accommodate it.
        float zNearest = prevNear;
        for (i = 0; i < (int) orbitPathList.size(); i++)
        {
            const OrbitPathListEntry& o = orbitPathList[i];
            float minNearDistance = min(-MinNearPlaneDistance, o.centerZ + o.radius);
            if (minNearDistance > zNearest)
                zNearest = minNearDistance;
        }

        // Adjust the nearest interval to include the closest marker (if it's
        // closer to the observer than anything else
        if (!depthSortedAnnotations.empty())
        {
            // Factor of 0.999 makes sure ensures that the near plane does not fall
            // exactly at the marker's z coordinate (in which case the marker
            // would be susceptible to getting clipped.)
            if (-depthSortedAnnotations[0].position.z() > zNearest)
                zNearest = -depthSortedAnnotations[0].position.z() * 0.999f;
        }


#if DEBUG_COALESCE
        clog << "nEntries: " << nEntries << ",   zNearest: " << zNearest << ",   prevNear: " << prevNear << "\n";
#endif

        // If the nearest distance wasn't set, nothing should appear
        // in the frontmost depth buffer interval (so we can set the near plane
        // of the front interval to whatever we want as long as it's less than
        // the far plane distance.
        if (zNearest == prevNear)
            zNearest = 0.0f;

        // Add one last interval for the span from 0 to the front of the
        // nearest object
        {
            // TODO: closest object may not be at entry 0, since objects are
            // sorted by far distance.
            float closest = zNearest;
            if (nEntries > 0)
            {
                closest = max(closest, renderList[0].nearZ);

                // Setting a the near plane distance to zero results in unreliable rendering, even
                // if we don't care about the depth buffer. Compromise and set the near plane
                // distance to a small fraction of distance to the nearest object.
                if (closest == 0.0f)
                {
                    closest = renderList[0].nearZ * 0.01f;
                }
            }

            DepthBufferPartition partition;
            partition.index = nIntervals;
            partition.nearZ = closest;
            partition.farZ = prevNear;
            depthPartitions.push_back(partition);

            nIntervals++;
        }

        // If orbits are enabled, adjust the farthest partition so that it
        // can contain the orbit.
        if (!orbitPathList.empty())
        {
            depthPartitions[0].farZ = min(depthPartitions[0].farZ,
                                           orbitPathList[orbitPathList.size() - 1].centerZ -
                                           orbitPathList[orbitPathList.size() - 1].radius);
        }

        // We want to avoid overpartitioning the depth buffer. In this stage, we coalesce
        // partitions that have small spans in the depth buffer.
        // TODO: Implement this step!

        vector<Annotation>::iterator annotation = depthSortedAnnotations.begin();

        // Render everything that wasn't culled.
        float intervalSize = 1.0f / (float) max(1, nIntervals);
        i = nEntries - 1;
        for (int interval = 0; interval < nIntervals; interval++)
        {
            currentIntervalIndex = interval;
            beginObjectAnnotations();

            float nearPlaneDistance = -depthPartitions[interval].nearZ;
            float farPlaneDistance  = -depthPartitions[interval].farZ;

            // Set the depth range for this interval--each interval is allocated an
            // equal section of the depth buffer.
            glDepthRange(1.0f - (float) (interval + 1) * intervalSize,
                         1.0f - (float) interval * intervalSize);

            // Set up a perspective projection using the current interval's near and
            // far clip planes.
            glMatrixMode(GL_PROJECTION);
            glLoadMatrix(Perspective(fov, getAspectRatio(),
                                     nearPlaneDistance,
                                     farPlaneDistance));
            glMatrixMode(GL_MODELVIEW);

            Frustum intervalFrustum(degToRad(fov),
                                    getAspectRatio(),
                                    -depthPartitions[interval].nearZ,
                                    -depthPartitions[interval].farZ);


#if DEBUG_COALESCE
            clog << "interval: " << interval <<
                    ", near: " << -depthPartitions[interval].nearZ <<
                    ", far: " << -depthPartitions[interval].farZ <<
                    "\n";
#endif
            int firstInInterval = i;

            // Render just the opaque objects in the first pass
            while (i >= 0 && renderList[i].farZ < depthPartitions[interval].nearZ)
            {
                // This interval should completely contain the item
                // Unless it's just a point?
                //assert(renderList[i].nearZ <= depthPartitions[interval].near);

#if DEBUG_COALESCE
                switch (renderList[i].renderableType)
                {
                case RenderListEntry::RenderableBody:
                    if (renderList[i].discSizeInPixels > 1)
                    {
                        clog << renderList[i].body->getName() << "\n";
                    }
                    else
                    {
                        clog << "point: " << renderList[i].body->getName() << "\n";
                    }
                    break;

                case RenderListEntry::RenderableStar:
                    if (renderList[i].discSizeInPixels > 1)
                    {
                        clog << "Star\n";
                    }
                    else
                    {
                        clog << "point: " << "Star" << "\n";
                    }
                    break;

                default:
                    break;
                }
#endif
                // Treat objects that are smaller than one pixel as transparent and render
                // them in the second pass.
                if (renderList[i].isOpaque && renderList[i].discSizeInPixels > 1.0f)
                    renderItem(renderList[i], observer, m_cameraOrientation, nearPlaneDistance, farPlaneDistance);

                i--;
            }

            // Render orbit paths
            if (!orbitPathList.empty())
            {
                glDisable(GL_LIGHTING);
                glDisable(GL_TEXTURE_2D);
                glEnable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
#ifdef USE_HDR
                glBlendFunc(GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA);
#else
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#endif
                enableSmoothLines(renderFlags);

                // Scan through the list of orbits and render any that overlap this interval
                for (const auto& orbit : orbitPathList)
                {
                    // Test for overlap
                    float nearZ = -orbit.centerZ - orbit.radius;
                    float farZ = -orbit.centerZ + orbit.radius;

                    // Don't render orbits when they're completely outside this
                    // depth interval.
                    if (nearZ < farPlaneDistance && farZ > nearPlaneDistance)
                    {
#ifdef DEBUG_COALESCE
                        switch (interval % 6)
                        {
                            case 0: glColor4f(1.0f, 0.0f, 0.0f, 1.0f); break;
                            case 1: glColor4f(1.0f, 1.0f, 0.0f, 1.0f); break;
                            case 2: glColor4f(0.0f, 1.0f, 0.0f, 1.0f); break;
                            case 3: glColor4f(0.0f, 1.0f, 1.0f, 1.0f); break;
                            case 4: glColor4f(0.0f, 0.0f, 1.0f, 1.0f); break;
                            case 5: glColor4f(1.0f, 0.0f, 1.0f, 1.0f); break;
                            default: glColor4f(1.0f, 1.0f, 1.0f, 1.0f); break;
                        }
#endif
                        orbitsRendered++;
                        renderOrbit(orbit, now, m_cameraOrientation.cast<double>(), intervalFrustum, nearPlaneDistance, farPlaneDistance);

#if DEBUG_COALESCE
                        if (highlightObject.body() == orbit.body)
                        {
                            clog << "orbit, radius=" << orbit.radius << "\n";
                        }
#endif
                    }
                    else
                    {
                        orbitsSkipped++;
                    }
                }

                disableSmoothLines(renderFlags);
                glDepthMask(GL_FALSE);
            }

            // Render transparent objects in the second pass
            i = firstInInterval;
            while (i >= 0 && renderList[i].farZ < depthPartitions[interval].nearZ)
            {
                if (!renderList[i].isOpaque || renderList[i].discSizeInPixels <= 1.0f)
                    renderItem(renderList[i], observer, m_cameraOrientation, nearPlaneDistance, farPlaneDistance);

                i--;
            }

            // Render annotations in this interval
            enableSmoothLines(renderFlags);
            annotation = renderSortedAnnotations(annotation, -depthPartitions[interval].nearZ, -depthPartitions[interval].farZ, FontNormal);
            endObjectAnnotations();
            disableSmoothLines(renderFlags);
            glDisable(GL_DEPTH_TEST);
        }
#if 0
        // TODO: Debugging output for new orbit code; remove when development is complete
        clog << "orbits: " << orbitsRendered
             << ", skipped: " << orbitsSkipped
             << ", sections culled: " << sectionsCulled
             << ", nIntervals: " << nIntervals << "\n";
#endif
        orbitsRendered = 0;
        orbitsSkipped = 0;
        sectionsCulled = 0;

        // reset the depth range
        glDepthRange(0, 1);
    }

    renderForegroundAnnotations(FontNormal);

    glMatrixMode(GL_PROJECTION);
    glLoadMatrix(Perspective(fov, getAspectRatio(), NEAR_DIST, FAR_DIST));
    glMatrixMode(GL_MODELVIEW);

    if (!selectionVisible && (renderFlags & ShowMarkers))
        renderSelectionPointer(observer, now, xfrustum, sel);

    // Pop camera orientation matrix
    glPopMatrix();

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_LIGHTING);

#if 0
    int errCode = glGetError();
    if (errCode != GL_NO_ERROR)
    {
        cout << "glError: " << (char*) gluErrorString(errCode) << '\n';
    }
#endif

#ifdef VIDEO_SYNC
    if (videoSync && glXWaitVideoSyncSGI != nullptr)
    {
        unsigned int count;
        glXGetVideoSyncSGI(&count);
        glXWaitVideoSyncSGI(2, (count+1) & 1, &count);
    }
#endif
}

// If the an object occupies a pixel or less of screen space, we don't
// render its mesh at all and just display a starlike point instead.
// Switching between the particle and mesh renderings of an object is
// jarring, however . . . so we'll blend in the particle view of the
// object to smooth things out, making it dimmer as the disc size exceeds the
// max disc size.
void Renderer::renderObjectAsPoint(const Vector3f& position,
                                   float radius,
                                   float appMag,
                                   float _faintestMag,
                                   float discSizeInPixels,
                                   Color color,
                                   const Quaternionf& cameraOrientation,
                                   bool useHalos,
                                   bool emissive)
{
    float maxDiscSize = (starStyle == ScaledDiscStars) ? MaxScaledDiscStarSize : 1.0f;
    float maxBlendDiscSize = maxDiscSize + 3.0f;

    bool useScaledDiscs = starStyle == ScaledDiscStars;

    if (discSizeInPixels < maxBlendDiscSize || useHalos)
    {
        float alpha = 1.0f;
        float fade = 1.0f;
        float size = BaseStarDiscSize;
#ifdef USE_HDR
        float fieldCorr = 2.0f * FOV/(fov + FOV);
        float satPoint = saturationMagNight * (1.0f + fieldCorr * fieldCorr);
        satPoint += brightPlus;
#else
        float satPoint = _faintestMag - (1.0f - brightnessBias) / brightnessScale;
#endif

        if (discSizeInPixels > maxDiscSize)
        {
            fade = (maxBlendDiscSize - discSizeInPixels) /
                (maxBlendDiscSize - maxDiscSize);
            if (fade > 1)
                fade = 1;
        }

        alpha = (_faintestMag - appMag) * brightnessScale * 2.0f + brightnessBias;
        if (alpha < 0.0f)
            alpha = 0.0f;

        float pointSize = size;
        float glareSize = 0.0f;
        float glareAlpha = 0.0f;
        if (useScaledDiscs)
        {
            if (alpha > 1.0f)
            {
                float discScale = min(MaxScaledDiscStarSize, (float) pow(2.0f, 0.3f * (satPoint - appMag)));
                pointSize *= max(1.0f, discScale);

                glareAlpha = min(0.5f, discScale / 4.0f);
                if (discSizeInPixels > MaxScaledDiscStarSize)
                {
                    glareAlpha = min(glareAlpha,
                                     (MaxScaledDiscStarSize - discSizeInPixels) / MaxScaledDiscStarSize + 1.0f);
                }
                glareSize = pointSize * 3.0f;

                alpha = 1.0f;
            }
        }
        else
        {
            if (alpha > 1.0f)
            {
                float discScale = min(100.0f, satPoint - appMag + 2.0f);
                glareAlpha = min(GlareOpacity, (discScale - 2.0f) / 4.0f);
                glareSize = pointSize * discScale * 2.0f ;
                if (emissive)
                    glareSize = max(glareSize, pointSize * discSizeInPixels * 3.0f);
            }
        }

        alpha *= fade;
        if (!emissive)
        {
            glareSize = max(glareSize, pointSize * discSizeInPixels * 3.0f);
            glareAlpha *= fade;
        }

        Matrix3f m = cameraOrientation.conjugate().toRotationMatrix();
        Vector3f center = position;

        // Offset the glare sprite so that it lies in front of the object
        Vector3f direction = center.normalized();

        // Position the sprite on the the line between the viewer and the
        // object, and on a plane normal to the view direction.
        center = center + direction * (radius / (m * Vector3f::UnitZ()).dot(direction));

        pointStarVertexBuffer->addStar(center, {color, alpha}, pointSize);
        // If the object is brighter than magnitude 1, add a halo around it to
        // make it appear more brilliant.  This is a hack to compensate for the
        // limited dynamic range of monitors.
        //
        // TODO: Stars look fine but planets look unrealistically bright
        // with halos.
        if (useHalos && glareAlpha > 0.0f)
        {
            glareVertexBuffer->addStar(center, {color, glareAlpha}, glareSize);
        }
    }
}


// Used to sort light sources in order of decreasing irradiance
struct LightIrradiancePredicate
{
    int unused;

    LightIrradiancePredicate() = default;

    bool operator()(const DirectionalLight& l0,
                    const DirectionalLight& l1) const
    {
        return (l0.irradiance > l1.irradiance);
    }
};


void Renderer::renderEllipsoidAtmosphere(const Atmosphere& atmosphere,
                                         const Vector3f& center,
                                         const Quaternionf& orientation,
                                         const Vector3f& semiAxes,
                                         const Vector3f& sunDirection,
                                         const LightingState& ls,
                                         float pixSize,
                                         bool lit)
{
    if (atmosphere.height == 0.0f)
        return;

    auto *prog = shaderManager->getShader(ShaderProperties::PerVertexColor);
    if (prog == nullptr)
        return;

    glDepthMask(GL_FALSE);

    // Gradually fade in the atmosphere if it's thickness on screen is just
    // over one pixel.
    float fade = clamp(pixSize - 2);

    Matrix3f rot = orientation.toRotationMatrix();
    Matrix3f irot = orientation.conjugate().toRotationMatrix();

    Vector3f eyePos = Vector3f::Zero();
    float radius = semiAxes.maxCoeff();
    Vector3f eyeVec = center - eyePos;
    eyeVec = rot * eyeVec;
    double centerDist = eyeVec.norm();

    float height = atmosphere.height / radius;
    Vector3f recipSemiAxes = semiAxes.cwiseInverse();

#if 0
    Vector3f recipAtmSemiAxes = recipSemiAxes / (1.0f + height);
#endif
    // ellipDist is not the true distance from the surface unless the
    // planet is spherical.  Computing the true distance requires finding
    // the roots of a sixth degree polynomial, and isn't actually what we
    // want anyhow since the atmosphere region is just the planet ellipsoid
    // multiplied by a uniform scale factor.  The value that we do compute
    // is the distance to the surface along a line from the eye position to
    // the center of the ellipsoid.
    float ellipDist = (eyeVec.cwiseProduct(recipSemiAxes)).norm() - 1.0f;
    bool within = ellipDist < height;

    // Adjust the tesselation of the sky dome/ring based on distance from the
    // planet surface.
    int nSlices = MaxSkySlices;
    if (ellipDist < 0.25f)
    {
        nSlices = MinSkySlices + max(0, (int) ((ellipDist / 0.25f) * (MaxSkySlices - MinSkySlices)));
        nSlices &= ~1;
    }

    int nRings = min(1 + (int) pixSize / 5, 6);
    int nHorizonRings = nRings;
    if (within)
        nRings += 12;

    float horizonHeight = height;
    if (within)
    {
        if (ellipDist <= 0.0f)
            horizonHeight = 0.0f;
        else
            horizonHeight *= max((float) pow(ellipDist / height, 0.33f), 0.001f);
    }

    Vector3f e = -eyeVec;
    Vector3f e_ = e.cwiseProduct(recipSemiAxes);
    float ee = e_.dot(e_);

    // Compute the cosine of the altitude of the sun.  This is used to compute
    // the degree of sunset/sunrise coloration.
    float cosSunAltitude = 0.0f;
    {
        // Check for a sun either directly behind or in front of the viewer
        float cosSunAngle = (float) (sunDirection.dot(e) / centerDist);
        if (cosSunAngle < -1.0f + 1.0e-6f)
        {
            cosSunAltitude = 0.0f;
        }
        else if (cosSunAngle > 1.0f - 1.0e-6f)
        {
            cosSunAltitude = 0.0f;
        }
        else
        {
            Vector3f v = (rot * -sunDirection) * (float) centerDist;
            Vector3f tangentPoint = center +
                irot * ellipsoidTangent(recipSemiAxes,
                                        v,
                                        e, e_, ee);
            Vector3f tangentDir = (tangentPoint - eyePos).normalized();
            cosSunAltitude = sunDirection.dot(tangentDir);
        }
    }

    Vector3f normal = eyeVec;
    normal = normal / (float) centerDist;

    Vector3f uAxis, vAxis;
    if (abs(normal.x()) < abs(normal.y()) && abs(normal.x()) < abs(normal.z()))
    {
        uAxis = Vector3f::UnitX().cross(normal);
    }
    else if (abs(eyeVec.y()) < abs(normal.z()))
    {
        uAxis = Vector3f::UnitY().cross(normal);
    }
    else
    {
        uAxis = Vector3f::UnitZ().cross(normal);
    }
    uAxis.normalize();
    vAxis = uAxis.cross(normal);

    // Compute the contour of the ellipsoid
    for (int i = 0; i <= nSlices; i++)
    {
        // We want rays with an origin at the eye point and tangent to the the
        // ellipsoid.
        float theta = (float) i / (float) nSlices * 2 * (float) PI;
        Vector3f w = (float) cos(theta) * uAxis + (float) sin(theta) * vAxis;
        w = w * (float) centerDist;

        Vector3f toCenter = ellipsoidTangent(recipSemiAxes, w, e, e_, ee);
        skyContour[i].v = irot * toCenter;
        skyContour[i].centerDist = skyContour[i].v.norm();
        skyContour[i].eyeDir = skyContour[i].v + (center - eyePos);
        skyContour[i].eyeDist = skyContour[i].eyeDir.norm();
        skyContour[i].eyeDir.normalize();

        float skyCapDist = (float) sqrt(square(skyContour[i].eyeDist) +
                                        square(horizonHeight * radius));
        skyContour[i].cosSkyCapAltitude = skyContour[i].eyeDist / skyCapDist;
    }


    Vector3f botColor = atmosphere.lowerColor.toVector3();
    Vector3f topColor = atmosphere.upperColor.toVector3();
    Vector3f sunsetColor = atmosphere.sunsetColor.toVector3();

    if (within)
    {
        Vector3f skyColor = atmosphere.skyColor.toVector3();
        if (ellipDist < 0.0f)
            topColor = skyColor;
        else
            topColor = skyColor + (topColor - skyColor) * (ellipDist / height);
    }

    if (ls.nLights == 0 && lit)
    {
        botColor = topColor = sunsetColor = Vector3f::Zero();
    }

    Vector3f zenith = (skyContour[0].v + skyContour[nSlices / 2].v);
    zenith.normalize();
    zenith *= skyContour[0].centerDist * (1.0f + horizonHeight * 2.0f);

    float minOpacity = within ? (1.0f - ellipDist / height) * 0.75f : 0.0f;
    float sunset = cosSunAltitude < 0.9f ? 0.0f : (cosSunAltitude - 0.9f) * 10.0f;

    // Build the list of vertices
    SkyVertex* vtx = skyVertices;
    for (int i = 0; i <= nRings; i++)
    {
        float h = min(1.0f, (float) i / (float) nHorizonRings);
        auto hh = (float) sqrt(h);
        float u = i <= nHorizonRings ? 0.0f :
            (float) (i - nHorizonRings) / (float) (nRings - nHorizonRings);
        float r = lerp(h, 1.0f - (horizonHeight * 0.05f), 1.0f + horizonHeight);
        float atten = 1.0f - hh;

        for (int j = 0; j < nSlices; j++)
        {
            Vector3f v;
            if (i <= nHorizonRings)
                v = skyContour[j].v * r;
            else
                v = (skyContour[j].v * (1.0f - u) + zenith * u) * r;
            Vector3f p = center + v;

            Vector3f viewDir = p.normalized();
            float cosSunAngle = viewDir.dot(sunDirection);
            float cosAltitude = viewDir.dot(skyContour[j].eyeDir);
            float brightness = 1.0f;
            float coloration = 0.0f;
            if (lit)
            {
                if (sunset > 0.0f && cosSunAngle > 0.7f && cosAltitude > 0.98f)
                {
                    coloration =  (1.0f / 0.30f) * (cosSunAngle - 0.70f);
                    coloration *= 50.0f * (cosAltitude - 0.98f);
                    coloration *= sunset;
                }

                cosSunAngle = skyContour[j].v.dot(sunDirection) / skyContour[j].centerDist;
                if (cosSunAngle > -0.2f)
                {
                    if (cosSunAngle < 0.3f)
                        brightness = (cosSunAngle + 0.2f) * 2.0f;
                    else
                        brightness = 1.0f;
                }
                else
                {
                    brightness = 0.0f;
                }
            }

            vtx->x = p.x();
            vtx->y = p.y();
            vtx->z = p.z();

            atten = 1.0f - hh;
            Vector3f color = (1.0f - hh) * botColor + hh * topColor;
            brightness *= minOpacity + (1.0f - minOpacity) * fade * atten;
            if (coloration != 0.0f)
                color = (1.0f - coloration) * color + coloration * sunsetColor;

#ifdef HDR_COMPRESS
            brightness *= 0.5f;
#endif
            Color(brightness * color.x(),
                  brightness * color.y(),
                  brightness * color.z(),
                  fade * (minOpacity + (1.0f - minOpacity)) * atten).get(vtx->color);
            vtx++;
        }
    }

    // Create the index list
    int index = 0;
    for (int i = 0; i < nRings; i++)
    {
        int baseVertex = i * nSlices;
        for (int j = 0; j < nSlices; j++)
        {
            skyIndices[index++] = baseVertex + j;
            skyIndices[index++] = baseVertex + nSlices + j;
        }
        skyIndices[index++] = baseVertex;
        skyIndices[index++] = baseVertex + nSlices;
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(SkyVertex), &skyVertices[0].x);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SkyVertex),
                   static_cast<void*>(&skyVertices[0].color));

    prog->use();
    for (int i = 0; i < nRings; i++)
    {
        glDrawElements(GL_QUAD_STRIP,
                       (nSlices + 1) * 2,
                       GL_UNSIGNED_INT,
                       &skyIndices[(nSlices + 1) * 2 * i]);
    }

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glUseProgram(0);
}


static void renderSphereUnlit(const RenderInfo& ri,
                              const Frustum& frustum,
                              const Renderer *r)
{
    Texture* textures[MAX_SPHERE_MESH_TEXTURES];
    int nTextures = 0;

    ShaderProperties shadprop;

    // Set up the textures used by this object
    if (ri.baseTex != nullptr)
    {
        shadprop.texUsage = ShaderProperties::DiffuseTexture;
        textures[nTextures++] = ri.baseTex;
    }
    if (ri.nightTex != nullptr)
    {
        shadprop.texUsage |= ShaderProperties::NightTexture;
        textures[nTextures++] = ri.nightTex;
    }
    if (ri.overlayTex != nullptr)
    {
        shadprop.texUsage |= ShaderProperties::OverlayTexture;
        textures[nTextures++] = ri.overlayTex;
    }

    // Get a shader for the current rendering configuration
    auto* prog = r->getShaderManager().getShader(shadprop);
    if (prog == nullptr)
        return;
    prog->use();

    prog->textureOffset = 0.0f;
    // TODO: introduce a new ShaderProperties light model, so those
    // assignments are not required
    prog->ambientColor = Color::White.toVector3();
    prog->opacity = 1.0f;
#ifdef USE_HDR
    prog->nightLightScale = ri.nightLightScale;
#endif
    g_lodSphere->render(frustum, ri.pixWidth, textures, nTextures);

    glUseProgram(0);
}


static void renderCloudsUnlit(const RenderInfo& ri,
                              const Frustum& frustum,
                              Texture *cloudTex,
                              float cloudTexOffset,
                              const Renderer *r)
{
    ShaderProperties shadprop;
    shadprop.texUsage = ShaderProperties::DiffuseTexture;

    // Get a shader for the current rendering configuration
    auto* prog = r->getShaderManager().getShader(shadprop);
    if (prog == nullptr)
        return;
    prog->use();

    prog->textureOffset = cloudTexOffset;
    // TODO: introduce a new ShaderProperties light model, so those
    // assignments are not required
    prog->ambientColor = Color::White.toVector3();
    prog->opacity = 1.0f;

    g_lodSphere->render(frustum, ri.pixWidth, &cloudTex, 1);

    glUseProgram(0);
}

void Renderer::renderLocations(const Body& body,
                               const Vector3d& bodyPosition,
                               const Quaterniond& bodyOrientation)
{
    const vector<Location*>* locations = body.getLocations();

    if (locations == nullptr)
        return;

    Vector3f semiAxes = body.getSemiAxes();

    float nearDist = getNearPlaneDistance();
    double boundingRadius = semiAxes.maxCoeff();

    Vector3d bodyCenter = bodyPosition;
    Vector3d viewRayOrigin = bodyOrientation * -bodyCenter;
    double labelOffset = 0.0001;

    Vector3f vn  = getCameraOrientation().conjugate() * -Vector3f::UnitZ();
    Vector3d viewNormal = vn.cast<double>();

    Ellipsoidd bodyEllipsoid(semiAxes.cast<double>());

    Matrix3d bodyMatrix = bodyOrientation.conjugate().toRotationMatrix();

    for (const auto location : *locations)
    {
        auto featureType = location->getFeatureType();
        if ((featureType & locationFilter) != 0)
        {
            // Get the position of the location with respect to the planet center
            Vector3f ppos = location->getPosition();

            // Compute the bodycentric position of the location
            Vector3d locPos = ppos.cast<double>();

            // Get the planetocentric position of the label.  Add a slight scale factor
            // to keep the point from being exactly on the surface.
            Vector3d pcLabelPos = locPos * (1.0 + labelOffset);

            // Get the camera space label position
            Vector3d labelPos = bodyCenter + bodyMatrix * locPos;

            float effSize = location->getImportance();
            if (effSize < 0.0f)
                effSize = location->getSize();

            float pixSize = effSize / (float) (labelPos.norm() * pixelSize);

            if (pixSize > minFeatureSize && labelPos.dot(viewNormal) > 0.0)
            {
                // Labels on non-ellipsoidal bodies need special handling; the
                // ellipsoid visibility test will always fail for them, since they
                // will lie on the surface of the mesh, which is inside the
                // the bounding ellipsoid. The following code projects location positions
                // onto the bounding sphere.
                if (!body.isEllipsoid())
                {
                    double r = locPos.norm();
                    if (r < boundingRadius)
                        pcLabelPos = locPos * (boundingRadius * 1.01 / r);
                }

                double t = 0.0;

                // Test for an intersection of the eye-to-location ray with
                // the planet ellipsoid.  If we hit the planet first, then
                // the label is obscured by the planet.  An exact calculation
                // for irregular objects would be too expensive, and the
                // ellipsoid approximation works reasonably well for them.
                Ray3d testRay(viewRayOrigin, pcLabelPos - viewRayOrigin);
                bool hit = testIntersection(testRay, bodyEllipsoid, t);

                if (!hit || t >= 1.0)
                {
                    // Calculate the intersection of the eye-to-label ray with the plane perpendicular to
                    // the view normal that touches the front of the object's bounding sphere
                    double planetZ = viewNormal.dot(bodyCenter) - boundingRadius;
                    if (planetZ < -nearDist * 1.001)
                        planetZ = -nearDist * 1.001;
                    double z = viewNormal.dot(labelPos);
                    labelPos *= planetZ / z;

                    MarkerRepresentation* locationMarker = nullptr;
                    if (featureType & Location::City)
                        locationMarker = &cityRep;
                    else if (featureType & (Location::LandingSite | Location::Observatory))
                        locationMarker = &observatoryRep;
                    else if (featureType & (Location::Crater | Location::Patera))
                        locationMarker = &craterRep;
                    else if (featureType & (Location::Mons | Location::Tholus))
                        locationMarker = &mountainRep;
                    else if (featureType & (Location::EruptiveCenter))
                        locationMarker = &genericLocationRep;

                    Color labelColor = location->isLabelColorOverridden() ? location->getLabelColor() : LocationLabelColor;
                    addObjectAnnotation(locationMarker,
                                        location->getName(true),
                                        labelColor,
                                        labelPos.cast<float>());
                }
            }
        }
    }
}


// Estimate the fraction of light reflected from a sphere that
// reaches an object at the specified position relative to that
// sphere.
//
// This is function is just a rough approximation to the actual
// lighting integral, but it reproduces the important features
// of the way that phase and distance affect reflected light:
//    - Higher phase angles mean less reflected light
//    - The closer an object is to the reflector, the less
//      area of the reflector that is visible.
//
// We approximate the reflected light by taking a weighted average
// of the reflected light at three points on the reflector: the
// light receiver's sub-point, and the two horizon points in the
// plane of the light vector and receiver-to-reflector vector.
//
// The reflecting object is assumed to be spherical and perfectly
// Lambertian.
static float
estimateReflectedLightFraction(const Vector3d& toSun,
                               const Vector3d& toObject,
                               float radius)
{
    // Theta is half the arc length visible to the reflector
    double d = toObject.norm();
    auto cosTheta = (float) (radius / d);
    if (cosTheta > 0.999f)
        cosTheta = 0.999f;

    // Phi is the angle between the light vector and receiver-to-reflector vector.
    // cos(phi) is thus the illumination at the sub-point. The horizon points are
    // at phi+theta and phi-theta.
    float cosPhi = (float) (toSun.dot(toObject) / (d * toSun.norm()));

    // Use a trigonometric identity to compute cos(phi +/- theta):
    //   cos(phi + theta) = cos(phi) * cos(theta) - sin(phi) * sin(theta)

    // s = sin(phi) * sin(theta)
    auto s = (float) sqrt((1.0f - cosPhi * cosPhi) * (1.0f - cosTheta * cosTheta));

    float cosPhi1 = cosPhi * cosTheta - s;  // cos(phi + theta)
    float cosPhi2 = cosPhi * cosTheta + s;  // cos(phi - theta)

    // Calculate a weighted average of illumination at the three points
    return (2.0f * max(cosPhi, 0.0f) + max(cosPhi1, 0.0f) + max(cosPhi2, 0.0f)) * 0.25f;
}


static void
setupObjectLighting(const vector<LightSource>& suns,
                    const vector<SecondaryIlluminator>& secondaryIlluminators,
                    const Quaternionf& objOrientation,
                    const Vector3f& objScale,
                    const Vector3f& objPosition_eye,
                    bool isNormalized,
#ifdef USE_HDR
                    const float faintestMag,
                    const float saturationMag,
                    const float appMag,
#endif
                    LightingState& ls)
{
    unsigned int nLights = min(MaxLights, (unsigned int) suns.size());
    if (nLights == 0)
        return;

#ifdef USE_HDR
    float exposureFactor = (faintestMag - appMag)/(faintestMag - saturationMag + 0.001f);
#endif

    unsigned int i;
    for (i = 0; i < nLights; i++)
    {
        Vector3d dir = suns[i].position - objPosition_eye.cast<double>();

        ls.lights[i].direction_eye = dir.cast<float>();
        float distance = ls.lights[i].direction_eye.norm();
        ls.lights[i].direction_eye *= 1.0f / distance;
        distance = astro::kilometersToAU((float) dir.norm());
        ls.lights[i].irradiance = suns[i].luminosity / (distance * distance);
        ls.lights[i].color = suns[i].color;

        // Store the position and apparent size because we'll need them for
        // testing for eclipses.
        ls.lights[i].position = dir;
        ls.lights[i].apparentSize = (float) (suns[i].radius / dir.norm());
        ls.lights[i].castsShadows = true;
    }

    // Include effects of secondary illumination (i.e. planetshine)
    if (!secondaryIlluminators.empty() && i < MaxLights - 1)
    {
        float maxIrr = 0.0f;
        unsigned int maxIrrSource = 0, counter = 0;
        Vector3d objpos = objPosition_eye.cast<double>();

        // Only account for light from the brightest secondary source
        for (auto& illuminator : secondaryIlluminators)
        {
            Vector3d toIllum = illuminator.position_v - objpos;  // reflector-to-object vector
            float distSquared = (float) toIllum.squaredNorm() / square(illuminator.radius);

            if (distSquared > 0.01f)
            {
                // Irradiance falls off with distance^2
                float irr = illuminator.reflectedIrradiance / distSquared;

                // Phase effects will always leave the irradiance unaffected or reduce it;
                // don't bother calculating them if we've already found a brighter secondary
                // source.
                if (irr > maxIrr)
                {
                    // Account for the phase
                    Vector3d toSun = objpos - suns[0].position;
                    irr *= estimateReflectedLightFraction(toSun, toIllum, illuminator.radius);
                    if (irr > maxIrr)
                    {
                        maxIrr = irr;
                        maxIrrSource = counter;
                    }
                }
            }
            counter++;
        }
#if DEBUG_SECONDARY_ILLUMINATION
        clog << "maxIrr = " << maxIrr << ", "
             << secondaryIlluminators[maxIrrSource].body->getName() << ", "
             << secondaryIlluminators[maxIrrSource].reflectedIrradiance << endl;
#endif

        if (maxIrr > 0.0f)
        {
            Vector3d toIllum = secondaryIlluminators[maxIrrSource].position_v - objpos;

            ls.lights[i].direction_eye = toIllum.cast<float>();
            ls.lights[i].direction_eye.normalize();
            ls.lights[i].irradiance = maxIrr;
            ls.lights[i].color = secondaryIlluminators[maxIrrSource].body->getSurface().color;
            ls.lights[i].apparentSize = 0.0f;
            ls.lights[i].castsShadows = false;
            i++;
            nLights++;
        }
    }

    // Sort light sources by brightness.  Light zero should always be the
    // brightest.  Optimize common cases of one and two lights.
    if (nLights == 2)
    {
        if (ls.lights[0].irradiance < ls.lights[1].irradiance)
            swap(ls.lights[0], ls.lights[1]);
    }
    else if (nLights > 2)
    {
        sort(ls.lights, ls.lights + nLights, LightIrradiancePredicate());
    }

    // Compute the total irradiance
    float totalIrradiance = 0.0f;
    for (i = 0; i < nLights; i++)
        totalIrradiance += ls.lights[i].irradiance;

    // Compute a gamma factor to make dim light sources visible.  This is
    // intended to approximate what we see with our eyes--for example,
    // Earth-shine is visible on the night side of the Moon, even though
    // the amount of reflected light from the Earth is 1/10000 of what
    // the Moon receives directly from the Sun.
    //
    // TODO: Skip this step when high dynamic range rendering to floating point
    //   buffers is enabled.
    float minVisibleFraction = 1.0f / 10000.0f;
    float minDisplayableValue = 1.0f / 255.0f;
    auto gamma = (float) (log(minDisplayableValue) / log(minVisibleFraction));
    float minVisibleIrradiance = minVisibleFraction * totalIrradiance;

    Matrix3f m = objOrientation.toRotationMatrix();

    // Gamma scale and normalize the light sources; cull light sources that
    // aren't bright enough to contribute the final pixels rendered into the
    // frame buffer.
    ls.nLights = 0;
    for (i = 0; i < nLights && ls.lights[i].irradiance > minVisibleIrradiance; i++)
    {
#ifdef USE_HDR
        ls.lights[i].irradiance *= exposureFactor / totalIrradiance;
#else
        ls.lights[i].irradiance =
            (float) pow(ls.lights[i].irradiance / totalIrradiance, gamma);
#endif

        // Compute the direction of the light in object space
        ls.lights[i].direction_obj = m * ls.lights[i].direction_eye;

        ls.nLights++;
    }

    Matrix3f invScale = objScale.cwiseInverse().asDiagonal();
    ls.eyePos_obj = invScale * m * -objPosition_eye;
    ls.eyeDir_obj = (m * -objPosition_eye).normalized();

    // When the camera is very far from the object, some view-dependent
    // calculations in the shaders can exhibit precision problems. This
    // occurs with atmospheres, where the scale height of the atmosphere
    // is very small relative to the planet radius. To address the problem,
    // we'll clamp the eye distance to some maximum value. The effect of the
    // adjustment should be impercetible, since at large distances rays from
    // the camera to object vertices are all nearly parallel to each other.
    float eyeFromCenterDistance = ls.eyePos_obj.norm();
    if (eyeFromCenterDistance > 100.0f && isNormalized)
    {
        ls.eyePos_obj *= 100.0f / eyeFromCenterDistance;
    }

    ls.ambientColor = Vector3f::Zero();
}


void Renderer::renderObject(const Vector3f& pos,
                            float distance,
                            double now,
                            const Quaternionf& cameraOrientation,
                            float nearPlaneDistance,
                            float farPlaneDistance,
                            RenderProperties& obj,
                            const LightingState& ls)
{
    RenderInfo ri;

    float altitude = distance - obj.radius;
    float discSizeInPixels = obj.radius / (max(nearPlaneDistance, altitude) * pixelSize);

    ri.sunDir_eye = Vector3f::UnitY();
    ri.sunDir_obj = Vector3f::UnitY();
    ri.sunColor = Color(0.0f, 0.0f, 0.0f);
    if (ls.nLights > 0)
    {
        ri.sunDir_eye = ls.lights[0].direction_eye;
        ri.sunDir_obj = ls.lights[0].direction_obj;
        ri.sunColor   = ls.lights[0].color;// * ls.lights[0].intensity;
    }

    // Enable depth buffering
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    glDisable(GL_BLEND);

    // Get the object's geometry; nullptr indicates that object is an
    // ellipsoid.
    Geometry* geometry = nullptr;
    if (obj.geometry != InvalidResource)
    {
        // This is a model loaded from a file
        geometry = GetGeometryManager()->find(obj.geometry);
    }

    // Get the textures . . .
    if (obj.surface->baseTexture.tex[textureResolution] != InvalidResource)
        ri.baseTex = obj.surface->baseTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::ApplyBumpMap) != 0 &&
        obj.surface->bumpTexture.tex[textureResolution] != InvalidResource)
        ri.bumpTex = obj.surface->bumpTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::ApplyNightMap) != 0 &&
        (renderFlags & ShowNightMaps) != 0)
        ri.nightTex = obj.surface->nightTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::SeparateSpecularMap) != 0)
        ri.glossTex = obj.surface->specularTexture.find(textureResolution);
    if ((obj.surface->appearanceFlags & Surface::ApplyOverlay) != 0)
        ri.overlayTex = obj.surface->overlayTexture.find(textureResolution);

    // Apply the modelview transform for the object
    glPushMatrix();
    glTranslate(pos);
    glRotate(obj.orientation.conjugate());

    // Scaling will be nonuniform for nonspherical planets. As long as the
    // deviation from spherical isn't too large, the nonuniform scale factor
    // shouldn't mess up the lighting calculations enough to be noticeable
    // (and we turn on renormalization anyhow, which most graphics cards
    // support.)
    float radius = obj.radius;
    Vector3f scaleFactors;
    float geometryScale;
    if (geometry == nullptr || geometry->isNormalized())
    {
        geometryScale = obj.radius;
        scaleFactors = obj.radius * obj.semiAxes;
        ri.pointScale = 2.0f * obj.radius / pixelSize;
    }
    else
    {
        geometryScale = obj.geometryScale;
        scaleFactors = Vector3f::Constant(geometryScale);
        ri.pointScale = 2.0f * geometryScale / pixelSize;
    }
    glScale(scaleFactors);

    Matrix3f planetRotation = obj.orientation.toRotationMatrix();

    ri.eyeDir_obj = -(planetRotation * pos).normalized();
    ri.eyePos_obj = -(planetRotation * (pos.cwiseQuotient(scaleFactors)));

    ri.orientation = cameraOrientation * obj.orientation.conjugate();

    ri.pixWidth = discSizeInPixels;

    // Set up the colors
    if (ri.baseTex == nullptr ||
        (obj.surface->appearanceFlags & Surface::BlendTexture) != 0)
    {
        ri.color = obj.surface->color;
    }

    ri.ambientColor = ambientColor;
    ri.specularColor = obj.surface->specularColor;
    ri.specularPower = obj.surface->specularPower;
    ri.useTexEnvCombine = true;
    ri.lunarLambert = obj.surface->lunarLambert;
#ifdef USE_HDR
    ri.nightLightScale = obj.surface->nightLightRadiance * exposure * 1.e5f * .5f;
#endif

    // See if the surface should be lit
    bool lit = (obj.surface->appearanceFlags & Surface::Emissive) == 0;

    // Compute the inverse model/view matrix
    Affine3f invModelView = obj.orientation *
                            Translation3f(-pos / obj.radius) *
                            cameraOrientation.conjugate();
    Matrix4f invMV = invModelView.matrix();

    // The sphere rendering code uses the view frustum to determine which
    // patches are visible. In order to avoid rendering patches that can't
    // be seen, make the far plane of the frustum as close to the viewer
    // as possible.
    float frustumFarPlane = farPlaneDistance;
    if (obj.geometry == InvalidResource)
    {
        // Only adjust the far plane for ellipsoidal objects
        float d = pos.norm();

        // Account for non-spherical objects
        float eradius = scaleFactors.minCoeff();

        if (d > eradius)
        {
            // Include a fudge factor to eliminate overaggressive clipping
            // due to limited floating point precision
            frustumFarPlane = (float) sqrt(square(d) - square(eradius)) * 1.1f;
        }
        else
        {
            // We're inside the bounding sphere; leave the far plane alone
        }

        if (obj.atmosphere != nullptr)
        {
            float atmosphereHeight = max(obj.atmosphere->cloudHeight,
                                         obj.atmosphere->mieScaleHeight * (float) -log(AtmosphereExtinctionThreshold));
            if (atmosphereHeight > 0.0f)
            {
                // If there's an atmosphere, we need to move the far plane
                // out so that the clouds and atmosphere shell aren't clipped.
                float atmosphereRadius = eradius + atmosphereHeight;
                frustumFarPlane += (float) sqrt(square(atmosphereRadius) -
                                                square(eradius));
            }
        }
    }

    // Transform the frustum into object coordinates using the
    // inverse model/view matrix. The frustum is scaled to a
    // normalized coordinate system where the 1 unit = 1 planet
    // radius (for an ellipsoidal planet, radius is taken to be
    // largest semiaxis.)
    Frustum viewFrustum(degToRad(fov),
                        getAspectRatio(),
                        nearPlaneDistance / radius, frustumFarPlane / radius);
    viewFrustum.transform(invMV);

    // Get cloud layer parameters
    Texture* cloudTex       = nullptr;
    Texture* cloudNormalMap = nullptr;
    float cloudTexOffset    = 0.0f;
    // Ugly cast required because MultiResTexture::find() is non-const
    Atmosphere* atmosphere  = const_cast<Atmosphere*>(obj.atmosphere);

    if (atmosphere != nullptr)
    {
        if ((renderFlags & ShowCloudMaps) != 0)
        {
            if (atmosphere->cloudTexture.tex[textureResolution] != InvalidResource)
                cloudTex = atmosphere->cloudTexture.find(textureResolution);
            if (atmosphere->cloudNormalMap.tex[textureResolution] != InvalidResource)
                cloudNormalMap = atmosphere->cloudNormalMap.find(textureResolution);
        }
        if (atmosphere->cloudSpeed != 0.0f)
            cloudTexOffset = (float) (-pfmod(now * atmosphere->cloudSpeed / (2 * PI), 1.0));
    }

    if (obj.geometry == InvalidResource)
    {
        // A null model indicates that this body is a sphere
        if (lit)
        {
            renderEllipsoid_GLSL(ri, ls,
                                 atmosphere, cloudTexOffset,
                                 scaleFactors,
                                 textureResolution,
                                 renderFlags,
                                 obj.orientation, viewFrustum, this);
        }
        else
        {
            renderSphereUnlit(ri, viewFrustum, this);
        }
    }
    else
    {
        if (geometry != nullptr)
        {
            ResourceHandle texOverride = obj.surface->baseTexture.tex[textureResolution];

            if (lit)
            {
                renderGeometry_GLSL(geometry,
                                    ri,
                                    texOverride,
                                    ls,
                                    obj.atmosphere,
                                    geometryScale,
                                    renderFlags,
                                    obj.orientation,
                                    astro::daysToSecs(now - astro::J2000),
                                    this);
            }
            else
            {
                renderGeometry_GLSL_Unlit(geometry,
                                          ri,
                                          texOverride,
                                          geometryScale,
                                          renderFlags,
                                          obj.orientation,
                                          astro::daysToSecs(now - astro::J2000),
                                          this);
            }

            // XXX: this code should be removed
            for (unsigned int i = 1; i < 8;/*context->getMaxTextures();*/ i++)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glDisable(GL_TEXTURE_2D);
            }
            glActiveTexture(GL_TEXTURE0);
            glEnable(GL_TEXTURE_2D);
        }
    }

    float segmentSizeInPixels = 0.0f;
    if (obj.rings != nullptr && (renderFlags & ShowPlanetRings) != 0)
    {
        // calculate ring segment size in pixels, actual size is segmentSizeInPixels * tan(segmentAngle)
        segmentSizeInPixels = 2.0f * obj.rings->outerRadius / (max(nearPlaneDistance, altitude) * pixelSize);
        if (distance <= obj.rings->innerRadius)
        {
            renderRings_GLSL(*obj.rings, ri, ls,
                             radius, 1.0f - obj.semiAxes.y(),
                             textureResolution,
                             (renderFlags & ShowRingShadows) != 0 && lit,
                             segmentSizeInPixels,
                             this);
        }
    }

    if (atmosphere != nullptr)
    {
        // Compute the apparent thickness in pixels of the atmosphere.
        // If it's only one pixel thick, it can look quite unsightly
        // due to aliasing.  To avoid popping, we gradually fade in the
        // atmosphere as it grows from two to three pixels thick.
        float fade;
        float thicknessInPixels = 0.0f;
        if (distance - radius > 0.0f)
        {
            thicknessInPixels = atmosphere->height /
                ((distance - radius) * pixelSize);
            fade = clamp(thicknessInPixels - 2);
        }
        else
        {
            fade = 1.0f;
        }

        if (fade > 0 && (renderFlags & ShowAtmospheres) != 0)
        {
            // Only use new atmosphere code in OpenGL 2.0 path when new style parameters are defined.
            // TODO: convert old style atmopshere parameters
            if (atmosphere->mieScaleHeight > 0.0f)
            {
                float atmScale = 1.0f + atmosphere->height / radius;

                renderAtmosphere_GLSL(ri, ls,
                                      atmosphere,
                                      radius * atmScale,
                                      obj.orientation,
                                      viewFrustum,
                                      this);
            }
            else
            {
                glPushMatrix();
                glLoadIdentity();
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

                glRotate(cameraOrientation);

                renderEllipsoidAtmosphere(*atmosphere,
                                          pos,
                                          obj.orientation,
                                          scaleFactors,
                                          ri.sunDir_eye,
                                          ls,
                                          thicknessInPixels,
                                          lit);
                glPopMatrix();
            }
        }

        // If there's a cloud layer, we'll render it now.
        if (cloudTex != nullptr)
        {
            glPushMatrix();

            float cloudScale = 1.0f + atmosphere->cloudHeight / radius;
            glScalef(cloudScale, cloudScale, cloudScale);

            // If we're beneath the cloud level, render the interior of
            // the cloud sphere.
            if (distance - radius < atmosphere->cloudHeight)
                glFrontFace(GL_CW);

            glDepthMask(GL_FALSE);
            cloudTex->bind();
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Cloud layers can be trouble for the depth buffer, since they tend
            // to be very close to the surface of a planet relative to the radius
            // of the planet. We'll help out by offsetting the cloud layer toward
            // the viewer.
            if (distance > radius * 1.1f)
            {
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(-1.0f, -1.0f);
            }

            if (lit)
            {
                renderClouds_GLSL(ri, ls,
                                  atmosphere,
                                  cloudTex,
                                  cloudNormalMap,
                                  cloudTexOffset,
                                  scaleFactors,
                                  textureResolution,
                                  renderFlags,
                                  obj.orientation,
                                  viewFrustum,
                                  this);
            }
            else
            {
                renderCloudsUnlit(ri, viewFrustum, cloudTex, cloudTexOffset, this);
            }

            glDisable(GL_POLYGON_OFFSET_FILL);

            // Reset the texture matrix
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);

            glDepthMask(GL_TRUE);
            glFrontFace(GL_CCW);

            glPopMatrix();
        }
    }

    if (obj.rings != nullptr && (renderFlags & ShowPlanetRings) != 0)
    {
        if (lit && (renderFlags & ShowRingShadows) != 0)
        {
            Texture* ringsTex = obj.rings->texture.find(textureResolution);
            if (ringsTex != nullptr)
            {
                ringsTex->bind();
            }
        }

        if (distance > obj.rings->innerRadius)
        {
            glDepthMask(GL_FALSE);
            renderRings_GLSL(*obj.rings, ri, ls,
                             radius, 1.0f - obj.semiAxes.y(),
                             textureResolution,
                             (renderFlags & ShowRingShadows) != 0 && lit,
                             segmentSizeInPixels,
                             this);
        }
    }

    glPopMatrix();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
}


bool Renderer::testEclipse(const Body& receiver,
                           const Body& caster,
                           LightingState& lightingState,
                           unsigned int lightIndex,
                           double now)
{
    const DirectionalLight& light = lightingState.lights[lightIndex];
    LightingState::EclipseShadowVector& shadows = *lightingState.shadows[lightIndex];
    bool isReceiverShadowed = false;

    // Ignore situations where the shadow casting body is much smaller than
    // the receiver, as these shadows aren't likely to be relevant.  Also,
    // ignore eclipses where the caster is not an ellipsoid, since we can't
    // generate correct shadows in this case.
    if (caster.getRadius() >= receiver.getRadius() * MinRelativeOccluderRadius &&
        caster.hasVisibleGeometry() &&
        caster.extant(now) &&
        caster.isEllipsoid())
    {
        // All of the eclipse related code assumes that both the caster
        // and receiver are spherical.  Irregular receivers will work more
        // or less correctly, but casters that are sufficiently non-spherical
        // will produce obviously incorrect shadows.  Another assumption we
        // make is that the distance between the caster and receiver is much
        // less than the distance between the sun and the receiver.  This
        // approximation works everywhere in the solar system, and is likely
        // valid for any orbitally stable pair of objects orbiting a star.
        Vector3d posReceiver = receiver.getAstrocentricPosition(now);
        Vector3d posCaster = caster.getAstrocentricPosition(now);

        //const Star* sun = receiver.getSystem()->getStar();
        //assert(sun != nullptr);
        //double distToSun = posReceiver.distanceFromOrigin();
        //float appSunRadius = (float) (sun->getRadius() / distToSun);
        float appSunRadius = light.apparentSize;

        Vector3d dir = posCaster - posReceiver;
        double distToCaster = dir.norm() - receiver.getRadius();
        float appOccluderRadius = (float) (caster.getRadius() / distToCaster);

        // The shadow radius is the radius of the occluder plus some additional
        // amount that depends upon the apparent radius of the sun.  For
        // a sun that's distant/small and effectively a point, the shadow
        // radius will be the same as the radius of the occluder.
        float shadowRadius = (1 + appSunRadius / appOccluderRadius) *
            caster.getRadius();

        // Test whether a shadow is cast on the receiver.  We want to know
        // if the receiver lies within the shadow volume of the caster.  Since
        // we're assuming that everything is a sphere and the sun is far
        // away relative to the caster, the shadow volume is a
        // cylinder capped at one end.  Testing for the intersection of a
        // singly capped cylinder is as simple as checking the distance
        // from the center of the receiver to the axis of the shadow cylinder.
        // If the distance is less than the sum of the caster's and receiver's
        // radii, then we have an eclipse. We also need to verify that the
        // receiver is behind the caster when seen from the light source.
        float R = receiver.getRadius() + shadowRadius;

        // The stored light position is receiver-relative; thus the caster-to-light
        // direction is casterPos - (receiverPos + lightPos)
        Vector3d lightPosition = posReceiver + light.position;
        Vector3d lightToCasterDir = posCaster - lightPosition;
        Vector3d receiverToCasterDir = posReceiver - posCaster;

        double dist = distance(posReceiver,
                               Ray3d(posCaster, lightToCasterDir));
        if (dist < R && lightToCasterDir.dot(receiverToCasterDir) > 0.0)
        {
            Vector3d sunDir = lightToCasterDir.normalized();

            EclipseShadow shadow;
            shadow.origin = dir.cast<float>();
            shadow.direction = sunDir.cast<float>();
            shadow.penumbraRadius = shadowRadius;

            // The umbra radius will be positive if the apparent size of the occluder
            // is greater than the apparent size of the sun, zero if they're equal,
            // and negative when the eclipse is partial. The absolute value of the
            // umbra radius is the radius of the shadow region with constant depth:
            // for total eclipses, this area is actually the umbra, with a depth of
            // 1. For annular eclipses and transits, it is less than 1.
            shadow.umbraRadius = caster.getRadius() *
                (appOccluderRadius - appSunRadius) / appOccluderRadius;
            shadow.maxDepth = std::min(1.0f, square(appOccluderRadius / appSunRadius));
            shadow.caster = &caster;

            // Ignore transits that don't produce a visible shadow.
            if (shadow.maxDepth > 1.0f / 256.0f)
                shadows.push_back(shadow);

            isReceiverShadowed = true;
        }

        // If the caster has a ring system, see if it casts a shadow on the receiver.
        // Ring shadows are only supported in the OpenGL 2.0 path.
        if (caster.getRings())
        {
            bool shadowed = false;

            // The shadow volume of the rings is an oblique circular cylinder
            if (dist < caster.getRings()->outerRadius + receiver.getRadius())
            {
                // Possible intersection, but it depends on the orientation of the
                // rings.
                Quaterniond casterOrientation = caster.getOrientation(now);
                Vector3d ringPlaneNormal = casterOrientation * Vector3d::UnitY();
                Vector3d shadowDirection = lightToCasterDir.normalized();
                Vector3d v = ringPlaneNormal.cross(shadowDirection);
                if (v.squaredNorm() < 1.0e-6)
                {
                    // Shadow direction is nearly coincident with ring plane normal, so
                    // the shadow cross section is close to circular. No additional test
                    // is required.
                    shadowed = true;
                }
                else
                {
                    // minDistance is the cross section of the ring shadows in the plane
                    // perpendicular to the ring plane and containing the light direction.
                    Vector3d shadowPlaneNormal = v.normalized().cross(shadowDirection);
                    Hyperplane<double, 3> shadowPlane(shadowPlaneNormal, posCaster - posReceiver);
                    double minDistance = receiver.getRadius() +
                        caster.getRings()->outerRadius * ringPlaneNormal.dot(shadowDirection);
                    if (abs(shadowPlane.signedDistance(Vector3d::Zero())) < minDistance)
                    {
                        // TODO: Implement this test and only set shadowed to true if it passes
                    }
                    shadowed = true;
                }

                if (shadowed)
                {
                    RingShadow& shadow = lightingState.ringShadows[lightIndex];
                    shadow.origin = dir.cast<float>();
                    shadow.direction = shadowDirection.cast<float>();
                    shadow.ringSystem = caster.getRings();
                    shadow.casterOrientation = casterOrientation.cast<float>();
                }
            }
        }
    }

    return isReceiverShadowed;
}


void Renderer::renderPlanet(Body& body,
                            const Vector3f& pos,
                            float distance,
                            float appMag,
                            const Observer& observer,
                            const Quaternionf& cameraOrientation,
                            float nearPlaneDistance,
                            float farPlaneDistance)
{
    double now = observer.getTime();
    float altitude = distance - body.getRadius();
    float discSizeInPixels = body.getRadius() /
        (max(nearPlaneDistance, altitude) * pixelSize);

    if (discSizeInPixels > 1 && body.hasVisibleGeometry())
    {
        RenderProperties rp;

        if (displayedSurface.empty())
        {
            rp.surface = const_cast<Surface*>(&body.getSurface());
        }
        else
        {
            rp.surface = body.getAlternateSurface(displayedSurface);
            if (rp.surface == nullptr)
                rp.surface = const_cast<Surface*>(&body.getSurface());
        }
        rp.atmosphere = body.getAtmosphere();
        rp.rings = body.getRings();
        rp.radius = body.getRadius();
        rp.geometry = body.getGeometry();
        rp.semiAxes = body.getSemiAxes() * (1.0f / rp.radius);
        rp.geometryScale = body.getGeometryScale();

        Quaterniond q = body.getRotationModel(now)->spin(now) *
                        body.getEclipticToEquatorial(now);

        rp.orientation = body.getGeometryOrientation() * q.cast<float>();

        if (body.getLocations() != nullptr && (labelMode & LocationLabels) != 0)
            body.computeLocations();

        Vector3f scaleFactors;
        bool isNormalized = false;
        Geometry* geometry = nullptr;
        if (rp.geometry != InvalidResource)
            geometry = GetGeometryManager()->find(rp.geometry);
        if (geometry == nullptr || geometry->isNormalized())
        {
            scaleFactors = rp.semiAxes * rp.radius;
            isNormalized = true;
        }
        else
        {
            scaleFactors = Vector3f::Constant(rp.geometryScale);
        }

        LightingState lights;
        setupObjectLighting(lightSourceList,
                            secondaryIlluminators,
                            rp.orientation,
                            scaleFactors,
                            pos,
                            isNormalized,
#ifdef USE_HDR
                            faintestMag,
                            DEFAULT_EXPOSURE + brightPlus, //exposure + brightPlus,
                            appMag,
#endif
                            lights);
        assert(lights.nLights <= MaxLights);

        lights.ambientColor = Vector3f(ambientColor.red(),
                                       ambientColor.green(),
                                       ambientColor.blue());

        {
            // Clear out the list of eclipse shadows
            for (unsigned int li = 0; li < lights.nLights; li++)
            {
                eclipseShadows[li].clear();
                lights.shadows[li] = &eclipseShadows[li];
            }
        }


        // Add ring shadow records for each light
        if (body.getRings() &&
            (renderFlags & ShowPlanetRings) != 0 &&
            (renderFlags & ShowRingShadows) != 0)
        {
            for (unsigned int li = 0; li < lights.nLights; li++)
            {
                lights.ringShadows[li].ringSystem = body.getRings();
                lights.ringShadows[li].casterOrientation = q.cast<float>();
                lights.ringShadows[li].origin = Vector3f::Zero();
                lights.ringShadows[li].direction = -lights.lights[li].position.normalized().cast<float>();
            }
        }

        // Calculate eclipse circumstances
        if ((renderFlags & ShowEclipseShadows) != 0 &&
            body.getSystem() != nullptr)
        {
            PlanetarySystem* system = body.getSystem();

            if (system->getPrimaryBody() == nullptr &&
                body.getSatellites() != nullptr)
            {
                // The body is a planet.  Check for eclipse shadows
                // from all of its satellites.
                PlanetarySystem* satellites = body.getSatellites();
                if (satellites != nullptr)
                {
                    int nSatellites = satellites->getSystemSize();
                    for (unsigned int li = 0; li < lights.nLights; li++)
                    {
                        if (lights.lights[li].castsShadows)
                        {
                            for (int i = 0; i < nSatellites; i++)
                            {
                                testEclipse(body, *satellites->getBody(i), lights, li, now);
                            }
                        }
                    }
                }
            }
            else if (system->getPrimaryBody() != nullptr)
            {
                for (unsigned int li = 0; li < lights.nLights; li++)
                {
                    if (lights.lights[li].castsShadows)
                    {
                        // The body is a moon.  Check for eclipse shadows from
                        // the parent planet and all satellites in the system.
                        // Traverse up the hierarchy so that any parent objects
                        // of the parent are also considered (TODO: their child
                        // objects will not be checked for shadows.)
                        Body* planet = system->getPrimaryBody();
                        while (planet != nullptr)
                        {
                            testEclipse(body, *planet, lights, li, now);
                            if (planet->getSystem() != nullptr)
                                planet = planet->getSystem()->getPrimaryBody();
                            else
                                planet = nullptr;
                        }

                        int nSatellites = system->getSystemSize();
                        for (int i = 0; i < nSatellites; i++)
                        {
                            if (system->getBody(i) != &body)
                            {
                                testEclipse(body, *system->getBody(i), lights, li, now);
                            }
                        }
                    }
                }
            }
        }

        // Sort out the ring shadows; only one ring shadow source is supported right now. This means
        // that exotic cases with shadows from two ring different ring systems aren't handled.
        for (unsigned int li = 0; li < lights.nLights; li++)
        {
            if (lights.ringShadows[li].ringSystem != nullptr)
            {
                RingSystem* rings = lights.ringShadows[li].ringSystem;

                // Use the first set of ring shadows found (shadowing the brightest light
                // source.)
                if (lights.shadowingRingSystem == nullptr)
                {
                    lights.shadowingRingSystem = rings;
                    lights.ringPlaneNormal = (rp.orientation * lights.ringShadows[li].casterOrientation.conjugate()) * Vector3f::UnitY();
                    lights.ringCenter = rp.orientation * lights.ringShadows[li].origin;
                }

                // Light sources have a finite size, which causes some blurring of the texture. Simulate
                // this effect by using a lower LOD (i.e. a smaller mipmap level, indicated somewhat
                // confusingly by a _higher_ LOD value.
                float ringWidth = rings->outerRadius - rings->innerRadius;
                float projectedRingSize = std::abs(lights.lights[li].direction_obj.dot(lights.ringPlaneNormal)) * ringWidth;
                float projectedRingSizeInPixels = projectedRingSize / (max(nearPlaneDistance, altitude) * pixelSize);
                Texture* ringsTex = rings->texture.find(textureResolution);
                if (ringsTex)
                {
                    // Calculate the approximate distance from the shadowed object to the rings
                    Hyperplane<float, 3> ringPlane(lights.ringPlaneNormal, lights.ringCenter);
                    float cosLightAngle = lights.lights[li].direction_obj.dot(ringPlane.normal());
                    float approxRingDistance = rings->innerRadius;
                    if (abs(cosLightAngle) < 0.99999f)
                    {
                        approxRingDistance = abs(ringPlane.offset() / cosLightAngle);
                    }
                    if (lights.ringCenter.norm() < rings->innerRadius)
                    {
                        approxRingDistance = max(approxRingDistance, rings->innerRadius - lights.ringCenter.norm());
                    }

                    // Calculate the LOD based on the size of the smallest
                    // ring feature relative to the apparent size of the light source.
                    float ringTextureWidth = ringsTex->getWidth();
                    float ringFeatureSize = (projectedRingSize / ringTextureWidth) / approxRingDistance;
                    float relativeFeatureSize = lights.lights[li].apparentSize / ringFeatureSize;
                    //float areaLightLod = log(max(relativeFeatureSize, 1.0f)) / log(2.0f);
                    float areaLightLod = log2(max(relativeFeatureSize, 1.0f));

                    // Compute the LOD that would be automatically used by the GPU.
                    float texelToPixelRatio = ringTextureWidth / projectedRingSizeInPixels;
                    float gpuLod = log2(texelToPixelRatio);

                    //float lod = max(areaLightLod, log(texelToPixelRatio) / log(2.0f));
                    float lod = max(areaLightLod, gpuLod);

                    // maxLOD is the index of the smallest mipmap (or close to it for non-power-of-two
                    // textures.) We can't make the lod larger than this.
                    float maxLod = log2((float) ringsTex->getWidth());
                    if (maxLod > 1.0f)
                    {
                        // Avoid using the 1x1 mipmap, as it appears to cause 'bleeding' when
                        // the light source is very close to the ring plane. This is probably
                        // a numerical precision issue from calculating the intersection of
                        // between a ray and plane that are nearly parallel.
                        maxLod -= 1.0f;
                    }
                    lod = min(lod, maxLod);

                    // Not all hardware/drivers support GLSL's textureXDLOD instruction, which lets
                    // us explicitly set the LOD. But, they do all have an optional lodBias parameter
                    // for the textureXD instruction. The bias is just the difference between the
                    // area light LOD and the approximate GPU calculated LOD.
                    float lodBias = max(0.0f, lod - gpuLod);

                    if (GLEW_ARB_shader_texture_lod)
                    {
                        lights.ringShadows[li].texLod = lod;
                    }
                    else
                    {
                        lights.ringShadows[li].texLod = lodBias;
                    }
                }
                else
                {
                    lights.ringShadows[li].texLod = 0.0f;
                }
            }
        }

        renderObject(pos, distance, now,
                     cameraOrientation, nearPlaneDistance, farPlaneDistance,
                     rp, lights);

        if (body.getLocations() != nullptr && (labelMode & LocationLabels) != 0)
        {
            // Set up location markers for this body
            mountainRep    = MarkerRepresentation(MarkerRepresentation::Triangle, 8.0f, LocationLabelColor);
            craterRep      = MarkerRepresentation(MarkerRepresentation::Circle,   8.0f, LocationLabelColor);
            observatoryRep = MarkerRepresentation(MarkerRepresentation::Plus,     8.0f, LocationLabelColor);
            cityRep        = MarkerRepresentation(MarkerRepresentation::X,        3.0f, LocationLabelColor);
            genericLocationRep = MarkerRepresentation(MarkerRepresentation::Square, 8.0f, LocationLabelColor);

            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_BLEND);

            // We need a double precision body-relative position of the
            // observer, otherwise location labels will tend to jitter.
            Vector3d posd = body.getPosition(observer.getTime()).offsetFromKm(observer.getPosition());
            renderLocations(body, posd, q);

            glDisable(GL_DEPTH_TEST);
        }
    }

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
#endif

    if (body.isVisibleAsPoint())
    {
        renderObjectAsPoint(pos,
                            body.getRadius(),
                            appMag,
                            faintestMag,
                            discSizeInPixels,
                            body.getSurface().color,
                            cameraOrientation,
                            false, false);
    }
#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#endif
}


void Renderer::renderStar(const Star& star,
                          const Vector3f& pos,
                          float distance,
                          float appMag,
                          const Quaternionf& cameraOrientation,
                          double now,
                          float nearPlaneDistance,
                          float farPlaneDistance)
{
    if (!star.getVisibility())
        return;

    Color color = colorTemp->lookupColor(star.getTemperature());
    float radius = star.getRadius();
    float discSizeInPixels = radius / (distance * pixelSize);

    if (discSizeInPixels > 1)
    {
        Surface surface;
        RenderProperties rp;

        surface.color = color;

        MultiResTexture mtex = star.getTexture();
        if (mtex.tex[textureResolution] != InvalidResource)
        {
            surface.baseTexture = mtex;
        }
        else
        {
            surface.baseTexture = InvalidResource;
        }
        surface.appearanceFlags |= Surface::ApplyBaseTexture;
        surface.appearanceFlags |= Surface::Emissive;

        rp.surface = &surface;
        rp.rings = nullptr;
        rp.radius = star.getRadius();
        rp.semiAxes = star.getEllipsoidSemiAxes();
        rp.geometry = star.getGeometry();

#ifndef USE_HDR
        Atmosphere atmosphere;

        // Use atmosphere effect to give stars a fuzzy fringe
        if (star.hasCorona() && rp.geometry == InvalidResource)
        {
            Color atmColor(color.red() * 0.5f, color.green() * 0.5f, color.blue() * 0.5f);
            atmosphere.height = radius * CoronaHeight;
            atmosphere.lowerColor = atmColor;
            atmosphere.upperColor = atmColor;
            atmosphere.skyColor = atmColor;

            rp.atmosphere = &atmosphere;
        }
        else
        {
            rp.atmosphere = nullptr;
        }
#else
        rp.atmosphere = nullptr;
#endif

        rp.orientation = star.getRotationModel()->orientationAtTime(now).cast<float>();

        renderObject(pos, distance, now,
                     cameraOrientation, nearPlaneDistance, farPlaneDistance,
                     rp, LightingState());
    }

    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
#endif

    renderObjectAsPoint(pos,
                        star.getRadius(),
                        appMag,
                        faintestMag,
                        discSizeInPixels,
                        color,
                        cameraOrientation,
                        true, true);
#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#endif
}


static const int MaxCometTailPoints = 120;
static const int CometTailSlices = 48;
struct CometTailVertex
{
    Vector3f point;
    Vector3f normal;
    float brightness;
};

static CometTailVertex cometTailVertices[CometTailSlices * MaxCometTailPoints];

// Compute a rough estimate of the visible length of the dust tail.
// TODO: This is old code that needs to be rewritten. For one thing,
// the length is inversely proportional to the distance from the sun,
// whereas the 1/distance^2 is probably more realistic. There should
// also be another parameter that specifies how active the comet is.
static float cometDustTailLength(float distanceToSun,
                                 float radius)
{
    return (1.0e8f / distanceToSun) * (radius / 5.0f) * 1.0e7f;
}


// TODO: Remove unused parameters??
void Renderer::renderCometTail(const Body& body,
                               const Vector3f& pos,
                               const Observer& observer,
                               float discSizeInPixels)
{
    auto prog = shaderManager->getShader("comet");
    if (prog == nullptr)
        return;

    double now = observer.getTime();

    Vector3f cometPoints[MaxCometTailPoints];
    Vector3d pos0 = body.getOrbit(now)->positionAtTime(now);
#if 0
    Vector3d pos1 = body.getOrbit(now)->positionAtTime(now - 0.01);
    Vector3d vd = pos1 - pos0;
#endif
    double t = now;

    float distanceFromSun, irradiance_max = 0.0f;

    // Adjust the amount of triangles used for the comet tail based on
    // the screen size of the comet.
    float lod = min(1.0f, max(0.2f, discSizeInPixels / 1000.0f));
    auto nTailPoints = (int) (MaxCometTailPoints * lod);
    auto nTailSlices = (int) (CometTailSlices * lod);

    // Find the sun with the largest irrradiance of light onto the comet
    // as function of the comet's position;
    // irradiance = sun's luminosity / square(distanceFromSun);
    Vector3d sunPos(Vector3d::Zero());
    for (const auto star : nearStars)
    {
        if (star->getVisibility())
        {
            Vector3d p = star->getPosition(t).offsetFromKm(observer.getPosition());
            distanceFromSun = (float) (pos.cast<double>() - p).norm();
            float irradiance = star->getBolometricLuminosity() / square(distanceFromSun);

            if (irradiance > irradiance_max)
            {
                irradiance_max = irradiance;
                sunPos = p;
            }
        }
    }

    float fadeDistance = 1.0f / (float) (COMET_TAIL_ATTEN_DIST_SOL * sqrt(irradiance_max));

    // direction to sun with dominant light irradiance:
    Vector3f sunDir = (pos.cast<double>() - sunPos).cast<float>().normalized();

    float dustTailLength = cometDustTailLength((float) pos0.norm(), body.getRadius());
    float dustTailRadius = dustTailLength * 0.1f;

    Vector3f origin = -sunDir * (body.getRadius() * 100);

    int i;
    for (i = 0; i < nTailPoints; i++)
    {
        float alpha = (float) i / (float) nTailPoints;
        alpha = alpha * alpha;
        cometPoints[i] = origin + sunDir * (dustTailLength * alpha);
    }

    // We need three axes to define the coordinate system for rendering the
    // comet.  The first axis is the sun-to-comet direction, and the other
    // two are chose orthogonal to each other and the primary axis.
    Vector3f v = (cometPoints[1] - cometPoints[0]).normalized();
    Quaternionf q = body.getEclipticToEquatorial(t).cast<float>();
    Vector3f u = v.unitOrthogonal();
    Vector3f w = u.cross(v);

    for (i = 0; i < nTailPoints; i++)
    {
        float brightness = 1.0f - (float) i / (float) (nTailPoints - 1);
        Vector3f v0, v1;
        float sectionLength;
        float w0, w1;
        // Special case the first vertex in the comet tail
        if (i == 0)
        {
            v0 = cometPoints[1] - cometPoints[0];
            sectionLength = v0.norm();
            v0.normalize();
            v1 = v0;
            w0 = 1.0f;
            w1 = 0.0f;
        }
        else
        {
            v0 = cometPoints[i] - cometPoints[i - 1];
            sectionLength = v0.norm();
            v0.normalize();

            if (i == nTailPoints - 1)
            {
                v1 = v0;
            }
            else
            {
                v1 = (cometPoints[i + 1] - cometPoints[i]).normalized();
                q.setFromTwoVectors(v0, v1);
                Matrix3f m = q.toRotationMatrix();
                u = m * u;
                v = m * v;
                w = m * w;
            }
            float dr = (dustTailRadius / (float) nTailPoints) / sectionLength;
            w0 = atan(dr);
            float d = sqrt(1.0f + w0 * w0);
            w1 = 1.0f / d;
            w0 = w0 / d;
        }

        float radius = (float) i / (float) nTailPoints * dustTailRadius;
        for (int j = 0; j < nTailSlices; j++)
        {
            float theta = (float) (2 * PI * (float) j / nTailSlices);
            float s, c;
            sincos(theta, s, c);
            CometTailVertex& vtx = cometTailVertices[i * nTailSlices + j];
            vtx.normal = u * (s * w1) + w * (c * w1) + v * w0;
            vtx.normal.normalize();
            s *= radius;
            c *= radius;

            vtx.point = cometPoints[i] + u * s + w * c;
            vtx.brightness = brightness;
        }
    }

    glPushMatrix();
    glTranslate(pos);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    prog->use();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    auto brightness = prog->attribIndex("brightness");
    if (brightness != -1)
        glEnableVertexAttribArray(brightness);
    prog->vec3Param("color") = body.getCometTailColor().toVector3();
    prog->vec3Param("viewDir") = pos.normalized();
    // If fadeDistFromSun = x/x0 >= 1.0, comet tail starts fading,
    // i.e. fadeFactor quickly transits from 1 to 0.
    float fadeFactor = 0.5f * (1.0f - tanh(fadeDistance - 1.0f / fadeDistance));
    prog->floatParam("fadeFactor") = fadeFactor;

    vector<unsigned short> indices;
    indices.reserve(nTailSlices * 2 + 2);
    for (int j = 0; j < nTailSlices; j++)
    {
        indices.push_back(j);
        indices.push_back(j + nTailSlices);
    }
    indices.push_back(0);
    indices.push_back(nTailSlices);

    const size_t stride = sizeof(CometTailVertex);
    for (i = 0; i < nTailPoints - 1; i++)
    {
        const auto p = &cometTailVertices[i * nTailSlices];
        glVertexPointer(3, GL_FLOAT, stride, &p->point);
        glNormalPointer(GL_FLOAT, stride, &p->normal);
        if (brightness != -1)
            glVertexAttribPointer(brightness, 1, GL_FLOAT, GL_FALSE, stride, &p->brightness);
        glDrawElements(GL_TRIANGLE_STRIP, indices.size(), GL_UNSIGNED_SHORT, indices.data());
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    if (brightness == -1)
        glDisableVertexAttribArray(brightness);
    glEnable(GL_CULL_FACE);
    glUseProgram(0);

#ifdef DEBUG_COMET_TAIL
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glColor4f(0.0f, 1.0f, 1.0f, 0.5f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, cometPoints);
    glDrawArrays(GL_LINE_STRIP, 0, nTailPoints);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
#endif

    glPopMatrix();
}


// Render a reference mark
void Renderer::renderReferenceMark(const ReferenceMark& refMark,
                                   const Vector3f& pos,
                                   float distance,
                                   double now,
                                   float nearPlaneDistance)
{
    float altitude = distance - refMark.boundingSphereRadius();
    float discSizeInPixels = refMark.boundingSphereRadius() /
        (max(nearPlaneDistance, altitude) * pixelSize);

    if (discSizeInPixels <= 1)
        return;

    // Apply the modelview transform for the object
    glPushMatrix();
    glTranslate(pos);

    refMark.render(this, pos, discSizeInPixels, now);

    glPopMatrix();

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
}


void Renderer::renderAsterisms(const Universe& universe, float dist)
{
    if ((renderFlags & ShowDiagrams) == 0 || universe.getAsterisms() == nullptr)
        return;

    float opacity = 1.0f;
    if (dist > MaxAsterismLinesConstDist)
    {
        opacity = clamp((MaxAsterismLinesConstDist - dist) /
                        (MaxAsterismLinesDist - MaxAsterismLinesConstDist) + 1);
    }

    glDisable(GL_TEXTURE_2D);
    enableSmoothLines(renderFlags);
    universe.getAsterisms()->render(Color(ConstellationColor, opacity), *this);
    disableSmoothLines(renderFlags);
}


void Renderer::renderBoundaries(const Universe& universe, float dist)
{
    if ((renderFlags & ShowBoundaries) == 0 || universe.getBoundaries() == nullptr)
        return;

    /* We'll linearly fade the boundaries as a function of the
       observer's distance to the origin of coordinates: */
    float opacity = 1.0f;
    if (dist > MaxAsterismLabelsConstDist)
    {
        opacity = clamp((MaxAsterismLabelsConstDist - dist) /
                        (MaxAsterismLabelsDist - MaxAsterismLabelsConstDist) + 1);
    }

    glDisable(GL_TEXTURE_2D);
    enableSmoothLines(renderFlags);
    universe.getBoundaries()->render(Color(BoundaryColor, opacity), *this);
    disableSmoothLines(renderFlags);
}


// Helper function to compute the luminosity of a perfectly
// reflective disc with the specified radius. This is used as an upper
// bound for the apparent brightness of an object when culling
// invisible objects.
static float luminosityAtOpposition(float sunLuminosity,
                                    float distanceFromSun,
                                    float objRadius)
{
    // Compute the total power of the star in Watts
    double power = astro::SOLAR_POWER * sunLuminosity;

    // Compute the irradiance at the body's distance from the star
    double irradiance = power / sphereArea(distanceFromSun * 1000);

    // Compute the total energy hitting the planet; assume an albedo of 1.0, so
    // reflected energy = incident energy.
    double incidentEnergy = irradiance * circleArea(objRadius * 1000);

    // Compute the luminosity (i.e. power relative to solar power)
    return (float) (incidentEnergy / astro::SOLAR_POWER);
}


static bool isBodyVisible(const Body* body, int bodyVisibilityMask)
{
    int klass = body->getClassification();
    switch (body->getClassification())
    {
    // Diffuse objects don't have controls to show/hide visibility
    case Body::Diffuse:
        return body->isVisible();

    // SurfaceFeature inherits visibility of its parent body
    case Body::SurfaceFeature:
        assert(body->getSystem() != nullptr);
        body = body->getSystem()->getPrimaryBody();
        assert(body != nullptr);
        return body->isVisible() && (bodyVisibilityMask & body->getClassification()) != 0;

    default:
        return body->isVisible() && (bodyVisibilityMask & klass) != 0;
    }
}

void Renderer::addRenderListEntries(RenderListEntry& rle,
                                    Body& body,
                                    bool isLabeled)
{
    bool visibleAsPoint = rle.appMag < faintestPlanetMag && body.isVisibleAsPoint();

    if (rle.discSizeInPixels > 1 || visibleAsPoint || isLabeled)
    {
        rle.renderableType = RenderListEntry::RenderableBody;
        rle.body = &body;

        if (body.getGeometry() != InvalidResource && rle.discSizeInPixels > 1)
        {
            Geometry* geometry = GetGeometryManager()->find(body.getGeometry());
            if (geometry == nullptr)
                rle.isOpaque = true;
            else
                rle.isOpaque = geometry->isOpaque();
        }
        else
        {
            rle.isOpaque = true;
        }
        rle.radius = body.getRadius();
        renderList.push_back(rle);
    }

    if (body.getClassification() == Body::Comet && (renderFlags & ShowCometTails) != 0)
    {
        float radius = cometDustTailLength(rle.sun.norm(), body.getRadius());
        float discSize = (radius / (float) rle.distance) / pixelSize;
        if (discSize > 1)
        {
            rle.renderableType = RenderListEntry::RenderableCometTail;
            rle.body = &body;
            rle.isOpaque = false;
            rle.radius = radius;
            rle.discSizeInPixels = discSize;
            renderList.push_back(rle);
        }
    }

    const list<ReferenceMark*>* refMarks = body.getReferenceMarks();
    if (refMarks != nullptr)
    {
        for (const auto rm : *refMarks)
        {
            rle.renderableType = RenderListEntry::RenderableReferenceMark;
            rle.refMark = rm;
            rle.isOpaque = rm->isOpaque();
            rle.radius = rm->boundingSphereRadius();
            renderList.push_back(rle);
        }
    }
}


void Renderer::buildRenderLists(const Vector3d& astrocentricObserverPos,
                                const Frustum& viewFrustum,
                                const Vector3d& viewPlaneNormal,
                                const Vector3d& frameCenter,
                                const FrameTree* tree,
                                const Observer& observer,
                                double now)
{
    int labelClassMask = translateLabelModeToClassMask(labelMode);

    Matrix3f viewMat = observer.getOrientationf().toRotationMatrix();
    Vector3f viewMatZ = viewMat.row(2);
    double invCosViewAngle = 1.0 / cosViewConeAngle;
    double sinViewAngle = sqrt(1.0 - square(cosViewConeAngle));

    unsigned int nChildren = tree != nullptr ? tree->childCount() : 0;
    for (unsigned int i = 0; i < nChildren; i++)
    {
        auto phase = tree->getChild(i);

        // No need to do anything if the phase isn't active now
        if (!phase->includes(now))
            continue;

        Body* body = phase->body();

        // pos_s: sun-relative position of object
        // pos_v: viewer-relative position of object

        // Get the position of the body relative to the sun.
        Vector3d p = phase->orbit()->positionAtTime(now);
        auto frame = phase->orbitFrame();
        Vector3d pos_s = frameCenter + frame->getOrientation(now).conjugate() * p;

        // We now have the positions of the observer and the planet relative
        // to the sun.  From these, compute the position of the body
        // relative to the observer.
        Vector3d pos_v = pos_s - astrocentricObserverPos;

        // dist_vn: distance along view normal from the viewer to the
        // projection of the object's center.
        double dist_vn = viewPlaneNormal.dot(pos_v);

        // Vector from object center to its projection on the view normal.
        Vector3d toViewNormal = pos_v - dist_vn * viewPlaneNormal;

        float cullingRadius = body->getCullingRadius();

        // The result of the planetshine test can be reused for the view cone
        // test, but only when the object's light influence sphere is larger
        // than the geometry. This is not
        bool viewConeTestFailed = false;
        if (body->isSecondaryIlluminator())
        {
            float influenceRadius = body->getBoundingRadius() + (body->getRadius() * PLANETSHINE_DISTANCE_LIMIT_FACTOR);
            if (dist_vn > -influenceRadius)
            {
                double maxPerpDist = (influenceRadius + dist_vn * sinViewAngle) * invCosViewAngle;
                double perpDistSq = toViewNormal.squaredNorm();
                if (perpDistSq < maxPerpDist * maxPerpDist)
                {
                    if ((body->getRadius() / (float) pos_v.norm()) / pixelSize > PLANETSHINE_PIXEL_SIZE_LIMIT)
                    {
                        // add to planetshine list if larger than 1/10 pixel
#if DEBUG_SECONDARY_ILLUMINATION
                        clog << "Planetshine: " << body->getName()
                             << ", " << body->getRadius() / (float) pos_v.length() / pixelSize << endl;
#endif
                        SecondaryIlluminator illum;
                        illum.body = body;
                        illum.position_v = pos_v;
                        illum.radius = body->getRadius();
                        secondaryIlluminators.push_back(illum);
                    }
                }
                else
                {
                    viewConeTestFailed = influenceRadius > cullingRadius;
                }
            }
            else
            {
                viewConeTestFailed = influenceRadius > cullingRadius;
            }
        }

        bool insideViewCone = false;
        if (!viewConeTestFailed)
        {
            float radius = body->getCullingRadius();
            if (dist_vn > -radius)
            {
                double maxPerpDist = (radius + dist_vn * sinViewAngle) * invCosViewAngle;
                double perpDistSq = toViewNormal.squaredNorm();
                insideViewCone = perpDistSq < maxPerpDist * maxPerpDist;
            }
        }

        if (insideViewCone)
        {
            // Calculate the distance to the viewer
            double dist_v = pos_v.norm();

            // Calculate the size of the planet/moon disc in pixels
            float discSize = (body->getCullingRadius() / (float) dist_v) / pixelSize;

            // Compute the apparent magnitude; instead of summing the reflected
            // light from all nearby stars, we just consider the one with the
            // highest apparent brightness.
            float appMag = 100.0f;
            for (unsigned int li = 0; li < lightSourceList.size(); li++)
            {
                Vector3d sunPos = pos_v - lightSourceList[li].position;
                appMag = min(appMag, body->getApparentMagnitude(lightSourceList[li].luminosity, sunPos, pos_v));
            }

            bool visibleAsPoint = appMag < faintestPlanetMag && body->isVisibleAsPoint();
            bool isLabeled = (body->getOrbitClassification() & labelClassMask) != 0;

            if ((discSize > 1 || visibleAsPoint || isLabeled) && isBodyVisible(body, bodyVisibilityMask))
            {
                RenderListEntry rle;

                rle.position = pos_v.cast<float>();
                rle.distance = (float) dist_v;
                rle.centerZ = pos_v.cast<float>().dot(viewMatZ);
                rle.appMag   = appMag;
                rle.discSizeInPixels = body->getRadius() / ((float) dist_v * pixelSize);

                // TODO: Remove this. It's only used in two places: for calculating comet tail
                // length, and for calculating sky brightness to adjust the limiting magnitude.
                // In both cases, it's the wrong quantity to use (e.g. for objects with orbits
                // defined relative to the SSB.)
                rle.sun = -pos_s.cast<float>();

                addRenderListEntries(rle, *body, isLabeled);
            }
        }

        const FrameTree* subtree = body->getFrameTree();
        if (subtree != nullptr)
        {
            double dist_v = pos_v.norm();
            bool traverseSubtree = false;

            // There are two different tests available to determine whether we can reject
            // the object's subtree. If the subtree contains no light reflecting objects,
            // then render the subtree only when:
            //    - the subtree bounding sphere intersects the view frustum, and
            //    - the subtree contains an object bright or large enough to be visible.
            // Otherwise, render the subtree when any of the above conditions are
            // true or when a subtree object could potentially illuminate something
            // in the view cone.
            auto minPossibleDistance = (float) (dist_v - subtree->boundingSphereRadius());
            float brightestPossible = 0.0;
            float largestPossible = 0.0;

            // If the viewer is not within the subtree bounding sphere, see if we can cull it because
            // it contains no objects brighter than the limiting magnitude and no objects that will
            // be larger than one pixel in size.
            if (minPossibleDistance > 1.0f)
            {
                // Figure out the magnitude of the brightest possible object in the subtree.

                // Compute the luminosity from reflected light of the largest object in the subtree
                float lum = 0.0f;
                for (unsigned int li = 0; li < lightSourceList.size(); li++)
                {
                    Vector3d sunPos = pos_v - lightSourceList[li].position;
                    lum += luminosityAtOpposition(lightSourceList[li].luminosity, (float) sunPos.norm(), (float) subtree->maxChildRadius());
                }
                brightestPossible = astro::lumToAppMag(lum, astro::kilometersToLightYears(minPossibleDistance));
                largestPossible = (float) subtree->maxChildRadius() / (float) minPossibleDistance / pixelSize;
            }
            else
            {
                // Viewer is within the bounding sphere, so the object could be very close.
                // Assume that an object in the subree could be very bright or large,
                // so no culling will occur.
                brightestPossible = -100.0f;
                largestPossible = 100.0f;
            }

            if (brightestPossible < faintestPlanetMag || largestPossible > 1.0f)
            {
                // See if the object or any of its children are within the view frustum
                if (viewFrustum.testSphere(pos_v.cast<float>(), (float) subtree->boundingSphereRadius()) != Frustum::Outside)
                {
                    traverseSubtree = true;
                }
            }

            // If the subtree contains secondary illuminators, do one last check if it hasn't
            // already been determined if we need to traverse the subtree: see if something
            // in the subtree could possibly contribute significant illumination to an
            // object in the view cone.
            if (subtree->containsSecondaryIlluminators() &&
                !traverseSubtree                         &&
                largestPossible > PLANETSHINE_PIXEL_SIZE_LIMIT)
            {
                auto influenceRadius = (float) (subtree->boundingSphereRadius() +
                    (subtree->maxChildRadius() * PLANETSHINE_DISTANCE_LIMIT_FACTOR));
                if (dist_vn > -influenceRadius)
                {
                    double maxPerpDist = (influenceRadius + dist_vn * sinViewAngle) * invCosViewAngle;
                    double perpDistSq = toViewNormal.squaredNorm();
                    if (perpDistSq < maxPerpDist * maxPerpDist)
                        traverseSubtree = true;
                }
            }

            if (traverseSubtree)
            {
                buildRenderLists(astrocentricObserverPos,
                                 viewFrustum,
                                 viewPlaneNormal,
                                 pos_s,
                                 subtree,
                                 observer,
                                 now);
            }
        } // end subtree traverse
    }
}


void Renderer::buildOrbitLists(const Vector3d& astrocentricObserverPos,
                               const Quaterniond& observerOrientation,
                               const Frustum& viewFrustum,
                               const FrameTree* tree,
                               double now)
{
    Matrix3d viewMat = observerOrientation.toRotationMatrix();
    Vector3d viewMatZ = viewMat.row(2);

    unsigned int nChildren = tree != nullptr ? tree->childCount() : 0;
    for (unsigned int i = 0; i < nChildren; i++)
    {
        auto phase = tree->getChild(i);

        // No need to do anything if the phase isn't active now
        if (!phase->includes(now))
            continue;

        Body* body = phase->body();

        // pos_s: sun-relative position of object
        // pos_v: viewer-relative position of object

        // Get the position of the body relative to the sun.
        Vector3d pos_s = body->getAstrocentricPosition(now);

        // We now have the positions of the observer and the planet relative
        // to the sun.  From these, compute the position of the body
        // relative to the observer.
        Vector3d pos_v = pos_s - astrocentricObserverPos;

        // Only show orbits for major bodies or selected objects.
        Body::VisibilityPolicy orbitVis = body->getOrbitVisibility();

        if (body->isVisible() &&
            (body == highlightObject.body() ||
             orbitVis == Body::AlwaysVisible ||
             (orbitVis == Body::UseClassVisibility && (body->getOrbitClassification() & orbitMask) != 0)))
        {
            Vector3d orbitOrigin = Vector3d::Zero();
            Selection centerObject = phase->orbitFrame()->getCenter();
            if (centerObject.body() != nullptr)
            {
                orbitOrigin = centerObject.body()->getAstrocentricPosition(now);
            }

            // Calculate the origin of the orbit relative to the observer
            Vector3d relOrigin = orbitOrigin - astrocentricObserverPos;

            // Compute the size of the orbit in pixels
            double originDistance = pos_v.norm();
            double boundingRadius = body->getOrbit(now)->getBoundingRadius();
            auto orbitRadiusInPixels = (float) (boundingRadius / (originDistance * pixelSize));

            if (orbitRadiusInPixels > minOrbitSize)
            {
                // Add the orbit of this body to the list of orbits to be rendered
                OrbitPathListEntry path;
                path.body = body;
                path.star = nullptr;
                path.centerZ = (float) relOrigin.dot(viewMatZ);
                path.radius = (float) boundingRadius;
                path.origin = relOrigin;
                path.opacity = sizeFade(orbitRadiusInPixels, minOrbitSize, 2.0f);
                orbitPathList.push_back(path);
            }
        }

        const FrameTree* subtree = body->getFrameTree();
        if (subtree != nullptr)
        {
            // Only try to render orbits of child objects when:
            //   - The apparent size of the subtree bounding sphere is large enough that
            //     orbit paths will be visible, and
            //   - The subtree bounding sphere isn't outside the view frustum
            double dist_v = pos_v.norm();
            auto distanceToBoundingSphere = (float) (dist_v - subtree->boundingSphereRadius());
            bool traverseSubtree = false;
            if (distanceToBoundingSphere > 0.0f)
            {
                // We're inside the subtree's bounding sphere
                traverseSubtree = true;
            }
            else
            {
                float maxPossibleOrbitSize = (float) subtree->boundingSphereRadius() / ((float) dist_v * pixelSize);
                if (maxPossibleOrbitSize > minOrbitSize)
                    traverseSubtree = true;
            }

            if (traverseSubtree)
            {
                // See if the object or any of its children are within the view frustum
                if (viewFrustum.testSphere(pos_v.cast<float>(), (float) subtree->boundingSphereRadius()) != Frustum::Outside)
                {
                    buildOrbitLists(astrocentricObserverPos,
                                    observerOrientation,
                                    viewFrustum,
                                    subtree,
                                    now);
                }
            }
        } // end subtree traverse
    }
}


void Renderer::buildLabelLists(const Frustum& viewFrustum,
                               double now)
{
    int labelClassMask = translateLabelModeToClassMask(labelMode);
    Body* lastPrimary = nullptr;
    Sphered primarySphere;

    for (auto& render_item : renderList)
    {
        if (render_item.renderableType != RenderListEntry::RenderableBody)
            continue;

        int classification = render_item.body->getOrbitClassification();

        if ((classification & labelClassMask) != 0 &&
            viewFrustum.testSphere(render_item.position, render_item.radius) != Frustum::Outside)
        {
            const Body* body = render_item.body;
            Vector3f pos = render_item.position;

            auto boundingRadiusSize = (float) (body->getOrbit(now)->getBoundingRadius() / render_item.distance) / pixelSize;
            if (boundingRadiusSize > minOrbitSize)
            {
                Color labelColor;
                float opacity = sizeFade(boundingRadiusSize, minOrbitSize, 2.0f);

                switch (classification)
                {
                case Body::Planet:
                    labelColor = PlanetLabelColor;
                    break;
                case Body::DwarfPlanet:
                    labelColor = DwarfPlanetLabelColor;
                    break;
                case Body::Moon:
                    labelColor = MoonLabelColor;
                    break;
                case Body::MinorMoon:
                    labelColor = MinorMoonLabelColor;
                    break;
                case Body::Asteroid:
                    labelColor = AsteroidLabelColor;
                    break;
                case Body::Comet:
                    labelColor = CometLabelColor;
                    break;
                case Body::Spacecraft:
                    labelColor = SpacecraftLabelColor;
                    break;
                default:
                    labelColor = Color::Black;
                    break;
                }

                labelColor = Color(labelColor, opacity * labelColor.alpha());

                if (!body->getName().empty())
                {
                    bool isBehindPrimary = false;

                    auto phase = body->getTimeline()->findPhase(now);
                    Body* primary = phase->orbitFrame()->getCenter().body();
                    if (primary != nullptr && (primary->getClassification() & Body::Invisible) != 0)
                    {
                        Body* parent = phase->orbitFrame()->getCenter().body();
                        if (parent != nullptr)
                            primary = parent;
                    }

                    // Position the label slightly in front of the object along a line from
                    // object center to viewer.
                    pos = pos * (1.0f - body->getBoundingRadius() * 1.01f / pos.norm());

                    // Try and position the label so that it's not partially
                    // occluded by other objects. We'll consider just the object
                    // that the labeled body is orbiting (its primary) as a
                    // potential occluder. If a ray from the viewer to labeled
                    // object center intersects the occluder first, skip
                    // rendering the object label. Otherwise, ensure that the
                    // label is completely in front of the primary by projecting
                    // it onto the plane tangent to the primary at the
                    // viewer-primary intersection point. Whew. Don't do any of
                    // this if the primary isn't an ellipsoid.
                    //
                    // This only handles the problem of partial label occlusion
                    // for low orbiting and surface positioned objects, but that
                    // case is *much* more common than other possibilities.
                    if (primary != nullptr && primary->isEllipsoid())
                    {
                        // In the typical case, we're rendering labels for many
                        // objects that orbit the same primary. Avoid repeatedly
                        // calling getPosition() by caching the last primary
                        // position.
                        if (primary != lastPrimary)
                        {
                            Vector3d p = phase->orbitFrame()->getOrientation(now).conjugate() *
                                         phase->orbit()->positionAtTime(now);
                            Vector3d v = render_item.position.cast<double>() - p;

                            primarySphere = Sphered(v, primary->getRadius());
                            lastPrimary = primary;
                        }

                        Ray3d testRay(Vector3d::Zero(), pos.cast<double>());

                        // Test the viewer-to-labeled object ray against
                        // the primary sphere (TODO: handle ellipsoids)
                        double t = 0.0;
                        if (testIntersection(testRay, primarySphere, t))
                        {
                            // Center of labeled object is behind primary
                            // sphere; mark it for rejection.
                            isBehindPrimary = t < 1.0;
                        }

                        if (!isBehindPrimary)
                        {
                            // Not rejected. Compute the plane tangent to
                            // the primary at the viewer-to-primary
                            // intersection point.
                            Vector3d primaryVec = primarySphere.center;
                            double distToPrimary = primaryVec.norm();
                            Hyperplane<double, 3> primaryTangentPlane(primaryVec,
                                                                      primaryVec.dot(primaryVec * (1.0 - primarySphere.radius / distToPrimary)));

                            // Compute the intersection of the viewer-to-labeled
                            // object ray with the tangent plane.
                            float u = (float) (primaryTangentPlane.offset() / primaryTangentPlane.normal().dot(pos.cast<double>()));

                            // If the intersection point is closer to the viewer
                            // than the label, then project the label onto the
                            // tangent plane.
                            if (u < 1.0f && u > 0.0f)
                            {
                                pos = pos * u;
                            }
                        }
                    }

                    addSortedAnnotation(nullptr, body->getName(true), labelColor, pos);
                }
            }
        }
    } // for each render list entry
}


// Add a star orbit to the render list
void Renderer::addStarOrbitToRenderList(const Star& star,
                                        const Observer& observer,
                                        double now)
{
    // If the star isn't fixed, add its orbit to the render list
    if ((renderFlags & ShowOrbits) != 0 &&
        ((orbitMask & Body::Stellar) != 0 || highlightObject.star() == &star))
    {
        Matrix3d viewMat = observer.getOrientation().toRotationMatrix();
        Vector3d viewMatZ = viewMat.row(2);

        if (star.getOrbit() != nullptr)
        {
            // Get orbit origin relative to the observer
            Vector3d orbitOrigin = star.getOrbitBarycenterPosition(now).offsetFromKm(observer.getPosition());

            // Compute the size of the orbit in pixels
            double originDistance = orbitOrigin.norm();
            double boundingRadius = star.getOrbit()->getBoundingRadius();
            auto orbitRadiusInPixels = (float) (boundingRadius / (originDistance * pixelSize));

            if (orbitRadiusInPixels > minOrbitSize)
            {
                // Add the orbit of this body to the list of orbits to be rendered
                OrbitPathListEntry path;
                path.star = &star;
                path.body = nullptr;
                path.centerZ = (float) orbitOrigin.dot(viewMatZ);
                path.radius = (float) boundingRadius;
                path.origin = orbitOrigin;
                path.opacity = sizeFade(orbitRadiusInPixels, minOrbitSize, 2.0f);
                orbitPathList.push_back(path);
            }
        }
    }
}


template <class OBJ, class PREC> class ObjectRenderer : public OctreeProcessor<OBJ, PREC>
{
 public:
    ObjectRenderer(PREC _distanceLimit);

    void process(const OBJ& /*unused*/, PREC /*unused*/, float /*unused*/) {};

 public:
    const Observer* observer{ nullptr };

#ifdef USE_GLCONTEXT
    GLContext* context{ nullptr };
#endif
    Renderer*  renderer{ nullptr };

    Eigen::Vector3f viewNormal;

    float fov{ 0.0f };
    float size{ 0.0f };
    float pixelSize{ 0.0f };
    float faintestMag{ 0.0f };
    float faintestMagNight{ 0.0f };
    float saturationMag{ 0.0f };
#ifdef USE_HDR
    float exposure{ 0.0f };
#endif
    float brightnessScale{ 0.0f };
    float brightnessBias{ 0.0f };
    float distanceLimit{ 0.0f };

    // Objects brighter than labelThresholdMag will be labeled
    float labelThresholdMag{ 0.0f };

    // These are not fully used by this template's descendants
    // but we place them here just in case a more sophisticated
    // rendering scheme is implemented:
    int nRendered{ 0 };
    int nClose{ 0 };
    int nBright{ 0 };
    int nProcessed{ 0 };
    int nLabelled{ 0 };

    uint64_t renderFlags{ 0 };
    int labelMode{ 0 };
};


template <class OBJ, class PREC>
ObjectRenderer<OBJ, PREC>::ObjectRenderer(const PREC _distanceLimit) :
    distanceLimit((float) _distanceLimit)
{
}


class PointStarRenderer : public ObjectRenderer<Star, float>
{
 public:
    PointStarRenderer();

    void process(const Star& star, float distance, float appMag);

 public:
    Vector3d obsPos;

    vector<RenderListEntry>* renderList{ nullptr };
    PointStarVertexBuffer*   starVertexBuffer{ nullptr };
    PointStarVertexBuffer*   glareVertexBuffer{ nullptr };

    const StarDatabase* starDB{ nullptr };

    bool  useScaledDiscs{ false };
    float maxDiscSize{ 1.0f };

    float cosFOV{ 1.0f };

    const ColorTemperatureTable* colorTemp{ nullptr };
    float SolarSystemMaxDistance { 1.0f };
#ifdef DEBUG_HDR_ADAPT
    float minMag;
    float maxMag;
    float minAlpha;
    float maxAlpha;
    float maxSize;
    float above;
    unsigned long countAboveN;
    unsigned long total;
#endif
};


PointStarRenderer::PointStarRenderer() :
    ObjectRenderer<Star, float>(STAR_DISTANCE_LIMIT)
{
}


void PointStarRenderer::process(const Star& star, float distance, float appMag)
{
    nProcessed++;

    Vector3f starPos = star.getPosition();

    // Calculate the difference at double precision *before* converting to float.
    // This is very important for stars that are far from the origin.
    Vector3f relPos = (starPos.cast<double>() - obsPos).cast<float>();
    float   orbitalRadius = star.getOrbitalRadius();
    bool    hasOrbit = orbitalRadius > 0.0f;

    if (distance > distanceLimit)
        return;


    // A very rough check to see if the star may be visible: is the star in
    // front of the viewer? If the star might be close (relPos.x^2 < 0.1) or
    // is moving in an orbit, we'll always regard it as potentially visible.
    // TODO: consider normalizing relPos and comparing relPos*viewNormal against
    // cosFOV--this will cull many more stars than relPos*viewNormal, at the
    // cost of a normalize per star.
    if (relPos.dot(viewNormal) > 0.0f || relPos.x() * relPos.x() < 0.1f || hasOrbit)
    {
#ifdef HDR_COMPRESS
        Color starColorFull = colorTemp->lookupColor(star.getTemperature());
        Color starColor(starColorFull.red()   * 0.5f,
                        starColorFull.green() * 0.5f,
                        starColorFull.blue()  * 0.5f);
#else
        Color starColor = colorTemp->lookupColor(star.getTemperature());
#endif
        float discSizeInPixels = 0.0f;
        float orbitSizeInPixels = 0.0f;

        if (hasOrbit)
            orbitSizeInPixels = orbitalRadius / (distance * pixelSize);

        // Special handling for stars less than one light year away . . .
        // We can't just go ahead and render a nearby star in the usual way
        // for two reasons:
        //   * It may be clipped by the near plane
        //   * It may be large enough that we should render it as a mesh
        //     instead of a particle
        // It's possible that the second condition might apply for stars
        // further than one light year away if the star is huge, the fov is
        // very small and the resolution is high.  We'll ignore this for now
        // and use the most inexpensive test possible . . .
        if (distance < 1.0f || orbitSizeInPixels > 1.0f)
        {
            // Compute the position of the observer relative to the star.
            // This is a much more accurate (and expensive) distance
            // calculation than the previous one which used the observer's
            // position rounded off to floats.
            Vector3d hPos = astrocentricPosition(observer->getPosition(),
                                                 star,
                                                 observer->getTime());
            relPos = hPos.cast<float>() * -astro::kilometersToLightYears(1.0f);
            distance = relPos.norm();

            // Recompute apparent magnitude using new distance computation
            appMag = astro::absToAppMag(star.getAbsoluteMagnitude(), distance);

            starPos = obsPos.cast<float>() + relPos * (RenderDistance / distance);

            float radius = star.getRadius();
            discSizeInPixels = radius / astro::lightYearsToKilometers(distance) / pixelSize;
            ++nClose;
        }

        // Place labels for stars brighter than the specified label threshold brightness
        if ((labelMode & Renderer::StarLabels) && appMag < labelThresholdMag)
        {
            Vector3f starDir = relPos;
            starDir.normalize();
            if (starDir.dot(viewNormal) > cosFOV)
            {
                float distr = 3.5f * (labelThresholdMag - appMag)/labelThresholdMag;
                if (distr > 1.0f)
                    distr = 1.0f;
                renderer->addBackgroundAnnotation(nullptr, starDB->getStarName(star, true),
                                                  Color(Renderer::StarLabelColor, distr * Renderer::StarLabelColor.alpha()),
                                                  relPos);
                nLabelled++;
            }
        }

        // Stars closer than the maximum solar system size are actually
        // added to the render list and depth sorted, since they may occlude
        // planets.
        if (distance > SolarSystemMaxDistance)
        {
#ifdef USE_HDR
            float satPoint = saturationMag;
            float alpha = exposure*(faintestMag - appMag)/(faintestMag - saturationMag + 0.001f);
#else
            float satPoint = faintestMag - (1.0f - brightnessBias) / brightnessScale; // TODO: precompute this value
            float alpha = (faintestMag - appMag) * brightnessScale + brightnessBias;
#endif
#ifdef DEBUG_HDR_ADAPT
            minMag = max(minMag, appMag);
            maxMag = min(maxMag, appMag);
            minAlpha = min(minAlpha, alpha);
            maxAlpha = max(maxAlpha, alpha);
            ++total;
            if (alpha > above)
            {
                ++countAboveN;
            }
#endif

            if (useScaledDiscs)
            {
                float discSize = size;
                if (alpha < 0.0f)
                {
                    alpha = 0.0f;
                }
                else if (alpha > 1.0f)
                {
                    float discScale = min(MaxScaledDiscStarSize, (float) pow(2.0f, 0.3f * (satPoint - appMag)));
                    discSize *= discScale;

                    float glareAlpha = min(0.5f, discScale / 4.0f);
                    glareVertexBuffer->addStar(relPos, Color(starColor, glareAlpha), discSize * 3.0f);

                    alpha = 1.0f;
                }
                starVertexBuffer->addStar(relPos, Color(starColor, alpha), discSize);
            }
            else
            {
                if (alpha < 0.0f)
                {
                    alpha = 0.0f;
                }
                else if (alpha > 1.0f)
                {
                    float discScale = min(100.0f, satPoint - appMag + 2.0f);
                    float glareAlpha = min(GlareOpacity, (discScale - 2.0f) / 4.0f);
                    glareVertexBuffer->addStar(relPos, Color(starColor, glareAlpha), 2.0f * discScale * size);
#ifdef DEBUG_HDR_ADAPT
                    maxSize = max(maxSize, 2.0f * discScale * size);
#endif
                }
                starVertexBuffer->addStar(relPos, Color(starColor, alpha), size);
            }

            ++nRendered;
        }
        else
        {
            Matrix3f viewMat = observer->getOrientationf().toRotationMatrix();
            Vector3f viewMatZ = viewMat.row(2);

            RenderListEntry rle;
            rle.renderableType = RenderListEntry::RenderableStar;
            rle.star = &star;

            // Objects in the render list are always rendered relative to
            // a viewer at the origin--this is different than for distant
            // stars.
            float scale = astro::lightYearsToKilometers(1.0f);
            rle.position = relPos * scale;
            rle.centerZ = rle.position.dot(viewMatZ);
            rle.distance = rle.position.norm();
            rle.radius = star.getRadius();
            rle.discSizeInPixels = discSizeInPixels;
            rle.appMag = appMag;
            rle.isOpaque = true;
            renderList->push_back(rle);
        }
    }
}


// Calculate the maximum field of view (from top left corner to bottom right) of
// a frustum with the specified aspect ratio (width/height) and vertical field of
// view. We follow the convention used elsewhere and use units of degrees for
// the field of view angle.
static double calcMaxFOV(double fovY_degrees, double aspectRatio)
{
    double l = 1.0 / tan(degToRad(fovY_degrees / 2.0));
    return radToDeg(atan(sqrt(aspectRatio * aspectRatio + 1.0) / l)) * 2.0;
}


void Renderer::renderPointStars(const StarDatabase& starDB,
                                float faintestMagNight,
                                const Observer& observer)
{
    Vector3d obsPos = observer.getPosition().toLy();

    PointStarRenderer starRenderer;
#ifdef USE_GLCONTEXT
    starRenderer.context           = context;
#endif
    starRenderer.renderer          = this;
    starRenderer.starDB            = &starDB;
    starRenderer.observer          = &observer;
    starRenderer.obsPos            = obsPos;
    starRenderer.viewNormal        = observer.getOrientationf().conjugate() * -Vector3f::UnitZ();
    starRenderer.renderList        = &renderList;
    starRenderer.starVertexBuffer  = pointStarVertexBuffer;
    starRenderer.glareVertexBuffer = glareVertexBuffer;
    starRenderer.fov               = fov;
    starRenderer.cosFOV            = (float) cos(degToRad(calcMaxFOV(fov, getAspectRatio())) / 2.0f);

    starRenderer.pixelSize         = pixelSize;
    starRenderer.brightnessScale   = brightnessScale * corrFac;
    starRenderer.brightnessBias    = brightnessBias;
    starRenderer.faintestMag       = faintestMag;
    starRenderer.faintestMagNight  = faintestMagNight;
    starRenderer.saturationMag     = saturationMag;
#ifdef USE_HDR
    starRenderer.exposure          = exposure + brightPlus;
#endif
    starRenderer.distanceLimit     = distanceLimit;
    starRenderer.labelMode         = labelMode;
    starRenderer.SolarSystemMaxDistance = SolarSystemMaxDistance;
#ifdef DEBUG_HDR_ADAPT
    starRenderer.minMag = -100.f;
    starRenderer.maxMag =  100.f;
    starRenderer.minAlpha = 1.f;
    starRenderer.maxAlpha = 0.f;
    starRenderer.maxSize  = 0.f;
    starRenderer.above    = 1.0f;
    starRenderer.countAboveN = 0L;
    starRenderer.total       = 0L;
#endif

    // = 1.0 at startup
    float effDistanceToScreen = mmToInches((float) REF_DISTANCE_TO_SCREEN) * pixelSize * getScreenDpi();
    starRenderer.labelThresholdMag = 1.2f * max(1.0f, (faintestMag - 4.0f) * (1.0f - 0.5f * (float) log10(effDistanceToScreen)));

    starRenderer.size = BaseStarDiscSize;
    if (starStyle == ScaledDiscStars)
    {
        starRenderer.useScaledDiscs = true;
        starRenderer.brightnessScale *= 2.0f;
        starRenderer.maxDiscSize = starRenderer.size * MaxScaledDiscStarSize;
    }
    else if (starStyle == FuzzyPointStars)
    {
        starRenderer.brightnessScale *= 1.0f;
    }

    starRenderer.colorTemp = colorTemp;

    glEnable(GL_TEXTURE_2D);
    gaussianDiscTex->bind();
    starRenderer.starVertexBuffer->setTexture(gaussianDiscTex);
    starRenderer.glareVertexBuffer->setTexture(gaussianGlareTex);

    starRenderer.glareVertexBuffer->startSprites();
    if (starStyle == PointStars)
        starRenderer.starVertexBuffer->startPoints();
    else
        starRenderer.starVertexBuffer->startSprites();

#ifdef OCTREE_DEBUG
    m_starProcStats.nodes = 0;
    m_starProcStats.height = 0;
    m_starProcStats.objects = 0;
#endif
    starDB.findVisibleStars(starRenderer,
                            obsPos.cast<float>(),
                            observer.getOrientationf(),
                            degToRad(fov),
                            getAspectRatio(),
                            faintestMagNight,
#ifdef OCTREE_DEBUG
                            &m_starProcStats);
#else
                            nullptr);
#endif

    starRenderer.starVertexBuffer->render();
    starRenderer.glareVertexBuffer->render();
    starRenderer.starVertexBuffer->finish();
    starRenderer.glareVertexBuffer->finish();
}


class DSORenderer : public ObjectRenderer<DeepSkyObject*, double>
{
 public:
    DSORenderer();

    void process(DeepSkyObject* const&, double, float);

 public:
    Vector3d     obsPos;
    DSODatabase* dsoDB{ nullptr };
    Frustum      frustum{ degToRad(45.0f), 1.0f, 1.0f };

    Matrix3f orientationMatrix;

    int wWidth{ 0 };
    int wHeight{ 0 };

    double avgAbsMag{ 0.0 };

    unsigned int dsosProcessed{ 0 };
};


DSORenderer::DSORenderer() :
    ObjectRenderer<DeepSkyObject*, double>(DSO_OCTREE_ROOT_SIZE)
{
};


void DSORenderer::process(DeepSkyObject* const & dso,
                          double distanceToDSO,
                          float  absMag)
{
    if (distanceToDSO > distanceLimit)
        return;

    Vector3f relPos = (dso->getPosition() - obsPos).cast<float>();

    Vector3f center = orientationMatrix.transpose() * relPos;

    constexpr const double enhance = 4.0, pc10 = 32.6167;

    // The parameter 'enhance' adjusts the DSO brightness as viewed from "inside"
    // (e.g. MilkyWay as seen from Earth). It provides an enhanced apparent  core
    // brightness appMag  ~ absMag - enhance. 'enhance' thus serves to uniformly
    // enhance the too low sprite luminosity at close distance.

    float  appMag = (distanceToDSO >= pc10)? (float) astro::absToAppMag((double) absMag, distanceToDSO): absMag + (float) (enhance * tanh(distanceToDSO/pc10 - 1.0));

    // Test the object's bounding sphere against the view frustum. If we
    // avoid this stage, overcrowded octree cells may hit performance badly:
    // each object (even if it's not visible) would be sent to the OpenGL
    // pipeline.

    if (dso->isVisible())
    {
        double dsoRadius = dso->getBoundingSphereRadius();
        bool inFrustum = frustum.testSphere(center, (float) dsoRadius) != Frustum::Outside;

        if (inFrustum)
        {
            if ((renderFlags & dso->getRenderMask()))
            {
                dsosProcessed++;

                // Input: display looks satisfactory for 0.2 < brightness < O(1.0)
                // Ansatz: brightness = a - b * appMag(distanceToDSO), emulating eye sensitivity...
                // determine a,b such that
                // a - b * absMag = absMag / avgAbsMag ~ 1; a - b * faintestMag = 0.2.
                // The 2nd eq. guarantees that the faintest galaxies are still visible.

                if(!strcmp(dso->getObjTypeName(),"globular"))
                    avgAbsMag =  -6.86;    // average over 150  globulars in globulars.dsc.
                else if (!strcmp(dso->getObjTypeName(),"galaxy"))
                    avgAbsMag = -19.04;    // average over 10937 galaxies in galaxies.dsc.


                float r = absMag / (float) avgAbsMag;
                float brightness = r - (r - 0.2f) * (absMag - appMag) / (absMag - faintestMag);

                // obviously, brightness(appMag = absMag) = r and
                // brightness(appMag = faintestMag) = 0.2, as desired.

                brightness = 2.3f * brightness * (faintestMag - 4.75f) / renderer->getFaintestAM45deg();

#ifdef USE_HDR
                brightness *= exposure;
#endif
                if (brightness < 0)
                    brightness = 0;

                if (dsoRadius < 1000.0)
                {
                    // Small objects may be prone to clipping; give them special
                    // handling.  We don't want to always set the projection
                    // matrix, since that could be expensive with large galaxy
                    // catalogs.
                    auto nearZ = (float) (distanceToDSO / 2);
                    auto farZ  = (float) (distanceToDSO + dsoRadius * 2 * CubeCornerToCenterDistance);
                    if (nearZ < dsoRadius * 0.001)
                    {
                        nearZ = (float) (dsoRadius * 0.001);
                        farZ  = nearZ * 10000.0f;
                    }

                    glMatrixMode(GL_PROJECTION);
                    glPushMatrix();
                    float t = (float) wWidth / (float) wHeight;
                    glLoadMatrix(Perspective(fov, t, nearZ, farZ));
                    glMatrixMode(GL_MODELVIEW);
                }

                glPushMatrix();
                glTranslate(relPos);

                dso->render(relPos,
                            observer->getOrientationf(),
                            (float) brightness,
                            pixelSize,
                            renderer);
                glPopMatrix();

                if (dsoRadius < 1000.0)
                {
                    glMatrixMode(GL_PROJECTION);
                    glPopMatrix();
                    glMatrixMode(GL_MODELVIEW);
                }
            } // renderFlags check

            // Only render those labels that are in front of the camera:
            // Place labels for DSOs brighter than the specified label threshold brightness
            //
            unsigned int labelMask = dso->getLabelMask();

            if ((labelMask & labelMode) != 0)
            {
                Color labelColor;
                float appMagEff = 6.0f;
                float step = 6.0f;
                float symbolSize = 0.0f;
                MarkerRepresentation* rep = nullptr;

                // Use magnitude based fading for galaxies, and distance based
                // fading for nebulae and open clusters.
                switch (labelMask)
                {
                case Renderer::NebulaLabels:
                    rep = &renderer->nebulaRep;
                    labelColor = Renderer::NebulaLabelColor;
                    appMagEff = astro::absToAppMag(-7.5f, (float) distanceToDSO);
                    symbolSize = (float) (dso->getRadius() / distanceToDSO) / pixelSize;
                    step = 6.0f;
                    break;
                case Renderer::OpenClusterLabels:
                    rep = &renderer->openClusterRep;
                    labelColor = Renderer::OpenClusterLabelColor;
                    appMagEff = astro::absToAppMag(-6.0f, (float) distanceToDSO);
                    symbolSize = (float) (dso->getRadius() / distanceToDSO) / pixelSize;
                    step = 4.0f;
                    break;
                case Renderer::GalaxyLabels:
                    labelColor = Renderer::GalaxyLabelColor;
                    appMagEff = appMag;
                    step = 6.0f;
                    break;
                case Renderer::GlobularLabels:
                    labelColor = Renderer::GlobularLabelColor;
                    appMagEff = appMag;
                    step = 3.0f;
                    break;
                default:
                    // Unrecognized object class
                    labelColor = Color::White;
                    appMagEff = appMag;
                    step = 6.0f;
                    break;
                }

                if (appMagEff < labelThresholdMag)
                {
                    // introduce distance dependent label transparency.
                    float distr = step * (labelThresholdMag - appMagEff) / labelThresholdMag;
                    if (distr > 1.0f)
                        distr = 1.0f;

                    renderer->addBackgroundAnnotation(rep,
                                                      dsoDB->getDSOName(dso, true),
                                                      Color(labelColor, distr * labelColor.alpha()),
                                                      relPos,
                                                      Renderer::AlignLeft, Renderer::VerticalAlignCenter, symbolSize);
                }
            } // labels enabled
        } // in frustum
    } // isVisible()
}


void Renderer::renderDeepSkyObjects(const Universe&  universe,
                                    const Observer& observer,
                                    const float     faintestMagNight)
{
    DSORenderer dsoRenderer;

    Vector3d obsPos     = observer.getPosition().toLy();

    DSODatabase* dsoDB  = universe.getDSOCatalog();

#ifdef USE_GLCONTEXT
    dsoRenderer.context          = context;
#endif
    dsoRenderer.renderer         = this;
    dsoRenderer.dsoDB            = dsoDB;
    dsoRenderer.orientationMatrix= observer.getOrientationf().conjugate().toRotationMatrix();
    dsoRenderer.observer         = &observer;
    dsoRenderer.obsPos           = obsPos;
    dsoRenderer.viewNormal       = observer.getOrientationf().conjugate() * -Vector3f::UnitZ();
    dsoRenderer.fov              = fov;
    // size/pixelSize =0.86 at 120deg, 1.43 at 45deg and 1.6 at 0deg.
    dsoRenderer.size             = pixelSize * 1.6f / corrFac;
    dsoRenderer.pixelSize        = pixelSize;
    dsoRenderer.brightnessScale  = brightnessScale * corrFac;
    dsoRenderer.brightnessBias   = brightnessBias;
    dsoRenderer.avgAbsMag        = dsoDB->getAverageAbsoluteMagnitude();
    dsoRenderer.faintestMag      = faintestMag;
    dsoRenderer.faintestMagNight = faintestMagNight;
    dsoRenderer.saturationMag    = saturationMag;
#ifdef USE_HDR
    dsoRenderer.exposure         = exposure + brightPlus;
#endif
    dsoRenderer.renderFlags      = renderFlags;
    dsoRenderer.labelMode        = labelMode;
    dsoRenderer.wWidth           = windowWidth;
    dsoRenderer.wHeight          = windowHeight;

    dsoRenderer.frustum = Frustum(degToRad(fov),
                                  getAspectRatio(),
                                  MinNearPlaneDistance);
    // Use pixelSize * screenDpi instead of FoV, to eliminate windowHeight dependence.
    // = 1.0 at startup
    float effDistanceToScreen = mmToInches((float) REF_DISTANCE_TO_SCREEN) * pixelSize * getScreenDpi();

    dsoRenderer.labelThresholdMag = 2.0f * max(1.0f, (faintestMag - 4.0f) * (1.0f - 0.5f * (float) log10(effDistanceToScreen)));

    galaxyRep      = MarkerRepresentation(MarkerRepresentation::Triangle, 8.0f, GalaxyLabelColor);
    nebulaRep      = MarkerRepresentation(MarkerRepresentation::Square,   8.0f, NebulaLabelColor);
    openClusterRep = MarkerRepresentation(MarkerRepresentation::Circle,   8.0f, OpenClusterLabelColor);
    globularRep    = MarkerRepresentation(MarkerRepresentation::Circle,   8.0f, OpenClusterLabelColor);

    // Render any line primitives with smooth lines
    // (mostly to make graticules look good.)
    enableSmoothLines(renderFlags);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

#ifdef OCTREE_DEBUG
    m_dsoProcStats.objects = 0;
    m_dsoProcStats.nodes = 0;
    m_dsoProcStats.height = 0;
#endif
    dsoDB->findVisibleDSOs(dsoRenderer,
                           obsPos,
                           observer.getOrientationf(),
                           degToRad(fov),
                           getAspectRatio(),
                           2 * faintestMagNight,
#ifdef OCTREE_DEBUG
                           &m_dsoProcStats);
#else
                            nullptr);
#endif

    // clog << "DSOs processed: " << dsoRenderer.dsosProcessed << endl;

    disableSmoothLines(renderFlags);
}


static Vector3d toStandardCoords(const Vector3d& v)
{
    return Vector3d(v.x(), -v.z(), v.y());
}


void Renderer::renderSkyGrids(const Observer& observer)
{
    if ((renderFlags & ShowCelestialSphere) != 0)
    {
        SkyGrid grid;
        grid.setOrientation(Quaterniond(AngleAxis<double>(astro::J2000Obliquity, Vector3d::UnitX())));
        grid.setLineColor(EquatorialGridColor);
        grid.setLabelColor(EquatorialGridLabelColor);
        grid.render(*this, observer, windowWidth, windowHeight);
    }

    if ((renderFlags & ShowGalacticGrid) != 0)
    {
        SkyGrid galacticGrid;
        galacticGrid.setOrientation((astro::eclipticToEquatorial() * astro::equatorialToGalactic()).conjugate());
        galacticGrid.setLineColor(GalacticGridColor);
        galacticGrid.setLabelColor(GalacticGridLabelColor);
        galacticGrid.setLongitudeUnits(SkyGrid::LongitudeDegrees);
        galacticGrid.render(*this, observer, windowWidth, windowHeight);
    }

    if ((renderFlags & ShowEclipticGrid) != 0)
    {
        SkyGrid grid;
        grid.setOrientation(Quaterniond::Identity());
        grid.setLineColor(EclipticGridColor);
        grid.setLabelColor(EclipticGridLabelColor);
        grid.setLongitudeUnits(SkyGrid::LongitudeDegrees);
        grid.render(*this, observer, windowWidth, windowHeight);
    }

    if ((renderFlags & ShowHorizonGrid) != 0)
    {
        double tdb = observer.getTime();
        auto frame = observer.getFrame();
        Body* body = frame->getRefObject().body();

        if (body != nullptr)
        {
            SkyGrid grid;
            grid.setLineColor(HorizonGridColor);
            grid.setLabelColor(HorizonGridLabelColor);
            grid.setLongitudeUnits(SkyGrid::LongitudeDegrees);
            grid.setLongitudeDirection(SkyGrid::IncreasingClockwise);

            Vector3d zenithDirection = observer.getPosition().offsetFromKm(body->getPosition(tdb)).normalized();

            Vector3d northPole = body->getEclipticToEquatorial(tdb).conjugate() * Vector3d::UnitY();
            zenithDirection = toStandardCoords(zenithDirection);
            northPole = toStandardCoords(northPole);

            Vector3d v = zenithDirection.cross(northPole);

            // Horizontal coordinate system not well defined when observer
            // is at a pole.
            double tolerance = 1.0e-10;
            if (v.norm() > tolerance && v.norm() < 1.0 - tolerance)
            {
                v.normalize();
                Vector3d u = v.cross(zenithDirection);

                Matrix3d m;
                m.row(0) = u;
                m.row(1) = v;
                m.row(2) = zenithDirection;
                grid.setOrientation(Quaterniond(m));

                grid.render(*this, observer, windowWidth, windowHeight);
            }
        }
    }

    renderEclipticLine();
}

void Renderer::labelConstellations(const AsterismList& asterisms,
                                   const Observer& observer)
{
    Vector3f observerPos = observer.getPosition().toLy().cast<float>();

    for (auto ast : asterisms)
    {
        if (ast->getChainCount() > 0 && ast->getActive())
        {
            const Asterism::Chain& chain = ast->getChain(0);

            if (!chain.empty())
            {
                // The constellation label is positioned at the average
                // position of all stars in the first chain.  This usually
                // gives reasonable results.
                Vector3f avg = Vector3f::Zero();
                // XXX: std::reduce
                for (const auto& c : chain)
                    avg += c;

                avg = avg / (float) chain.size();

                // Draw all constellation labels at the same distance
                avg.normalize();
                avg = avg * 1.0e4f;

                Vector3f rpos = avg - observerPos;

                if ((observer.getOrientationf() * rpos).z() < 0)
                {
                    // We'll linearly fade the labels as a function of the
                    // observer's distance to the origin of coordinates:
                    float opacity = 1.0f;
                    float dist = observerPos.norm();
                    if (dist > MaxAsterismLabelsConstDist)
                    {
                        opacity = clamp((MaxAsterismLabelsConstDist - dist) /
                                        (MaxAsterismLabelsDist - MaxAsterismLabelsConstDist) + 1);
                    }

                    // Use the default label color unless the constellation has an
                    // override color set.
                    Color labelColor = ConstellationLabelColor;
                    if (ast->isColorOverridden())
                        labelColor = ast->getOverrideColor();

                    addBackgroundAnnotation(nullptr,
                                            ast->getName((labelMode & I18nConstellationLabels) != 0),
                                            Color(labelColor, opacity),
                                            rpos,
                                            AlignCenter, VerticalAlignCenter);
                }
            }
        }
    }
}


void Renderer::renderParticles(const vector<Particle>& particles,
                               const Quaternionf& /*orientation*/)
{
    ShaderProperties shaderprop;
    shaderprop.lightModel = ShaderProperties::ParticleModel;
    shaderprop.texUsage = ShaderProperties::PointSprite;
    auto *prog = shaderManager->getShader(shaderprop);
    if (prog == nullptr)
        return;
    prog->use();

    glEnable(GL_POINT_SPRITE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(Particle), &particles[0].center);
    glEnableVertexAttribArray(CelestiaGLProgram::PointSizeAttributeIndex);
    glVertexAttribPointer(CelestiaGLProgram::PointSizeAttributeIndex,
                          1, GL_FLOAT, GL_FALSE,
                          sizeof(Particle), &particles[0].size);
    glDrawArrays(GL_POINTS, 0, particles.size());

    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableVertexAttribArray(CelestiaGLProgram::PointSizeAttributeIndex);
    glUseProgram(0);
    glDisable(GL_POINT_SPRITE);
}

void Renderer::renderAnnotations(const vector<Annotation>& annotations, FontStyle fs)
{
    if (font[fs] == nullptr)
        return;

    // Enable line smoothing for rendering symbols
    enableSmoothLines(renderFlags);

#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
#endif
    glEnable(GL_TEXTURE_2D);
    font[fs]->bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrix(Ortho2D(0.0f, (float)windowWidth, 0.0f, (float)windowHeight));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    for (int i = 0; i < (int) annotations.size(); i++)
    {
        if (annotations[i].markerRep != nullptr)
        {
            glPushMatrix();
            const MarkerRepresentation& markerRep = *annotations[i].markerRep;

            float size = markerRep.size();
            if (annotations[i].size > 0.0f)
            {
                size = annotations[i].size;
            }

            glColor(annotations[i].color);
            glTranslatef((GLfloat) (int) annotations[i].position.x(),
                         (GLfloat) (int) annotations[i].position.y(), 0.0f);

            if (markerRep.symbol() == MarkerRepresentation::Crosshair)
                renderCrosshair(size, realTime, annotations[i].color);
            else
                markerRep.render(*this, size);

            if (!markerRep.label().empty())
            {
                int labelOffset = (int) markerRep.size() / 2;
                glTranslatef(labelOffset + PixelOffset, -labelOffset - font[fs]->getHeight() + PixelOffset, 0.0f);
                font[fs]->render(markerRep.label(), 0.0f, 0.0f);
            }
            glPopMatrix();
        }

        if (!annotations[i].labelText.empty())
        {
            glPushMatrix();
            int labelWidth = 0;
            int hOffset = 2;
            int vOffset = 0;

            switch (annotations[i].halign)
            {
            case AlignCenter:
                labelWidth = (font[fs]->getWidth(annotations[i].labelText));
                hOffset = -labelWidth / 2;
                break;

            case AlignRight:
                labelWidth = (font[fs]->getWidth(annotations[i].labelText));
                hOffset = -(labelWidth + 2);
                break;

            case AlignLeft:
                if (annotations[i].markerRep != nullptr)
                    hOffset = 2 + (int) annotations[i].markerRep->size() / 2;
                break;
            }

            switch (annotations[i].valign)
            {
            case VerticalAlignCenter:
                vOffset = -font[fs]->getHeight() / 2;
                break;
            case VerticalAlignTop:
                vOffset = -font[fs]->getHeight();
                break;
            case VerticalAlignBottom:
                vOffset = 0;
                break;
            }

            glColor(annotations[i].color);
            glTranslatef((int) annotations[i].position.x() + hOffset + PixelOffset,
                         (int) annotations[i].position.y() + vOffset + PixelOffset, 0.0f);
            // EK TODO: Check where to replace (see '_(' above)
            font[fs]->render(annotations[i].labelText, 0.0f, 0.0f);
            glPopMatrix();
        }
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
#ifdef USE_HDR
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
#endif

    disableSmoothLines(renderFlags);
}


void
Renderer::renderBackgroundAnnotations(FontStyle fs)
{
    glEnable(GL_DEPTH_TEST);
    renderAnnotations(backgroundAnnotations, fs);
    glDisable(GL_DEPTH_TEST);

    clearAnnotations(backgroundAnnotations);
}


void
Renderer::renderForegroundAnnotations(FontStyle fs)
{
    glDisable(GL_DEPTH_TEST);
    renderAnnotations(foregroundAnnotations, fs);

    clearAnnotations(foregroundAnnotations);
}


vector<Renderer::Annotation>::iterator
Renderer::renderSortedAnnotations(vector<Annotation>::iterator iter,
                                  float nearDist,
                                  float farDist,
                                  FontStyle fs)
{
    if (font[fs] == nullptr)
        return iter;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    font[fs]->bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrix(Ortho2D(0.0f, (float)windowWidth, 0.0f, (float)windowHeight));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Precompute values that will be used to generate the normalized device z value;
    // we're effectively just handling the projection instead of OpenGL. We use an orthographic
    // projection matrix in order to get the label text position exactly right but need to mimic
    // the depth coordinate generation of a perspective projection.
    float d1 = -(farDist + nearDist) / (farDist - nearDist);
    float d2 = -2.0f * nearDist * farDist / (farDist - nearDist);

    for (; iter != depthSortedAnnotations.end() && iter->position.z() > nearDist; iter++)
    {
        // Compute normalized device z
        float ndc_z = d1 + d2 / -iter->position.z();
        ndc_z = min(1.0f, max(-1.0f, ndc_z)); // Clamp to [-1,1]

        // Offsets to left align label
        int labelHOffset = 0;
        int labelVOffset = 0;

        glPushMatrix();
        if (iter->markerRep != nullptr)
        {
            const MarkerRepresentation& markerRep = *iter->markerRep;
            float size = markerRep.size();
            if (iter->size > 0.0f)
            {
                size = iter->size;
            }

            glTranslatef((GLfloat) (int) iter->position.x(), (GLfloat) (int) iter->position.y(), ndc_z);
            glColor(iter->color);

            if (markerRep.symbol() == MarkerRepresentation::Crosshair)
                renderCrosshair(size, realTime, iter->color);
            else
                markerRep.render(*this, size);

            if (!markerRep.label().empty())
            {
                int labelOffset = (int) markerRep.size() / 2;
                glTranslatef(labelOffset + PixelOffset, -labelOffset - font[fs]->getHeight() + PixelOffset, 0.0f);
                font[fs]->render(markerRep.label(), 0.0f, 0.0f);
            }
        }
        else
        {
            glTranslatef((int) iter->position.x() + PixelOffset + labelHOffset,
                         (int) iter->position.y() + PixelOffset + labelVOffset,
                         ndc_z);
            glColor(iter->color);
            font[fs]->render(iter->labelText, 0.0f, 0.0f);
        }
        glPopMatrix();
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_DEPTH_TEST);

    return iter;
}


vector<Renderer::Annotation>::iterator
Renderer::renderAnnotations(vector<Annotation>::iterator startIter,
                            vector<Annotation>::iterator endIter,
                            float nearDist,
                            float farDist,
                            FontStyle fs)
{
    if (font[fs] == nullptr)
        return endIter;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    font[fs]->bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrix(Ortho2D(0.0f, (float)windowWidth, 0.0f, (float)windowHeight));
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Precompute values that will be used to generate the normalized device z value;
    // we're effectively just handling the projection instead of OpenGL. We use an orthographic
    // projection matrix in order to get the label text position exactly right but need to mimic
    // the depth coordinate generation of a perspective projection.
    float d1 = -(farDist + nearDist) / (farDist - nearDist);
    float d2 = -2.0f * nearDist * farDist / (farDist - nearDist);

    vector<Annotation>::iterator iter = startIter;
    for (; iter != endIter && iter->position.z() > nearDist; iter++)
    {
        // Compute normalized device z
        float ndc_z = d1 + d2 / -iter->position.z();
        ndc_z = min(1.0f, max(-1.0f, ndc_z)); // Clamp to [-1,1]

        // Offsets to left align label
        int labelHOffset = 0;
        int labelVOffset = 0;

        if (iter->markerRep != nullptr)
        {
            glPushMatrix();
            const MarkerRepresentation& markerRep = *iter->markerRep;
            float size = markerRep.size();
            if (iter->size > 0.0f)
            {
                size = iter->size;
            }

            glTranslatef((GLfloat) (int) iter->position.x(), (GLfloat) (int) iter->position.y(), ndc_z);
            glColor(iter->color);

            if (markerRep.symbol() == MarkerRepresentation::Crosshair)
                renderCrosshair(size, realTime, iter->color);
            else
                markerRep.render(*this, size);

            if (!markerRep.label().empty())
            {
                int labelOffset = (int) markerRep.size() / 2;
                glTranslatef(labelOffset + PixelOffset, -labelOffset - font[fs]->getHeight() + PixelOffset, 0.0f);
                font[fs]->render(markerRep.label(), 0.0f, 0.0f);
            }
            glPopMatrix();
        }

        if (!iter->labelText.empty())
        {
            if (iter->markerRep != nullptr)
                labelHOffset += (int) iter->markerRep->size() / 2 + 3;

            glPushMatrix();
            glTranslatef((int) iter->position.x() + PixelOffset + labelHOffset,
                         (int) iter->position.y() + PixelOffset + labelVOffset,
                         ndc_z);
            glColor(iter->color);
            font[fs]->render(iter->labelText, 0.0f, 0.0f);
            glPopMatrix();
        }
    }

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_DEPTH_TEST);

    return iter;
}


void Renderer::renderMarkers(const MarkerList& markers,
                             const UniversalCoord& cameraPosition,
                             const Quaterniond& cameraOrientation,
                             double jd)
{
    // Calculate the cosine of half the maximum field of view. We'll use this for
    // fast testing of marker visibility. The stored field of view is the
    // vertical field of view; we want the field of view as measured on the
    // diagonal between viewport corners.
    double h = tan(degToRad(fov / 2));
    double diag = sqrt(1.0 + square(h) + square(h * (double) getAspectRatio()));
    double cosFOV = 1.0 / diag;

    Vector3d viewVector = cameraOrientation.conjugate() * -Vector3d::UnitZ();

    for (const auto& marker : markers)
    {
        Vector3d offset = marker.position(jd).offsetFromKm(cameraPosition);

        // Only render those markers that lie withing the field of view.
        if ((offset.dot(viewVector)) > cosFOV * offset.norm())
        {
            double distance = offset.norm();
            float symbolSize = 0.0f;
            if (marker.sizing() == DistanceBasedSize)
            {
                symbolSize = (float) (marker.representation().size() / distance) / pixelSize;
            }

            if (marker.occludable())
            {
                // If the marker is occludable, add it to the sorted annotation list if it's relatively
                // nearby, and to the background list if it's very distant.
                if (distance < astro::lightYearsToKilometers(1.0))
                {
                    // Modify the marker position so that it is always in front of the marked object.
                    double boundingRadius;
                    if (marker.object().body() != nullptr)
                        boundingRadius = marker.object().body()->getBoundingRadius();
                    else
                        boundingRadius = marker.object().radius();
                    offset *= (1.0 - boundingRadius * 1.01 / distance);

                    addSortedAnnotation(&(marker.representation()), "", marker.representation().color(),
                                        offset.cast<float>(),
                                        AlignLeft, VerticalAlignTop, symbolSize);
                }
                else
                {
                    addAnnotation(backgroundAnnotations,
                                  &(marker.representation()), "", marker.representation().color(),
                                  offset.cast<float>(),
                                  AlignLeft, VerticalAlignTop, symbolSize);
                }
            }
            else
            {
                addAnnotation(foregroundAnnotations,
                              &(marker.representation()), "", marker.representation().color(),
                              offset.cast<float>(),
                              AlignLeft, VerticalAlignTop, symbolSize);
            }
        }
    }
}


void Renderer::setStarStyle(StarStyle style)
{
    starStyle = style;
    markSettingsChanged();
}


Renderer::StarStyle Renderer::getStarStyle() const
{
    return starStyle;
}


void Renderer::loadTextures(Body* body)
{
    Surface& surface = body->getSurface();

    if (surface.baseTexture.tex[textureResolution] != InvalidResource)
        surface.baseTexture.find(textureResolution);
    if ((surface.appearanceFlags & Surface::ApplyBumpMap) != 0 &&
        surface.bumpTexture.tex[textureResolution] != InvalidResource)
        surface.bumpTexture.find(textureResolution);
    if ((surface.appearanceFlags & Surface::ApplyNightMap) != 0 &&
        (renderFlags & ShowNightMaps) != 0)
        surface.nightTexture.find(textureResolution);
    if ((surface.appearanceFlags & Surface::SeparateSpecularMap) != 0 &&
        surface.specularTexture.tex[textureResolution] != InvalidResource)
        surface.specularTexture.find(textureResolution);

    if ((renderFlags & ShowCloudMaps) != 0 &&
        body->getAtmosphere() != nullptr &&
        body->getAtmosphere()->cloudTexture.tex[textureResolution] != InvalidResource)
    {
        body->getAtmosphere()->cloudTexture.find(textureResolution);
    }

    if (body->getRings() != nullptr &&
        body->getRings()->texture.tex[textureResolution] != InvalidResource)
    {
        body->getRings()->texture.find(textureResolution);
    }

    if (body->getGeometry() != InvalidResource)
    {
        Geometry* geometry = GetGeometryManager()->find(body->getGeometry());
        if (geometry != nullptr)
        {
            geometry->loadTextures();
        }
    }
}


void Renderer::invalidateOrbitCache()
{
    orbitCache.clear();
}


bool Renderer::settingsHaveChanged() const
{
    return settingsChanged;
}


void Renderer::markSettingsChanged()
{
    settingsChanged = true;
    notifyWatchers();
}


void Renderer::addWatcher(RendererWatcher* watcher)
{
    assert(watcher != nullptr);
    watchers.push_back(watcher);
}


void Renderer::removeWatcher(RendererWatcher* watcher)
{
    auto iter = find(watchers.begin(), watchers.end(), watcher);
    if (iter != watchers.end())
        watchers.erase(iter);
}


void Renderer::notifyWatchers() const
{
    for (const auto watcher : watchers)
    {
        watcher->notifyRenderSettingsChanged(this);
    }
}

void Renderer::updateBodyVisibilityMask()
{
    // Bodies with type `Invisible' (e.g. ReferencePoints) are not drawn,
    // but if their property `Visible' is set they have visible labels,
    // so we make `Body::Invisible' class visible.
    int flags = Body::Invisible;

    if ((renderFlags & Renderer::ShowPlanets) != 0)
        flags |= Body::Planet;
    if ((renderFlags & Renderer::ShowDwarfPlanets) != 0)
        flags |= Body::DwarfPlanet;
    if ((renderFlags & Renderer::ShowMoons) != 0)
        flags |= Body::Moon;
    if ((renderFlags & Renderer::ShowMinorMoons) != 0)
        flags |= Body::MinorMoon;
    if ((renderFlags & Renderer::ShowAsteroids) != 0)
        flags |= Body::Asteroid;
    if ((renderFlags & Renderer::ShowComets) != 0)
        flags |= Body::Comet;
    if ((renderFlags & Renderer::ShowSpacecrafts) != 0)
        flags |= Body::Spacecraft;

    bodyVisibilityMask = flags;
}

void Renderer::setSolarSystemMaxDistance(float t)
{
    SolarSystemMaxDistance = clamp(t, 1.0f, 10.0f);
}

void Renderer::getViewport(int* x, int* y, int* w, int* h) const
{
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (x != nullptr)
        *x = viewport[0];
    if (y != nullptr)
        *y = viewport[1];
    if (w != nullptr)
        *w = viewport[2];
    if (h != nullptr)
        *h = viewport[3];
}

void Renderer::getViewport(std::array<int, 4>& viewport) const
{
    static_assert(sizeof(int) == sizeof(GLint), "int and GLint size mismatch");
    glGetIntegerv(GL_VIEWPORT, &viewport[0]);
}

void Renderer::setViewport(int x, int y, int w, int h) const
{
    glViewport(x, y, w, h);
}

void Renderer::setViewport(const std::array<int, 4>& viewport) const
{
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

void Renderer::setScissor(int x, int y, int w, int h)
{
    if ((m_GLStateFlag & ScissorTest) == 0)
    {
        glEnable(GL_SCISSOR_TEST);
        m_GLStateFlag |= ScissorTest;
    }
    glScissor(x, y, w, h);
}

void Renderer::removeScissor()
{
    if ((m_GLStateFlag & ScissorTest) != 0)
    {
        glDisable(GL_SCISSOR_TEST);
        m_GLStateFlag &= ~ScissorTest;
    }
}

void Renderer::enableMSAA()
{
    if ((m_GLStateFlag & Multisaple) == 0)
    {
        glEnable(GL_MULTISAMPLE);
        m_GLStateFlag |= Multisaple;
    }
}
void Renderer::disableMSAA()
{
    if ((m_GLStateFlag & Multisaple) != 0)
    {
        glDisable(GL_MULTISAMPLE);
        m_GLStateFlag &= ~Multisaple;
    }
}

bool Renderer::isMSAAEnabled() const
{
    return (m_GLStateFlag & Multisaple) != 0;;
}

constexpr GLenum toGLFormat(Renderer::PixelFormat format)
{
    return (GLenum) format;
}

bool Renderer::captureFrame(int x, int y, int w, int h, Renderer::PixelFormat format, unsigned char* buffer, bool back) const
{
    glReadBuffer(back ? GL_BACK : GL_FRONT);
    glReadPixels(x, y, w, h, toGLFormat(format), GL_UNSIGNED_BYTE, (void*) buffer);

    return glGetError() == GL_NO_ERROR;
}

static void drawRectangle(const Renderer &renderer, const Rect &r)
{
    uint32_t p = r.tex == nullptr ? 0 : ShaderProperties::HasTexture;
    switch (r.nColors)
    {
    case 0:
        break;
    case 1:
        p |= ShaderProperties::UniformColor;
        break;
    case 4:
        p |= ShaderProperties::PerVertexColor;
        break;
    default:
        fmt::fprintf(cerr, "Incorrect number of colors: %i\n", r.nColors);
    }

    auto prog = renderer.getShaderManager().getShader(ShaderProperties(p));
    if (prog == nullptr)
        return;

    constexpr array<short, 8> texels = {0, 1,  1, 1,  1, 0,  0, 0};
    array<float, 8> vertices = { r.x, r.y,  r.x+r.w, r.y, r.x+r.w, r.y+r.h, r.x, r.y+r.h };

    auto s = static_cast<GLsizeiptr>(memsize(vertices) + memsize(texels) + 4*4*sizeof(GLfloat));
    static celgl::VertexObject vo{ GL_ARRAY_BUFFER, s, GL_DYNAMIC_DRAW };
    vo.bindWritable();

    if (!vo.initialized())
    {
        vo.allocate();
        vo.setBufferData(texels.data(), memsize(vertices), memsize(texels));
        vo.setVertices(2, GL_FLOAT);
        vo.setTextureCoords(2, GL_SHORT, false, 0, memsize(vertices));
        vo.setColors(4, GL_FLOAT, false, 0, memsize(vertices) + memsize(texels));
    }

    vo.setBufferData(vertices.data(), 0, memsize(vertices));
    if (r.nColors == 4)
    {
        array<Vector4f, 4> ct;
        for (size_t i = 0; i < 4; i++)
            ct[i] = r.colors[i].toVector4();
        vo.setBufferData(ct.data(), memsize(vertices) + memsize(texels), 4*4*sizeof(GLfloat));
    }

    prog->use();
    if (r.tex != nullptr)
    {
        glEnable(GL_TEXTURE_2D);
        r.tex->bind();
        prog->samplerParam("tex") = 0;
    }

    if (r.nColors == 1)
        prog->vec4Param("color") = r.colors[0].toVector4();

    if (r.type != Rect::Type::BorderOnly)
    {
        vo.draw(GL_TRIANGLE_FAN, 4);
    }
    else
    {
        if (r.lw != 1.0f)
            glLineWidth(r.lw);
        vo.draw(GL_LINE_LOOP, 4);
        if (r.lw != 1.0f)
            glLineWidth(1.0f);
    }

    glUseProgram(0);
    vo.unbind();
}

void Renderer::drawRectangle(const Rect &r) const
{
    ::drawRectangle(*this, r);
}

void Renderer::setRenderRegion(int x, int y, int width, int height, bool withScissor)
{
    if (withScissor)
        setScissor(x, y, width, height);
    else
        removeScissor();

    setViewport(x, y, width, height);
    resize(width, height);
}

float Renderer::getAspectRatio() const
{
    return static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
}
