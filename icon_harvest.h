// Collects icons of installed applications (Desktop, Start Menu and taskbar
// shortcuts) into a single RGBA texture atlas.  Everything stays local to the
// machine; nothing is read beyond the icon images themselves.
#pragma once
#include <vector>

struct IconAtlas {
    int cell = 64;                       // pixel size of one icon cell
    int cols = 8;
    int rows = 0;
    int count = 0;                       // number of valid icons
    int width = 0, height = 0;           // atlas size in pixels
    std::vector<unsigned char> rgba;     // width*height*4, bottom-up rows (OpenGL order)
};

// Returns an atlas with up to maxIcons unique icons.  On non-Windows platforms
// (or if nothing is found) count is 0.
IconAtlas harvestAppIcons(int maxIcons = 48);
