/* Mobile presentation uses SDL's GLES2 renderer; desktop-only GL hooks are inert. */
#include "util.h"
#include "desktop/opengl.h"
#include <string.h>
void OpenGLRenderer_Create(struct RendererFuncs *funcs) { memset(funcs,0,sizeof(*funcs)); }
void snesrecomp_opengl_set_vsync(int interval) { (void)interval; }
void snesrecomp_opengl_set_viewport(void (*compute)(int,int,int,int,SnesDisplayViewport *)) { (void)compute; }
void OpenGLRenderer_RequestScreenshot(void) { }
