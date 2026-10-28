// ---------------------------------------------------------------------------
//  Black hole simulation - 3D GPU ray tracer
//
//  The heavy lifting (null-geodesic integration, disk shading, lensed sky)
//  happens in geodesic.comp.  This file owns the window, camera, the toy
//  Newtonian orbit integrator for the stars, the Flamm-paraboloid spacetime
//  grid, and the uniform buffers that feed the compute shader.
//
//  Controls
//    left drag        orbit camera          mouse wheel      zoom
//    G                pause / resume orbits R                reset camera
//    Shift+R          reset orbits          D                toggle accretion disk
//    S                toggle starfield      B                toggle Doppler / redshift
//    T                toggle disk texture   W                toggle spacetime grid
//    + / -            time scale x2 / /2    [ / ]            exposure
//    A                auto resolution       H                print help
//    F11              fullscreen            F12              screenshot
//    I                app icons             O                auto-orbit camera
//    Esc              quit
//
//  Screensaver: the same source builds BlackHoleSaver.scr, which understands
//  the standard /s (run), /c (configure) and /p (preview) arguments, harvests
//  the icons of installed apps and lets them spiral into the hole.
//
//  This is purely a computer simulation.  No actual black holes are produced,
//  harmed, or operated by running this program.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <random>
#include <string>
#include <vector>

#include "icon_harvest.h"
#include "shaders_embedded.h"

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif
#ifndef BH_SOURCE_DIR
#  define BH_SOURCE_DIR "."
#endif

using glm::vec3;
using glm::vec4;
using glm::dvec3;
using glm::mat4;
using std::cout;
using std::cerr;
using std::endl;
using std::vector;
using std::string;

// --------------------------------------------------------------- constants
static const double c_light = 299792458.0;        // m/s
static const double G_const = 6.67430e-11;        // m^3 kg^-1 s^-2

struct BlackHole {
    dvec3  position;
    double mass;      // kg
    double r_s;       // Schwarzschild radius, m

    BlackHole(dvec3 pos, double m) : position(pos), mass(m) {
        r_s = 2.0 * G_const * mass / (c_light * c_light);
    }
};
static BlackHole SagA(dvec3(0.0), 8.54e36);        // Sagittarius A*  (~4.3 million solar masses)

// --------------------------------------------------------------- simulation state
struct SimState {
    bool   orbitsRunning = true;
    bool   showDisk      = true;
    bool   showStars     = true;
    bool   doppler       = true;
    bool   diskTexture   = true;
    bool   showGrid      = true;
    double timeScale     = 600.0;    // simulated seconds per real second (1 s -> 10 min)
    double simTime       = 0.0;      // simulated seconds elapsed
    float  exposure      = 1.0f;
    // dynamic resolution: the ray tracer renders at renderScale * window size and is
    // upscaled; with autoScale on, the scale adapts to hold ~30 fps on slower GPUs.
    float  renderScale   = 1.0f;
    bool   autoScale     = true;
    double targetFps     = 30.0;
    bool   showIcons     = false;    // app-icon sprites spiralling in
    bool   autoOrbit     = false;    // slow automatic camera motion
    bool   saverMode     = false;    // launched as a screensaver: exit on any input
};
static SimState sim;

// --------------------------------------------------------------- camera
struct Camera {
    vec3  target = vec3(0.0f);
    float radius;
    float minRadius, maxRadius;
    float azimuth, elevation;
    float orbitSpeed = 0.005f;
    float zoomFactor = 1.12f;
    bool  dragging = false;
    double lastX = 0.0, lastY = 0.0;

    Camera() { reset(); }

    void reset() {
        float rs = float(SagA.r_s);
        radius    = 12.0f * rs;
        minRadius = 1.6f * rs;          // keep the camera outside the photon sphere
        maxRadius = 90.0f * rs;
        azimuth   = -0.35f;
        elevation = 1.32f;              // ~75 deg from the disk normal: slightly above the disk
    }
    vec3 position() const {
        float el = glm::clamp(elevation, 0.02f, float(M_PI) - 0.02f);
        return vec3(radius * std::sin(el) * std::cos(azimuth),
                    radius * std::cos(el),
                    radius * std::sin(el) * std::sin(azimuth));
    }
    void processMouseMove(double x, double y) {
        if (dragging) {
            azimuth   += float(x - lastX) * orbitSpeed;
            elevation -= float(y - lastY) * orbitSpeed;
            elevation  = glm::clamp(elevation, 0.02f, float(M_PI) - 0.02f);
        }
        lastX = x; lastY = y;
    }
    void processMouseButton(int button, int action, int /*mods*/, GLFWwindow* win) {
        if (button == GLFW_MOUSE_BUTTON_LEFT || button == GLFW_MOUSE_BUTTON_MIDDLE) {
            if (action == GLFW_PRESS) {
                dragging = true;
                glfwGetCursorPos(win, &lastX, &lastY);
            } else if (action == GLFW_RELEASE) {
                dragging = false;
            }
        }
    }
    void processScroll(double /*xoffset*/, double yoffset) {
        if (yoffset > 0) radius /= float(std::pow(zoomFactor, yoffset));
        else             radius *= float(std::pow(zoomFactor, -yoffset));
        radius = glm::clamp(radius, minRadius, maxRadius);
    }
};
static Camera camera;

// --------------------------------------------------------------- scene objects
struct ObjectData {
    dvec3  pos;            // metres (double precision for the orbit integrator)
    dvec3  vel;            // m/s
    double radius;         // metres
    double mass;           // kg
    vec4   color;          // rgb + emissive strength in .a
    bool   isBlackHole = false;
    dvec3  pos0, vel0;     // initial conditions, for reset
};
static vector<ObjectData> objects;

static dvec3 circularVelocity(const dvec3& pos, const dvec3& axis) {
    double r = glm::length(pos);
    double v = std::sqrt(G_const * SagA.mass / r);
    dvec3 t = glm::normalize(glm::cross(glm::normalize(axis), pos / r));
    return t * v;
}
static void setupObjects() {
    objects.clear();
    const double Msun = 1.98892e30, Rstar = 3.2e10;
    ObjectData a;   // yellow star, orbit in the disk plane
    a.pos = dvec3(2.6e11, 0.0, 0.0); a.vel = circularVelocity(a.pos, dvec3(0, 1, 0));
    a.radius = Rstar; a.mass = Msun; a.color = vec4(1.0f, 0.93f, 0.55f, 1.0f);
    ObjectData b;   // red star, inclined orbit
    b.pos = dvec3(0.0, 1.0e11, -2.3e11); b.vel = circularVelocity(b.pos, dvec3(0.35, 1.0, 0.0));
    b.radius = Rstar * 0.8; b.mass = Msun * 0.6; b.color = vec4(1.0f, 0.35f, 0.15f, 1.0f);
    ObjectData bh;  // the black hole itself (used by the grid; the shader handles the horizon)
    bh.pos = SagA.position; bh.vel = dvec3(0.0);
    bh.radius = SagA.r_s; bh.mass = SagA.mass; bh.color = vec4(0.0f); bh.isBlackHole = true;
    objects = { a, b, bh };
    for (auto& o : objects) { o.pos0 = o.pos; o.vel0 = o.vel; }
}
static void resetObjects() {
    for (auto& o : objects) { o.pos = o.pos0; o.vel = o.vel0; }
    sim.simTime = 0.0;
}
// Leapfrog (kick-drift-kick) Newtonian N-body step.  The black hole is pinned.
static void stepOrbits(double dt) {
    auto accel = [](size_t i) {
        dvec3 acc(0.0);
        for (size_t j = 0; j < objects.size(); ++j) {
            if (i == j) continue;
            dvec3 d = objects[j].pos - objects[i].pos;
            double r2 = glm::dot(d, d);
            double r  = std::sqrt(r2);
            if (r < objects[j].radius) continue;        // inside the body: ignore
            acc += d * (G_const * objects[j].mass / (r2 * r));
        }
        return acc;
    };
    const int substeps = 4;
    double h = dt / substeps;
    for (int s = 0; s < substeps; ++s) {
        for (size_t i = 0; i < objects.size(); ++i)
            if (!objects[i].isBlackHole) objects[i].vel += accel(i) * (0.5 * h);
        for (auto& o : objects)
            if (!o.isBlackHole) o.pos += o.vel * h;
        for (size_t i = 0; i < objects.size(); ++i)
            if (!objects[i].isBlackHole) objects[i].vel += accel(i) * (0.5 * h);
    }
}

// --------------------------------------------------------------- app-icon sprites
// Each icon is a massless test particle on a decaying orbit in the
// Paczynski-Wiita pseudo-Newtonian potential  a = -GM / (r - r_s)^2, which
// reproduces the ISCO and the final plunge.  Near the horizon the coordinate
// motion is slowed by the lapse (1 - r_s/r) so icons appear to freeze and
// redshift away, as a distant observer would see.
struct IconSprite {
    dvec3  pos, vel;       // metres, m/s
    int    atlasIndex = 0;
    double halfSize = 0;   // metres
    double spin = 0, spinRate = 0;
    double brightness = 1.0, stretch = 1.0;
};
static vector<IconSprite> icons;
static IconAtlas iconAtlas;
static std::mt19937 rng(20251028u);
static double nextSpawnTime = 0.0;
static const int MAX_ICONS = 14;

static double urand(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

static void spawnIcon() {
    if (iconAtlas.count == 0) return;
    IconSprite s;
    double rs = SagA.r_s;
    double r  = urand(7.5, 14.0) * rs;
    double az = urand(0.0, 2.0 * M_PI);
    double incl = urand(-0.45, 0.45);                       // mostly near the disk plane
    dvec3 axis = glm::normalize(dvec3(std::sin(incl) * std::cos(az), std::cos(incl), std::sin(incl) * std::sin(az)));
    // random point on the orbit circle around `axis`
    dvec3 e1 = glm::normalize(glm::cross(axis, std::abs(axis.y) < 0.9 ? dvec3(0, 1, 0) : dvec3(1, 0, 0)));
    dvec3 e2 = glm::cross(axis, e1);
    double th = urand(0.0, 2.0 * M_PI);
    s.pos = (e1 * std::cos(th) + e2 * std::sin(th)) * r;
    double vcirc = std::sqrt(G_const * SagA.mass * r) / (r - rs);  // circular speed in the PW potential
    s.vel = glm::cross(axis, s.pos / r) * (vcirc * urand(0.93, 1.02));
    s.atlasIndex = int(urand(0.0, double(iconAtlas.count) - 1e-6));
    s.halfSize = rs * urand(0.5, 0.8);
    s.spin = urand(0.0, 2.0 * M_PI);
    s.spinRate = urand(-0.6, 0.6);
    icons.push_back(s);
}
static void stepIcons(double simDt, double realDt, double now) {
    if (!sim.showIcons) return;
    const double rs = SagA.r_s, GM = G_const * SagA.mass;
    const double dragTau = 2.0e4;                            // s (sim) - orbital decay time scale
    for (auto& s : icons) {
        double r = glm::length(s.pos);
        double f = std::max(1.0 - rs / r, 0.0);
        double h = simDt * std::pow(f, 1.5);                 // coordinate time slows near the horizon
        auto accel = [&](const dvec3& p) {
            double rr = glm::length(p);
            double d  = std::max(rr - rs, 0.05 * rs);
            return -p / rr * (GM / (d * d));
        };
        s.vel += accel(s.pos) * (0.5 * h);
        s.pos += s.vel * h;
        s.vel += accel(s.pos) * (0.5 * h);
        s.vel *= std::exp(-h / dragTau);                     // gentle drag: spiral inwards
        s.spin += s.spinRate * realDt;
        r = glm::length(s.pos);
        f = std::max(1.0 - rs / r, 0.0);
        s.brightness = std::pow(f, 0.6) * glm::smoothstep(0.06, 0.2, f);
        s.stretch = 1.0 + 2.5 * std::pow(rs / r, 2.0);
    }
    icons.erase(std::remove_if(icons.begin(), icons.end(), [&](const IconSprite& s) {
        double r = glm::length(s.pos);
        return (1.0 - rs / r) < 0.06 || r > 60.0 * rs;
    }), icons.end());
    if ((int)icons.size() < MAX_ICONS && now >= nextSpawnTime) {
        spawnIcon();
        nextSpawnTime = now + urand(1.5, 4.0);
    }
}

// --------------------------------------------------------------- file helpers
static string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    string p(buf, buf + n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    string p = n > 0 ? string(buf, buf + n) : string();
#endif
    size_t s = p.find_last_of("\\/");
    return s == string::npos ? string(".") : p.substr(0, s);
}
// Shader sources: the interactive app prefers files on disk (next to the exe,
// in the cwd, then the source tree) so shaders can be edited without a rebuild;
// the screensaver, which may be installed anywhere, uses the embedded copies.
static string loadShaderSource(const string& name) {
#ifndef BH_SCREENSAVER
    const string candidates[] = { exeDir() + "/" + name, name, string(BH_SOURCE_DIR) + "/" + name };
    for (const auto& c : candidates) {
        std::ifstream in(c);
        if (in.good()) { std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
    }
#endif
    auto it = kEmbeddedShaders.find(name);
    if (it != kEmbeddedShaders.end()) return it->second;
    cerr << "Shader '" << name << "' not found on disk or embedded\n";
    exit(EXIT_FAILURE);
}

// --------------------------------------------------------------- engine
struct Engine {
    GLFWwindow* window = nullptr;
    GLuint quadVAO = 0, quadVBO = 0, texture = 0;
    GLuint quadProgram = 0, gridProgram = 0, computeProgram = 0;
    GLuint cameraUBO = 0, paramsUBO = 0, objectsUBO = 0, spritesUBO = 0;
    GLuint atlasTex = 0;
    GLuint gridVAO = 0, gridVBO = 0, gridEBO = 0;
    int    gridIndexCount = 0;

    int WIDTH = 1280, HEIGHT = 800;          // framebuffer size (updated on resize)
    int texW = 0, texH = 0;                  // current compute texture size
    static bool noFocus;                     // create the window without taking keyboard focus
    static bool coverScreen;                 // screensaver: undecorated, on top, over the whole virtual desktop
    bool fullscreen = false;
    int  winX = 100, winY = 100, winW = 1280, winH = 800;   // windowed placement, restored on F11

    // Borderless fullscreen on the monitor the window currently occupies.
    void toggleFullscreen() {
        if (!fullscreen) {
            glfwGetWindowPos(window, &winX, &winY);
            glfwGetWindowSize(window, &winW, &winH);
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            int cx = winX + winW / 2, cy = winY + winH / 2, count = 0;
            GLFWmonitor** mons = glfwGetMonitors(&count);
            for (int i = 0; i < count; ++i) {
                int mx, my; glfwGetMonitorPos(mons[i], &mx, &my);
                const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
                if (cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height) mon = mons[i];
            }
            const GLFWvidmode* mode = glfwGetVideoMode(mon);
            glfwSetWindowMonitor(window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            glfwSetWindowMonitor(window, nullptr, winX, winY, winW, winH, 0);
        }
        fullscreen = !fullscreen;
        glfwGetFramebufferSize(window, &WIDTH, &HEIGHT);
        cout << "[INFO] " << (fullscreen ? "fullscreen " : "windowed ") << WIDTH << "x" << HEIGHT << endl;
    }

    Engine() {
        if (!glfwInit()) { cerr << "GLFW init failed\n"; exit(EXIT_FAILURE); }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        if (noFocus) {
            glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
            glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        }
        int posX = 0, posY = 0;
        if (coverScreen) {
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
#ifdef _WIN32
            posX = GetSystemMetrics(SM_XVIRTUALSCREEN);  posY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            WIDTH = GetSystemMetrics(SM_CXVIRTUALSCREEN); HEIGHT = GetSystemMetrics(SM_CYVIRTUALSCREEN);
#else
            const GLFWvidmode* vm = glfwGetVideoMode(glfwGetPrimaryMonitor());
            WIDTH = vm->width; HEIGHT = vm->height;
#endif
        }
        window = glfwCreateWindow(WIDTH, HEIGHT, "Black Hole Simulation", nullptr, nullptr);
        if (!window) {
            cerr << "Failed to create an OpenGL 4.3 window (compute shaders are required)\n";
            glfwTerminate(); exit(EXIT_FAILURE);
        }
        if (coverScreen) {
            glfwSetWindowPos(window, posX, posY);
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
        }
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        glewExperimental = GL_TRUE;
        GLenum glewErr = glewInit();
        if (glewErr != GLEW_OK) {
            cerr << "Failed to initialize GLEW: " << (const char*)glewGetErrorString(glewErr) << "\n";
            glfwTerminate(); exit(EXIT_FAILURE);
        }
        glGetError(); // GLEW may raise a spurious GL_INVALID_ENUM
        glfwGetFramebufferSize(window, &WIDTH, &HEIGHT);
        cout << "OpenGL  " << glGetString(GL_VERSION) << "\n"
             << "GPU     " << glGetString(GL_RENDERER) << "\n";

        quadProgram    = createQuadProgram();
        gridProgram    = link({ compile(GL_VERTEX_SHADER,   loadShaderSource("grid.vert"), "grid.vert"),
                                compile(GL_FRAGMENT_SHADER, loadShaderSource("grid.frag"), "grid.frag") }, "grid");
        computeProgram = link({ compile(GL_COMPUTE_SHADER, loadShaderSource("geodesic.comp"), "geodesic.comp") }, "compute");

        glGenBuffers(1, &spritesUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, spritesUBO);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(SpritesUBO), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 4, spritesUBO);

        // 1x1 placeholder atlas so the sampler is always valid
        glGenTextures(1, &atlasTex);
        glBindTexture(GL_TEXTURE_2D, atlasTex);
        unsigned char px[4] = { 0, 0, 0, 0 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenBuffers(1, &cameraUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
        glBufferData(GL_UNIFORM_BUFFER, 128, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, cameraUBO);

        glGenBuffers(1, &paramsUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, paramsUBO);
        glBufferData(GL_UNIFORM_BUFFER, 64, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, paramsUBO);

        glGenBuffers(1, &objectsUBO);
        glBindBuffer(GL_UNIFORM_BUFFER, objectsUBO);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(ObjectsUBO), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 3, objectsUBO);

        createQuad();
    }
    ~Engine() {
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
    }

    // ---- std140 mirrors of the shader uniform blocks ----
    struct CameraUBO {
        vec3 pos;     float _p0;
        vec3 right;   float _p1;
        vec3 up;      float _p2;
        vec3 forward; float _p3;
        float tanHalfFov;
        float aspect;
        int   moving;
        int   _p4;
    };
    struct ParamsUBO {
        float disk_r1, disk_r2, rs, simTime;
        int   flags, maxSteps;
        float escapeR, exposure;
    };
    struct ObjectsUBO {
        int   numObjects; float _p0, _p1, _p2;
        vec4  posRadius[16];
        vec4  color[16];
    };
    struct SpritesUBO {
        int   numSprites; float atlasCols, atlasRows, _p0;
        vec4  posSize[32];
        vec4  info[32];
        vec4  normal[32];     // xyz billboard normal, w bounding radius^2 (rs units)
        vec4  radial[32];     // xyz in-plane radial axis, w cos(spin)
        vec4  tangent[32];    // xyz in-plane tangential axis, w sin(spin)
    };

    void uploadAtlas(const IconAtlas& a) {
        if (a.count == 0) return;
        glBindTexture(GL_TEXTURE_2D, atlasTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, a.width, a.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, a.rgba.data());
    }
    void uploadSpritesUBO(const vector<IconSprite>& sprites, const IconAtlas& a, const vec3& camPos) {
        SpritesUBO d{};
        int n = 0;
        if (sim.showIcons) {
            for (const auto& s : sprites) {
                if (n >= 32) break;
                vec3 C = vec3(s.pos);
                vec3 nrm = glm::normalize(camPos - C);
                vec3 rad = -C - glm::dot(-C, nrm) * nrm;           // towards the hole, in the billboard plane
                float rl = glm::length(rad);
                rad = rl > 1e-6f ? rad / rl : glm::normalize(glm::cross(nrm, vec3(0, 1, 0)));
                vec3 tan = glm::cross(nrm, rad);
                float hsRs = float(s.halfSize / SagA.r_s);
                float ext  = hsRs * float(std::max(s.stretch, 1.0)) * 1.45f;   // half-diagonal incl. stretch
                d.posSize[n] = vec4(C, float(s.halfSize));
                d.info[n]    = vec4(float(s.atlasIndex), float(s.brightness), float(s.stretch), float(s.spin));
                d.normal[n]  = vec4(nrm, ext * ext);
                d.radial[n]  = vec4(rad, float(std::cos(s.spin)));
                d.tangent[n] = vec4(tan, float(std::sin(s.spin)));
                ++n;
            }
        }
        d.numSprites = n;
        d.atlasCols = float(std::max(a.cols, 1));
        d.atlasRows = float(std::max(a.rows, 1));
        glBindBuffer(GL_UNIFORM_BUFFER, spritesUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
    }

    // ---- shaders ----
    static GLuint compile(GLenum type, const string& src, const string& label) {
        GLuint sh = glCreateShader(type);
        const char* s = src.c_str();
        glShaderSource(sh, 1, &s, nullptr);
        glCompileShader(sh);
        GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
            vector<char> log(std::max(len, 1));
            glGetShaderInfoLog(sh, len, nullptr, log.data());
            cerr << "Shader compile error (" << label << "):\n" << log.data() << "\n";
            exit(EXIT_FAILURE);
        }
        return sh;
    }
    static GLuint link(const vector<GLuint>& shaders, const string& label) {
        GLuint prog = glCreateProgram();
        for (GLuint s : shaders) glAttachShader(prog, s);
        glLinkProgram(prog);
        GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
            vector<char> log(std::max(len, 1));
            glGetProgramInfoLog(prog, len, nullptr, log.data());
            cerr << "Program link error (" << label << "):\n" << log.data() << "\n";
            exit(EXIT_FAILURE);
        }
        for (GLuint s : shaders) glDeleteShader(s);
        return prog;
    }
    GLuint createQuadProgram() {
        const char* vs = R"(
            #version 330 core
            layout (location = 0) in vec2 aPos;
            layout (location = 1) in vec2 aTexCoord;
            out vec2 TexCoord;
            void main() { gl_Position = vec4(aPos, 0.0, 1.0); TexCoord = aTexCoord; })";
        const char* fs = R"(
            #version 330 core
            in vec2 TexCoord;
            out vec4 FragColor;
            uniform sampler2D screenTexture;
            void main() { FragColor = texture(screenTexture, TexCoord); })";
        return link({ compile(GL_VERTEX_SHADER, vs, "quad.vert"),
                      compile(GL_FRAGMENT_SHADER, fs, "quad.frag") }, "quad");
    }

    // ---- fullscreen quad + output texture ----
    void createQuad() {
        float quad[] = {
            -1.f,  1.f, 0.f, 1.f,
            -1.f, -1.f, 0.f, 0.f,
             1.f, -1.f, 1.f, 0.f,
            -1.f,  1.f, 0.f, 1.f,
             1.f, -1.f, 1.f, 0.f,
             1.f,  1.f, 1.f, 1.f,
        };
        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);
        glBindVertexArray(quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        ensureTexture(WIDTH, HEIGHT);
    }
    void ensureTexture(int w, int h) {
        w = std::max(w, 1); h = std::max(h, 1);
        if (w == texW && h == texH) return;
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        texW = w; texH = h;
    }

    // ---- uniform uploads ----
    void uploadCameraUBO(const Camera& cam) {
        CameraUBO d{};
        vec3 fwd   = glm::normalize(cam.target - cam.position());
        vec3 right = glm::normalize(glm::cross(fwd, vec3(0, 1, 0)));
        vec3 up    = glm::cross(right, fwd);
        d.pos = cam.position(); d.right = right; d.up = up; d.forward = fwd;
        d.tanHalfFov = std::tan(glm::radians(60.0f * 0.5f));
        d.aspect = float(WIDTH) / float(std::max(HEIGHT, 1));
        d.moving = cam.dragging ? 1 : 0;
        glBindBuffer(GL_UNIFORM_BUFFER, cameraUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
    }
    void uploadParamsUBO() {
        ParamsUBO d{};
        d.rs      = float(SagA.r_s);
        d.disk_r1 = float(SagA.r_s * 3.0);     // innermost stable circular orbit
        d.disk_r2 = float(SagA.r_s * 7.5);
        d.simTime = float(sim.simTime);
        d.flags   = (sim.showDisk ? 1 : 0) | (sim.showStars ? 2 : 0) |
                    (sim.doppler ? 4 : 0) | (sim.diskTexture ? 8 : 0);
        d.maxSteps = 1500;
        d.escapeR  = 200.0f;
        d.exposure = sim.exposure;
        glBindBuffer(GL_UNIFORM_BUFFER, paramsUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
    }
    void uploadObjectsUBO(const vector<ObjectData>& objs) {
        ObjectsUBO d{};
        int n = 0;
        for (const auto& o : objs) {
            if (o.isBlackHole || n >= 16) continue;   // the horizon is handled analytically
            d.posRadius[n] = vec4(vec3(o.pos), float(o.radius));
            d.color[n] = o.color;
            ++n;
        }
        d.numObjects = n;
        glBindBuffer(GL_UNIFORM_BUFFER, objectsUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(d), &d);
    }

    // ---- ray tracing pass ----
    void dispatchCompute(const Camera& cam) {
        // lower resolution while dragging keeps interaction snappy on slower GPUs
        float scale = cam.dragging ? std::min(sim.renderScale, 0.5f) : sim.renderScale;
        ensureTexture(int(WIDTH * scale + 0.5f), int(HEIGHT * scale + 0.5f));

        glUseProgram(computeProgram);
        uploadCameraUBO(cam);
        uploadParamsUBO();
        uploadObjectsUBO(objects);
        uploadSpritesUBO(icons, iconAtlas, cam.position());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, atlasTex);
        glUniform1i(glGetUniformLocation(computeProgram, "iconAtlas"), 1);
        glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute((texW + 15) / 16, (texH + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    void drawFullScreenQuad() {
        glUseProgram(quadProgram);
        glBindVertexArray(quadVAO);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(quadProgram, "screenTexture"), 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    // ---- spacetime grid (Flamm's paraboloid embedding) ----
    void generateGrid(const vector<ObjectData>& objs) {
        const int   gridSize = 40;
        const float spacing  = float(SagA.r_s) * 1.25f;
        const float baseY    = -float(SagA.r_s) * 2.6f;

        vector<vec3>   vertices;
        vector<GLuint> indices;
        vertices.reserve((gridSize + 1) * (gridSize + 1));

        for (int z = 0; z <= gridSize; ++z) {
            for (int x = 0; x <= gridSize; ++x) {
                float worldX = (x - gridSize / 2) * spacing;
                float worldZ = (z - gridSize / 2) * spacing;
                double y = baseY;
                for (const auto& obj : objs) {
                    double r_s  = 2.0 * G_const * obj.mass / (c_light * c_light);
                    double dx   = worldX - obj.pos.x;
                    double dz   = worldZ - obj.pos.z;
                    double dist = std::sqrt(dx * dx + dz * dz);
                    // Flamm's paraboloid: embedding height  h(r) = 2 sqrt(r_s (r - r_s))
                    if (dist > r_s) y += 2.0 * std::sqrt(r_s * (dist - r_s));
                    else            y += 0.0;     // bottom of the well
                }
                vertices.emplace_back(worldX, float(y), worldZ);
            }
        }
        for (int z = 0; z < gridSize; ++z) {
            for (int x = 0; x < gridSize; ++x) {
                GLuint i = GLuint(z * (gridSize + 1) + x);
                indices.push_back(i); indices.push_back(i + 1);
                indices.push_back(i); indices.push_back(i + GLuint(gridSize + 1));
            }
        }
        // closing edges
        for (int x = 0; x < gridSize; ++x) {
            GLuint i = GLuint(gridSize * (gridSize + 1) + x);
            indices.push_back(i); indices.push_back(i + 1);
        }
        for (int z = 0; z < gridSize; ++z) {
            GLuint i = GLuint(z * (gridSize + 1) + gridSize);
            indices.push_back(i); indices.push_back(i + GLuint(gridSize + 1));
        }

        if (gridVAO == 0) glGenVertexArrays(1, &gridVAO);
        if (gridVBO == 0) glGenBuffers(1, &gridVBO);
        if (gridEBO == 0) glGenBuffers(1, &gridEBO);
        glBindVertexArray(gridVAO);
        glBindBuffer(GL_ARRAY_BUFFER, gridVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(vec3), vertices.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gridEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3), (void*)0);
        gridIndexCount = int(indices.size());
        glBindVertexArray(0);
    }
    void drawGrid(const mat4& viewProj) {
        glUseProgram(gridProgram);
        glUniformMatrix4fv(glGetUniformLocation(gridProgram, "viewProj"), 1, GL_FALSE, glm::value_ptr(viewProj));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glUniform1i(glGetUniformLocation(gridProgram, "sceneTexture"), 0);
        glUniform2f(glGetUniformLocation(gridProgram, "viewportSize"), float(WIDTH), float(HEIGHT));
        glBindVertexArray(gridVAO);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawElements(GL_LINES, gridIndexCount, GL_UNSIGNED_INT, 0);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }
};
bool Engine::noFocus = false;
bool Engine::coverScreen = false;
static Engine* engine = nullptr;

// --------------------------------------------------------------- input
static void printHelp() {
    cout <<
        "\n  Black hole simulation - controls\n"
        "  --------------------------------\n"
        "  left drag   orbit camera        wheel      zoom\n"
        "  G           pause/resume orbits R          reset camera   (Shift+R: reset orbits)\n"
        "  D           accretion disk      S          starfield\n"
        "  B           Doppler/redshift    T          disk texture\n"
        "  W           spacetime grid      + / -      time scale x2 / /2\n"
        "  [ / ]       exposure            A          auto resolution scaling\n"
        "  I           app icons           O          auto-orbit camera\n"
        "  F11         fullscreen          F12        save screenshot\n"
        "  H           this help           Esc        quit\n\n";
}
static bool screenshotRequested = false;   // set by F12, consumed in the render loop
static void onKey(GLFWwindow* win, int key, int /*scancode*/, int action, int mods) {
    if (action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(win, GLFW_TRUE); break;
    case GLFW_KEY_G: sim.orbitsRunning = !sim.orbitsRunning;
        cout << "[INFO] orbits " << (sim.orbitsRunning ? "running" : "paused") << endl; break;
    case GLFW_KEY_R:
        if (mods & GLFW_MOD_SHIFT) { resetObjects(); cout << "[INFO] orbits reset" << endl; }
        else { camera.reset(); cout << "[INFO] camera reset" << endl; }
        break;
    case GLFW_KEY_D: sim.showDisk = !sim.showDisk; break;
    case GLFW_KEY_S: sim.showStars = !sim.showStars; break;
    case GLFW_KEY_B: sim.doppler = !sim.doppler; break;
    case GLFW_KEY_T: sim.diskTexture = !sim.diskTexture; break;
    case GLFW_KEY_W: sim.showGrid = !sim.showGrid; break;
    case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD:
        sim.timeScale = std::min(sim.timeScale * 2.0, 3600.0 * 24 * 365);
        cout << "[INFO] time scale " << sim.timeScale << " s/s" << endl; break;
    case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT:
        sim.timeScale = std::max(sim.timeScale / 2.0, 1.0);
        cout << "[INFO] time scale " << sim.timeScale << " s/s" << endl; break;
    case GLFW_KEY_LEFT_BRACKET:  sim.exposure = std::max(sim.exposure / 1.25f, 0.1f); break;
    case GLFW_KEY_RIGHT_BRACKET: sim.exposure = std::min(sim.exposure * 1.25f, 10.0f); break;
    case GLFW_KEY_A:
        sim.autoScale = !sim.autoScale;
        if (!sim.autoScale) sim.renderScale = 1.0f;
        cout << "[INFO] auto resolution " << (sim.autoScale ? "on" : "off (full resolution)") << endl; break;
    case GLFW_KEY_H: printHelp(); break;
    case GLFW_KEY_I:
        sim.showIcons = !sim.showIcons;
        if (sim.showIcons && iconAtlas.count == 0) {
            iconAtlas = harvestAppIcons(48);
            engine->uploadAtlas(iconAtlas);
            cout << "[INFO] harvested " << iconAtlas.count << " app icons" << endl;
        }
        break;
    case GLFW_KEY_O: sim.autoOrbit = !sim.autoOrbit; break;
    case GLFW_KEY_F11: engine->toggleFullscreen(); break;
    case GLFW_KEY_F12: screenshotRequested = true; break;
    default: break;
    }
}
// Screensaver input handling: any key, button or real mouse movement ends it.
static double saverStart = 0.0, saverMouseX = -1.0, saverMouseY = -1.0;
static bool saverShouldExit(GLFWwindow* w, double x, double y) {
    if (glfwGetTime() - saverStart < 0.75) { saverMouseX = x; saverMouseY = y; return false; }  // settle
    if (saverMouseX < 0.0) { saverMouseX = x; saverMouseY = y; return false; }
    bool moved = std::abs(x - saverMouseX) > 12.0 || std::abs(y - saverMouseY) > 12.0;
    if (moved) glfwSetWindowShouldClose(w, GLFW_TRUE);
    return moved;
}
static void setupCallbacks(GLFWwindow* window) {
    glfwSetWindowUserPointer(window, &camera);
    saverStart = glfwGetTime();
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int b, int a, int m) {
        if (sim.saverMode) { if (glfwGetTime() - saverStart > 0.75) glfwSetWindowShouldClose(w, GLFW_TRUE); return; }
        static_cast<Camera*>(glfwGetWindowUserPointer(w))->processMouseButton(b, a, m, w);
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
        if (sim.saverMode) { saverShouldExit(w, x, y); return; }
        static_cast<Camera*>(glfwGetWindowUserPointer(w))->processMouseMove(x, y);
    });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double x, double y) {
        if (sim.saverMode) { glfwSetWindowShouldClose(w, GLFW_TRUE); return; }
        static_cast<Camera*>(glfwGetWindowUserPointer(w))->processScroll(x, y);
    });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int sc, int action, int mods) {
        if (sim.saverMode) { if (action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE); return; }
        onKey(w, key, sc, action, mods);
    });
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow*, int w, int h) {
        engine->WIDTH = std::max(w, 1); engine->HEIGHT = std::max(h, 1);
    });
}

static string formatSimTime(double s) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1);
    if (s < 3600.0)            os << s / 60.0 << " min";
    else if (s < 86400.0)      os << s / 3600.0 << " h";
    else if (s < 86400.0 * 365) os << s / 86400.0 << " d";
    else                       os << s / (86400.0 * 365) << " yr";
    return os.str();
}

// --------------------------------------------------------------- screenshots
// Reads the back buffer (call before glfwSwapBuffers) and writes a 24-bit BMP.
static bool saveScreenshot(const string& path, int w, int h) {
    vector<unsigned char> rgb(size_t(w) * size_t(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

    const int rowBytes = (w * 3 + 3) & ~3;
    const uint32_t dataSize = uint32_t(rowBytes) * uint32_t(h);
    auto put32 = [](unsigned char* p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; };
    unsigned char fileHdr[14] = { 'B', 'M' }, infoHdr[40] = { 40 };
    put32(fileHdr + 2, 54 + dataSize); put32(fileHdr + 10, 54);
    put32(infoHdr + 4, uint32_t(w)); put32(infoHdr + 8, uint32_t(h));
    infoHdr[12] = 1; infoHdr[14] = 24; put32(infoHdr + 20, dataSize);

    std::ofstream out(path, std::ios::binary);
    if (!out) { cerr << "[WARN] cannot write " << path << endl; return false; }
    out.write((const char*)fileHdr, 14);
    out.write((const char*)infoHdr, 40);
    vector<unsigned char> row(rowBytes, 0);
    for (int y = 0; y < h; ++y) {                       // GL rows are bottom-up, like BMP
        const unsigned char* src = &rgb[size_t(y) * w * 3];
        for (int x = 0; x < w; ++x) { row[x * 3] = src[x * 3 + 2]; row[x * 3 + 1] = src[x * 3 + 1]; row[x * 3 + 2] = src[x * 3]; }
        out.write((const char*)row.data(), rowBytes);
    }
    cout << "[INFO] screenshot saved: " << path << endl;
    return true;
}
static string timestampedName() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "blackhole_%Y%m%d_%H%M%S.bmp", std::localtime(&t));
    return buf;
}

// Applies a string of key toggles as if typed (e.g. "bw" = Doppler off, grid off).
static void applyKeyString(GLFWwindow* win, const string& keys) {
    for (char ch : keys) {
        int key = -1, mods = 0;
        if (ch >= 'a' && ch <= 'z') key = GLFW_KEY_A + (ch - 'a');
        else if (ch >= 'A' && ch <= 'Z') { key = GLFW_KEY_A + (ch - 'A'); mods = GLFW_MOD_SHIFT; }
        else if (ch == '+' || ch == '=') key = GLFW_KEY_EQUAL;
        else if (ch == '-') key = GLFW_KEY_MINUS;
        else if (ch == '[') key = GLFW_KEY_LEFT_BRACKET;
        else if (ch == ']') key = GLFW_KEY_RIGHT_BRACKET;
        if (key >= 0) onKey(win, key, 0, GLFW_PRESS, mods);
    }
}

// --------------------------------------------------------------- main
//   --stats              print frame statistics to the console every 2 s
//   --no-vsync           uncapped frame rate (useful with --stats)
//   --size WxH           initial window size, e.g. --size 1920x1080
//   --fullscreen         start in borderless fullscreen (F11 toggles)
//   --keys <chars>       apply key toggles at start-up, e.g. --keys bw
//   --camera az,el,d     initial camera: azimuth and elevation in radians, distance in r_s
//   --screenshot <file>  render --frames N frames (default 90), save a BMP and exit
//   --icons              show harvested app icons spiralling into the hole
//   --saver              behave as a screensaver (cover the screen, exit on input); same as /s
int main(int argc, char** argv) {
    bool stats = false, vsync = true, startFullscreen = false, wantIcons = false;
    int reqW = 0, reqH = 0, exitAfterFrames = 90;
    string keys, screenshotPath;
    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        // Windows screensaver conventions: /s run, /c[:hwnd] configure, /p hwnd preview
        if (a.size() >= 2 && (a[0] == '/' || a[0] == '-') && a.size() <= 3 + 10) {
            char c = char(tolower((unsigned char)a[1]));
            if (c == 's' && (a.size() == 2 || a[2] == ':')) { sim.saverMode = true; continue; }
            if (c == 'c') {
#ifdef _WIN32
                MessageBoxA(nullptr,
                    "Black Hole screensaver\n\n"
                    "A Schwarzschild black hole with a Doppler-beamed accretion disk,\n"
                    "a gravitationally lensed sky, and the icons of your installed apps\n"
                    "spiralling past the event horizon.\n\n"
                    "There are no settings. Any key or mouse movement exits.\n\n"
                    "This is a computer simulation only: no spacetime is harmed.",
                    "Black Hole screensaver", MB_OK | MB_ICONINFORMATION);
#endif
                return 0;
            }
            if (c == 'p') return 0;               // no preview rendering in the settings dialog
        }
        if (a == "--stats") stats = true;
        else if (a == "--no-vsync") vsync = false;
        else if (a == "--fullscreen") startFullscreen = true;
        else if (a == "--icons") wantIcons = true;
        else if (a == "--saver") sim.saverMode = true;
        else if (a == "--size" && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &reqW, &reqH) != 2) reqW = reqH = 0; }
        else if (a == "--keys" && i + 1 < argc) keys = argv[++i];
        else if (a == "--screenshot" && i + 1 < argc) screenshotPath = argv[++i];
        else if (a == "--frames" && i + 1 < argc) exitAfterFrames = std::max(1, atoi(argv[++i]));
        else if (a == "--camera" && i + 1 < argc) {           // azimuth(rad),elevation(rad),distance(rs)
            float az, el, r;
            if (sscanf(argv[++i], "%f,%f,%f", &az, &el, &r) == 3) {
                camera.azimuth = az; camera.elevation = el; camera.radius = r * float(SagA.r_s);
            }
        }
        else if (a == "--help" || a == "-h") {
            cout << "usage: BlackHole3D [--stats] [--no-vsync] [--fullscreen] [--icons] [--saver] [--size WxH]\n"
                    "                   [--keys <chars>] [--camera az,el,dist_rs] [--screenshot <file.bmp> [--frames N]]\n"; return 0;
        }
    }
#ifdef BH_SCREENSAVER
    if (!sim.saverMode && screenshotPath.empty()) sim.saverMode = true;   // double-clicking the .scr
#endif
    if (sim.saverMode) {
        Engine::coverScreen = true;
        wantIcons = true;
        sim.autoOrbit = true;
        sim.showGrid = false;
    }
    // scripted capture: do not steal keyboard focus from whatever the user is doing
    if (!screenshotPath.empty()) Engine::noFocus = true;
    Engine eng;
    engine = &eng;
    if (reqW > 0 && reqH > 0 && !sim.saverMode) glfwSetWindowSize(eng.window, reqW, reqH);
    if (startFullscreen && !sim.saverMode) eng.toggleFullscreen();
    glfwSwapInterval(vsync ? 1 : 0);
    setupObjects();
    setupCallbacks(eng.window);
    if (!sim.saverMode) printHelp();
    applyKeyString(eng.window, keys);
    if (wantIcons) {
        iconAtlas = harvestAppIcons(48);
        eng.uploadAtlas(iconAtlas);
        sim.showIcons = iconAtlas.count > 0;
        cout << "[INFO] harvested " << iconAtlas.count << " app icons" << endl;
        for (int i = 0; i < 6; ++i) spawnIcon();          // start with a few already in orbit
    }
    double statsTime = glfwGetTime();

    double lastTime = glfwGetTime(), fpsTime = lastTime;
    int frames = 0, totalFrames = 0;

    while (!glfwWindowShouldClose(eng.window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        double dt  = std::min(now - lastTime, 0.1);     // clamp long stalls
        lastTime   = now;

        if (sim.orbitsRunning) {
            double simDt = dt * sim.timeScale;
            stepOrbits(simDt);
            stepIcons(simDt, dt, now);
            sim.simTime += simDt;
        }
        if (sim.autoOrbit && !camera.dragging) {
            camera.azimuth  += float(dt * 0.035);
            camera.elevation = 1.22f + 0.16f * float(std::sin(now * 0.09));
            camera.radius    = float(SagA.r_s) * (13.0f + 2.5f * float(std::sin(now * 0.053)));
        }

        glViewport(0, 0, eng.WIDTH, eng.HEIGHT);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // 1) ray-traced scene
        eng.dispatchCompute(camera);
        eng.drawFullScreenQuad();

        // 2) translucent spacetime grid overlay
        if (sim.showGrid) {
            eng.generateGrid(objects);
            mat4 view = glm::lookAt(camera.position(), camera.target, vec3(0, 1, 0));
            mat4 proj = glm::perspective(glm::radians(60.0f), float(eng.WIDTH) / float(eng.HEIGHT),
                                         float(SagA.r_s) * 0.05f, float(SagA.r_s) * 2000.0f);
            eng.drawGrid(proj * view);
        }

        // 3) screenshots (read the back buffer before presenting it)
        ++totalFrames;
        if (screenshotRequested) {
            screenshotRequested = false;
            saveScreenshot(timestampedName(), eng.WIDTH, eng.HEIGHT);
        }
        if (!screenshotPath.empty() && totalFrames >= exitAfterFrames) {
            glFinish();
            saveScreenshot(screenshotPath, eng.WIDTH, eng.HEIGHT);
            glfwSetWindowShouldClose(eng.window, GLFW_TRUE);
        }

        glfwSwapBuffers(eng.window);

        // 4) title bar status
        ++frames;
        if (now - fpsTime >= 0.5) {
            double fps = frames / (now - fpsTime);
            if (sim.autoScale && !camera.dragging) {
                if (fps < sim.targetFps * 0.9)
                    sim.renderScale = std::max(sim.renderScale * 0.9f, 0.35f);
                else if (fps > sim.targetFps * 1.6 && sim.renderScale < 1.0f)
                    sim.renderScale = std::min(sim.renderScale * 1.05f, 1.0f);
            }
            std::ostringstream t;
            t << "Black Hole Simulation  |  " << std::fixed << std::setprecision(0) << fps << " fps  |  "
              << eng.texW << "x" << eng.texH << (sim.autoScale ? " auto" : "") << "  |  t = " << formatSimTime(sim.simTime)
              << "  (" << std::setprecision(0) << sim.timeScale << "x)"
              << (sim.orbitsRunning ? "" : "  [paused]")
              << "  |  disk " << (sim.showDisk ? "on" : "off")
              << "  doppler " << (sim.doppler ? "on" : "off")
              << "  stars " << (sim.showStars ? "on" : "off")
              << "  grid " << (sim.showGrid ? "on" : "off")
              << "  icons " << (sim.showIcons ? std::to_string(icons.size()) : "off");
            glfwSetWindowTitle(eng.window, t.str().c_str());
            if (stats && now - statsTime >= 2.0) {
                cout << "[STATS] " << t.str() << endl;
                statsTime = now;
            }
            frames = 0; fpsTime = now;
        }
    }
    return 0;
}

#ifdef BH_SCREENSAVER
// The .scr is a GUI-subsystem executable (no console window); forward to main().
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
