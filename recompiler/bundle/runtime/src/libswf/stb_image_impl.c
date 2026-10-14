// Single translation unit that compiles the stb_image implementation.
// All other files include "stb_image.h" for declarations only.
//
// Linked into both graphics and no-graphics builds because MCL image loads
// (mcl.loadClip("foo.gif", clip)) must work even in trace-only mode.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PIC
#define STBI_NO_PSD
#define STBI_NO_PNM
#define STBI_NO_TGA
#include "stb_image.h"
