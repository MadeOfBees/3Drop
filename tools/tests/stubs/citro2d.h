// citro2d and citro3d stubs. fakes.c supplies inert defaults; testUi.c and testLed.c record what is
// drawn.
#ifndef TEST_STUBS_CITRO2D_H
#define TEST_STUBS_CITRO2D_H

#include "fakes.h"

#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C3D_FRAME_SYNCDRAW 1
#define C3D_FRAME_NONBLOCK 2
typedef struct {
	int screen;
} C3D_RenderTarget;
typedef const void *C2D_TextBuf;
// The parsed string is kept here so a recording C2D_DrawText can tell what it draws.
typedef struct {
	float width;
	const char *str;
} C2D_Text;
enum { C2D_AtBaseline = 1, C2D_WithColor = 2, C2D_AlignRight = 4, C2D_WordWrap = 16 };
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);
const char *C2D_TextParse(C2D_Text *text, C2D_TextBuf buf, const char *str);
void C2D_DrawText(const C2D_Text *text, u32 flags, float x, float y, float depth, float scaleX,
				  float scaleY, ...);
void C2D_DrawCircleSolid(float x, float y, float z, float radius, u32 color);

// For the scan's picture: a texture whose bytes a test can read back, and a draw that a test can
// count.
typedef struct {
	void *data;
	u16 width, height;
} C3D_Tex;
typedef struct {
	u16 width, height;
	float left, top, right, bottom;
} Tex3DS_SubTexture;
typedef struct {
	C3D_Tex *tex;
	const Tex3DS_SubTexture *subtex;
} C2D_Image;
typedef enum { GPU_RGBA8 = 0, GPU_RGB565 = 2 } GPU_TEXCOLOR;
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
bool C3D_TexInit(C3D_Tex *tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexSetFilter(C3D_Tex *tex, GPU_TEXTURE_FILTER_PARAM magFilter,
					  GPU_TEXTURE_FILTER_PARAM minFilter);
void C3D_TexDelete(C3D_Tex *tex);
bool C2D_DrawImageAt(C2D_Image img, float x, float y, float depth, const void *tint, float scaleX,
					 float scaleY);

#define gfxSetWide(enable) ((void)(enable))
#define gfxIsWide() false
#define fontEnsureMapped() 0
#define C3D_Init(size) ((void)(size), !fake.rendererFails)
#define C3D_Fini() ((void)0)
#define C2D_Init(maxObjects) ((void)(maxObjects), true)
#define C2D_Fini() ((void)0)
#define C2D_Prepare() ((void)0)
#define C2D_CreateScreenTarget(screen, side) ((C3D_RenderTarget *)"target")
#define C2D_TargetClear(target, color) ((void)(target), (void)(color))
#define C2D_SceneBegin(target) ((void)(target))
// A buffer's value is its capacity, so a test can tell the frame's buffer from the small one that
// ui.c uses for measuring.
#define C2D_TextBufNew(glyphs) ((C2D_TextBuf)(size_t)(glyphs))
#define C2D_TextBufClear(buf) ((void)(buf))
#define C2D_TextBufDelete(buf) ((void)(buf))
#define C2D_TextOptimize(text) ((void)(text))
#define C2D_DrawRectSolid(x, y, z, w, h, c) ((void)(w), (void)(c))

#endif // TEST_STUBS_CITRO2D_H
