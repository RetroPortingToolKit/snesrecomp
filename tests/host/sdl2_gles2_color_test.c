/* Actual hidden SDL2 GLES2 upload/readback, including zero-alpha SNES RGB. */
#define SDL_MAIN_HANDLED
#define SNESRECOMP_SDL3 0
#define SNESRECOMP_SDL_GLES2_ONLY 1
#include "desktop/sdl_compat.h"
#include <stdio.h>
#include <string.h>

int main(void) {
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
  }
  SDL_Window *window = SDL_CreateWindow("renderer fixture", 0, 0, 8, 8, SDL_WINDOW_HIDDEN);
  if (!window) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
  SDL_Renderer *renderer = snesrecomp_sdl_create_renderer_named(window, "opengles2", 0);
  if (!renderer) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
  const char *driver = snesrecomp_sdl_renderer_name(renderer);
  if (!driver || strcmp(driver, "opengles2")) return 1;
  printf("video=%s renderer=%s\n", SDL_GetCurrentVideoDriver(), driver);
  const Uint32 colors[] = {0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff,
                           0x00000000, 0x00112233, 0x007f8040, 0x00abcdef};
  int failures = 0;
  for (int width = 2; width <= 8; width *= 2) {
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, width, 8);
    if (!texture) { fprintf(stderr, "texture: %s\n", SDL_GetError()); return 1; }
    snesrecomp_sdl_set_texture_opaque(texture);
    snesrecomp_sdl_set_texture_linear(texture, false);
    Uint32 pixels[64], actual[64];
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < width; x++) pixels[y * width + x] = colors[y];
    if (SDL_UpdateTexture(texture, NULL, pixels, width * 4) ||
        SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255) ||
        SDL_RenderClear(renderer) || SDL_RenderCopy(renderer, texture, NULL, NULL) ||
        SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, actual, 8 * 4)) {
      fprintf(stderr, "upload/readback: %s\n", SDL_GetError()); return 1;
    }
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
      if ((actual[y * 8 + x] & 0x00ffffff) != colors[y]) {
        fprintf(stderr, "width=%d x=%d y=%d actual=%08x expected=%08x\n",
                width, x, y, actual[y * 8 + x], colors[y]); failures++;
      }
    }
    SDL_DestroyTexture(texture);
  }
  SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
  printf("192 uploaded/scaled RGB pixel checks: %s\n", failures ? "FAIL" : "PASS");
  return failures != 0;
}
