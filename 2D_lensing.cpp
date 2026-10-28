// ---------------------------------------------------------------------------
//  2D gravitational lensing demo
//
//  Light rays are integrated along Schwarzschild null geodesics in the
//  equatorial plane with a 4th-order Runge-Kutta scheme.  A fan of parallel
//  rays is launched at start-up so the bending (and capture inside the
//  critical impact parameter b = 3*sqrt(3)/2 * r_s) is visible immediately.
//
//  Controls
//    left click      launch a ray from the cursor heading +x
//    right drag      pan                     wheel     zoom
//    space           pause / resume          R         reset to the default fan
//    C               clear all rays          Esc       quit
// ---------------------------------------------------------------------------
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cmath>
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif
using glm::vec2;
using glm::dvec2;
using std::vector;

static const double c_light = 299792458.0;
static const double G_const = 6.67430e-11;

struct BlackHole {
    dvec2  position;
    double mass;
    double r_s;
    BlackHole(dvec2 pos, double m) : position(pos), mass(m) { r_s = 2.0 * G_const * mass / (c_light * c_light); }

    void draw() const {
        // event horizon
        glColor3f(0.0f, 0.0f, 0.0f);
        glBegin(GL_TRIANGLE_FAN);
        glVertex2d(position.x, position.y);
        for (int i = 0; i <= 120; i++) {
            double a = 2.0 * M_PI * i / 120;
            glVertex2d(position.x + r_s * std::cos(a), position.y + r_s * std::sin(a));
        }
        glEnd();
        // horizon outline + photon sphere (1.5 r_s) as a dashed hint
        glColor4f(1.0f, 0.3f, 0.2f, 0.9f);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < 120; i++) {
            double a = 2.0 * M_PI * i / 120;
            glVertex2d(position.x + r_s * std::cos(a), position.y + r_s * std::sin(a));
        }
        glEnd();
        glColor4f(1.0f, 0.8f, 0.3f, 0.35f);
        glBegin(GL_LINES);
        for (int i = 0; i < 120; i += 2) {
            double a0 = 2.0 * M_PI * i / 120, a1 = 2.0 * M_PI * (i + 1) / 120;
            glVertex2d(position.x + 1.5 * r_s * std::cos(a0), position.y + 1.5 * r_s * std::sin(a0));
            glVertex2d(position.x + 1.5 * r_s * std::cos(a1), position.y + 1.5 * r_s * std::sin(a1));
        }
        glEnd();
    }
};
static BlackHole SagA(dvec2(0.0, 0.0), 8.54e36);   // Sagittarius A*

struct Engine {
    GLFWwindow* window = nullptr;
    int WIDTH = 1200, HEIGHT = 800;
    double halfWidth  = 1.0e11;     // half extent of the view in metres
    double offsetX = 0.0, offsetY = 0.0;
    bool   panning = false;
    double lastX = 0.0, lastY = 0.0;

    Engine() {
        if (!glfwInit()) { std::cerr << "Failed to initialize GLFW\n"; exit(EXIT_FAILURE); }
        window = glfwCreateWindow(WIDTH, HEIGHT, "Black Hole - 2D lensing", nullptr, nullptr);
        if (!window) { std::cerr << "Failed to create GLFW window\n"; glfwTerminate(); exit(EXIT_FAILURE); }
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) { std::cerr << "Failed to initialize GLEW\n"; exit(EXIT_FAILURE); }
        glGetError();
    }
    double halfHeight() const { return halfWidth * HEIGHT / WIDTH; }
    void beginFrame() {
        glfwGetFramebufferSize(window, &WIDTH, &HEIGHT);
        glViewport(0, 0, WIDTH, HEIGHT);
        glClearColor(0.02f, 0.02f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-halfWidth + offsetX, halfWidth + offsetX, -halfHeight() + offsetY, halfHeight() + offsetY, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
    dvec2 screenToWorld(double sx, double sy) const {
        double x = (sx / WIDTH * 2.0 - 1.0) * halfWidth + offsetX;
        double y = (1.0 - sy / HEIGHT * 2.0) * halfHeight() + offsetY;
        return dvec2(x, y);
    }
};
static Engine engine;

struct Ray {
    double x, y;              // cartesian position (m)
    double r, phi;            // polar position
    double dr, dphi;          // d/dlambda
    double E, L;              // conserved energy / angular momentum
    bool   alive = true;      // false once captured or far away
    vector<vec2> trail;

    Ray(dvec2 pos, dvec2 dir) {
        x = pos.x; y = pos.y;
        r = std::sqrt(x * x + y * y);
        phi = std::atan2(y, x);
        dr   = dir.x * std::cos(phi) + dir.y * std::sin(phi);
        dphi = (-dir.x * std::sin(phi) + dir.y * std::cos(phi)) / r;
        L = r * r * dphi;
        double f = 1.0 - SagA.r_s / r;
        double dt_dLambda = std::sqrt((dr * dr) / (f * f) + (r * r * dphi * dphi) / f);   // null condition
        E = f * dt_dLambda;
        trail.push_back(vec2(float(x), float(y)));
    }
};
static vector<Ray> rays;
static bool paused = false;

// Schwarzschild null geodesic in the equatorial plane
static void geodesicRHS(double r, double dr, double dphi, double E, double rs, double out[4]) {
    double f = 1.0 - rs / r;
    double dt_dLambda = E / f;
    out[0] = dr;
    out[1] = dphi;
    out[2] = -(rs / (2 * r * r)) * f * (dt_dLambda * dt_dLambda)
           + (rs / (2 * r * r * f)) * (dr * dr)
           + (r - rs) * (dphi * dphi);
    out[3] = -2.0 * dr * dphi / r;
}
static void rk4Step(Ray& ray, double dLambda, double rs) {
    double y0[4] = { ray.r, ray.phi, ray.dr, ray.dphi };
    double k1[4], k2[4], k3[4], k4[4], t[4];
    geodesicRHS(y0[0], y0[2], y0[3], ray.E, rs, k1);
    for (int i = 0; i < 4; i++) t[i] = y0[i] + 0.5 * dLambda * k1[i];
    geodesicRHS(t[0], t[2], t[3], ray.E, rs, k2);
    for (int i = 0; i < 4; i++) t[i] = y0[i] + 0.5 * dLambda * k2[i];
    geodesicRHS(t[0], t[2], t[3], ray.E, rs, k3);
    for (int i = 0; i < 4; i++) t[i] = y0[i] + dLambda * k3[i];
    geodesicRHS(t[0], t[2], t[3], ray.E, rs, k4);
    ray.r    += (dLambda / 6.0) * (k1[0] + 2 * k2[0] + 2 * k3[0] + k4[0]);
    ray.phi  += (dLambda / 6.0) * (k1[1] + 2 * k2[1] + 2 * k3[1] + k4[1]);
    ray.dr   += (dLambda / 6.0) * (k1[2] + 2 * k2[2] + 2 * k3[2] + k4[2]);
    ray.dphi += (dLambda / 6.0) * (k1[3] + 2 * k2[3] + 2 * k3[3] + k4[3]);
}
static void stepRay(Ray& ray, double dLambda, double rs) {
    if (!ray.alive) return;
    if (ray.r <= rs * 1.001) { ray.alive = false; return; }          // captured
    if (ray.r > 4.0e12) { ray.alive = false; return; }                // long gone
    // smaller steps close to the hole where curvature is strong
    double h = dLambda * glm::clamp(ray.r / (6.0 * rs), 0.15, 1.0);
    rk4Step(ray, h, rs);
    ray.x = ray.r * std::cos(ray.phi);
    ray.y = ray.r * std::sin(ray.phi);
    ray.trail.push_back(vec2(float(ray.x), float(ray.y)));
    if (ray.trail.size() > 6000) ray.trail.erase(ray.trail.begin(), ray.trail.begin() + 1000);
}

static void drawRays() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.5f);
    for (const auto& ray : rays) {
        size_t N = ray.trail.size();
        if (N < 2) continue;
        glBegin(GL_LINE_STRIP);
        for (size_t i = 0; i < N; ++i) {
            float a = 0.15f + 0.85f * float(i) / float(N - 1);
            if (!ray.alive) a *= 0.6f;
            glColor4f(1.0f, 0.95f, 0.8f, a);
            glVertex2f(ray.trail[i].x, ray.trail[i].y);
        }
        glEnd();
    }
    glPointSize(4.0f);
    glBegin(GL_POINTS);
    for (const auto& ray : rays) {
        if (!ray.alive) continue;
        glColor3f(1.0f, 0.3f, 0.3f);
        glVertex2d(ray.x, ray.y);
    }
    glEnd();
    glDisable(GL_BLEND);
}

static void launchDefaultFan() {
    rays.clear();
    const int n = 31;
    const double x0 = -1.8e11;
    const double span = 5.0 * SagA.r_s;                // impact parameters from -span .. +span
    for (int i = 0; i < n; ++i) {
        double b = -span + 2.0 * span * i / (n - 1);
        rays.emplace_back(dvec2(x0, b), dvec2(c_light, 0.0));
    }
    // and one exactly at the critical impact parameter, which spirals onto the photon sphere
    rays.emplace_back(dvec2(x0, 1.5 * std::sqrt(3.0) * SagA.r_s), dvec2(c_light, 0.0));
}

static void printHelp() {
    std::cout <<
        "\n  2D lensing - controls\n"
        "  ---------------------\n"
        "  left click   launch a ray from the cursor heading +x\n"
        "  right drag   pan            wheel   zoom\n"
        "  space        pause/resume   R       reset default fan\n"
        "  C            clear          Esc     quit\n\n";
}

int main() {
    printHelp();
    launchDefaultFan();

    glfwSetMouseButtonCallback(engine.window, [](GLFWwindow* w, int button, int action, int) {
        double sx, sy; glfwGetCursorPos(w, &sx, &sy);
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
            dvec2 p = engine.screenToWorld(sx, sy);
            if (glm::length(p) > SagA.r_s * 1.05) rays.emplace_back(p, dvec2(c_light, 0.0));
        }
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            engine.panning = (action == GLFW_PRESS);
            engine.lastX = sx; engine.lastY = sy;
        }
    });
    glfwSetCursorPosCallback(engine.window, [](GLFWwindow*, double sx, double sy) {
        if (engine.panning) {
            engine.offsetX -= (sx - engine.lastX) / engine.WIDTH * 2.0 * engine.halfWidth;
            engine.offsetY += (sy - engine.lastY) / engine.HEIGHT * 2.0 * engine.halfHeight();
        }
        engine.lastX = sx; engine.lastY = sy;
    });
    glfwSetScrollCallback(engine.window, [](GLFWwindow*, double, double yoff) {
        engine.halfWidth *= std::pow(1.15, -yoff);
        engine.halfWidth = glm::clamp(engine.halfWidth, 2.0e10, 2.0e12);
    });
    glfwSetKeyCallback(engine.window, [](GLFWwindow* w, int key, int, int action, int) {
        if (action != GLFW_PRESS) return;
        if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(w, GLFW_TRUE);
        if (key == GLFW_KEY_SPACE)  paused = !paused;
        if (key == GLFW_KEY_R)      launchDefaultFan();
        if (key == GLFW_KEY_C)      rays.clear();
    });

    // each frame advances the rays by `stepsPerFrame` RK4 steps of 1 light-second... scaled down:
    // dLambda = 1 means the affine parameter advances so that |dx/dlambda| = c, i.e. ~3e8 m per step.
    const int stepsPerFrame = 6;
    while (!glfwWindowShouldClose(engine.window)) {
        glfwPollEvents();
        engine.beginFrame();
        if (!paused)
            for (int s = 0; s < stepsPerFrame; ++s)
                for (auto& ray : rays) stepRay(ray, 1.0, SagA.r_s);
        SagA.draw();
        drawRays();
        glfwSwapBuffers(engine.window);
    }
    glfwDestroyWindow(engine.window);
    glfwTerminate();
    return 0;
}
