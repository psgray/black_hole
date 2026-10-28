# **black**_**hole**

Black hole simulation project

Here is the black hole raw code, everything will be inside a src bin incase you want to copy the files

I'm writing this as I'm beginning this project (hopefully I complete it ;D) here is what I plan to do:

1. Ray-tracing : add ray tracing to the gravity simulation to simulate gravitational lensing

2. Accretion disk : simulate accreciate disk using the ray tracing + the halos

3. Spacetime curvature : demonstrate visually the "trapdoor in spacetime" that is black holes using spacetime grid

4. [optional] try to make it run realtime ;D

I hope it works :/

Edit: After completion of project -

## **Building Requirements:**

1. C++ Compiler supporting C++ 17 or newer

2. [Cmake](https://cmake.org/)

3. [Vcpkg](https://vcpkg.io/en/)

4. [Git](https://git-scm.com/)

## **Build Instructions:**

1. Clone the repository:
	-  `git clone https://github.com/kavan010/black_hole.git`
2. CD into the newly cloned directory
	- `cd ./black_hole` 
3. Install dependencies with Vcpkg
	- `vcpkg install`
4. Get the vcpkg cmake toolchain file path
	- `vcpkg integrate install`
	- This will output something like : `CMake projects should use: "-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"`
5. Create a build directory
	- `mkdir build`
6. Configure project with CMake
	-  `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake`
	- Use the vcpkg cmake toolchain path from above
7. Build the project
	- `cmake --build build`
8. Run the program
	- The executables will be located in the build folder

### Alternative: Debian/Ubuntu apt workaround

If you don't want to use vcpkg, or you just need a quick way to install the native development packages on Debian/Ubuntu, install these packages and then run the normal CMake steps above:

```bash
sudo apt update
sudo apt install build-essential cmake \
	libglew-dev libglfw3-dev libglm-dev libgl1-mesa-dev
```

This provides the GLEW, GLFW, GLM and OpenGL development files so `find_package(...)` calls in `CMakeLists.txt` can locate the libraries. After installing, run the `cmake -B build -S .` and `cmake --build build` commands as shown in the Build Instructions.

### Building with Visual Studio 2026 (Windows)

The repo ships a `CMakePresets.json`, so with Visual Studio 2026 (or 2022), CMake and vcpkg installed and the `VCPKG_ROOT` environment variable set:

- **IDE:** *File → Open → Folder…* on the repo. Pick the `vs2026` configure preset; dependencies are installed by vcpkg on first configure. `BlackHole3D` is the startup project, so F5 just works (the debugger working directory is the output folder, where the shaders are copied).
- **Command line:**
  ```bash
  cmake --preset vs2026
  cmake --build --preset vs2026-release
  build\Release\BlackHole3D.exe
  ```
  This generates `build/BlackHoleSim.slnx` (VS 2026's solution format; VS 2022 via the `vs2022` preset produces a `.sln`), which can also be opened directly.

## **Controls (3D)**

| Input | Action |
|---|---|
| Left drag / wheel | Orbit camera / zoom |
| `G` | Pause / resume the stars' orbits |
| `R` / `Shift+R` | Reset camera / reset orbits |
| `D` `S` `B` `T` `W` | Toggle accretion disk, starfield, Doppler + gravitational redshift, disk texture, spacetime grid |
| `+` / `-` | Simulated time scale ×2 / ÷2 (default: 1 real second = 10 simulated minutes) |
| `[` / `]` | Exposure |
| `A` | Toggle automatic resolution scaling (targets ~30 fps on slower GPUs) |
| `F11` | Toggle borderless fullscreen (or start with `--fullscreen`) |
| `F12` | Save a screenshot (`blackhole_<date>.bmp`) |
| `H` / `Esc` | Help / quit |

Command-line options: `--stats`, `--no-vsync`, `--size WxH`, `--camera az,el,dist_rs`, `--keys <toggles>`, `--screenshot <file.bmp> [--frames N]` (renders N frames, saves and exits).

**2D demo:** left click launches a ray heading +x from the cursor, right drag pans, wheel zooms, `Space` pauses, `R` resets the default fan of rays, `C` clears.

## **What the 3D renderer does**

- Each pixel integrates a Schwarzschild null geodesic backwards from the camera. Because the spacetime is spherically symmetric every ray stays in a plane, so the shader integrates the exact orbit equation `d²u/dφ² = -u + (3/2)u²` (with `u = r_s/r`) with RK4 and an adaptive step. This has no polar-axis singularity and needs a few hundred steps per ray instead of tens of thousands, so the image is rendered at full window resolution.
- The thin accretion disk (ISCO at 3 r_s to 7.5 r_s) is shaded as a blackbody with the standard `T⁴ ∝ r⁻³(1-√(r_in/r))` profile, gravitational redshift and relativistic Doppler beaming from its Keplerian orbital speed, plus a turbulent texture that co-rotates with the gas. The far side of the disk appears bent over and under the shadow, and the thin photon ring at 1.5 r_s is visible.
- Rays that escape sample a procedural starfield in their final, lensed direction, so the sky is smeared into the Einstein ring around the shadow.
- The two stars follow stable Keplerian orbits (leapfrog integrator) and are lensed like everything else. The spacetime grid is Flamm's paraboloid embedding of the equatorial plane, drawn behind the ray-traced scene.
- Note: this is purely a computer simulation. No black holes are created or operated by running it.

## **How the code works:**
for 2D: simple, just run 2D_lensing.cpp with the nessesary dependencies installed.

for 3D: black_hole.cpp and geodesic.comp work together to run the simuation faster using GPU, essentially it sends over a UBO and geodesic.comp runs heavy calculations using that data.

should work with nessesary dependencies installed, however I have only run it on windows with my GPU so am not sure!

LMK if you would like an in-depth explanation of how the code works aswell :)
