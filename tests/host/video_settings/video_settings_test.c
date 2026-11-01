#include "desktop/launcher_video.h"
#include "desktop/local_views.h"
#include <assert.h>
#include <math.h>
#include <string.h>

int main(void) {
  /* Both seats publish atomically, including padded pitches and canonical
   * shared-screen fallback. Invalid geometry must leave the output alone. */
  uint32_t left[6] = {1, 2, 99, 3, 4, 99};
  uint32_t right[4] = {5, 6, 7, 8};
  uint32_t pair[10];
  memset(pair, 0xcc, sizeof(pair));
  assert(SnesLocalViews_ComposeSideBySide(pair, 20, 2, 2, left, 12, right, 8));
  assert(pair[0] == 1 && pair[1] == 2 && pair[2] == 5 && pair[3] == 6);
  assert(pair[5] == 3 && pair[6] == 4 && pair[7] == 7 && pair[8] == 8);
  assert(pair[4] == 0xcccccccc && pair[9] == 0xcccccccc);
  assert(SnesLocalViews_ComposeSideBySide(pair, 20, 2, 2, left, 12, NULL, 0));
  assert(pair[2] == 1 && pair[3] == 2 && pair[7] == 3 && pair[8] == 4);
  assert(!SnesLocalViews_ComposeSideBySide(pair, 12, 2, 2, left, 12, right, 8));
  assert(!SnesLocalViews_ComposeSideBySide(pair, 20, 1025, 2, left, 12, right, 8));
  SnesDisplayViewport panes = SnesLocalViews_Viewport(5.0 / 4, 1600, 900);
  assert(panes.width == 1600 && panes.height == 640 && panes.y == 130);
  panes = SnesLocalViews_Viewport(16.0 / 9, 1920, 1080);
  assert(panes.width == 1920 && panes.height == 540 && panes.y == 270);
  SnesDisplayFrame pane=SnesLocalViews_ComputePane(256,224,5.0/4,16.0/9);
  assert(pane.width==364 && pane.extra==54 && pane.aspect==16.0/9);
  pane=SnesLocalViews_ComputePane(256,224,5.0/4,.5);
  assert(pane.width==256 && pane.extra==0 && pane.aspect==5.0/4);
  pane=SnesLocalViews_ComputePane(256,224,5.0/4,1000);
  assert(pane.width==1024 && pane.extra==384 && pane.aspect==5);
  const int widths[][3] = {{342, 448, 682}, {398, 522, 796}, {456, 598, 910}};
  const int ratios[] = {16, 21, 32};
  const double pixel_aspects[] = {7.0 / 6, 1, 7.0 / 8};
  for (int setting = 0; setting < kSnesDisplayAspect_Count; ++setting) {
    SnesDisplayAspect aspect = (SnesDisplayAspect)setting;
    uint8_t parsed = 99;
    assert(SnesDisplayAspect_Parse(SnesDisplayAspect_Name(setting), &parsed));
    assert(parsed == setting);
    for (int i = 0; i < 3; ++i) {
      SnesDisplayFrame f = SnesDisplayAspect_ComputeAdaptiveFrame(256, 224, 1024,
          ratios[i] / 9.0, aspect);
      assert(f.width == widths[setting][i] && f.extra * 2 + 256 == f.width);
      SnesDisplayViewport dst = SnesDisplayAspect_FitViewport(f.aspect, ratios[i] * 120, 1080);
      assert(dst.width == ratios[i] * 120 && dst.height == 1080);
      assert(fabs((double)dst.width * 224 / (dst.height * f.width) - pixel_aspects[setting]) < .006);
    }
    SnesDisplayFrame native = SnesDisplayAspect_ComputeAdaptiveFrame(256, 224, 1024, .1, aspect);
    assert(native.width == 256 && fabs(native.aspect - 256.0 / 224 * pixel_aspects[setting]) < 1e-9);
    SnesDisplayFrame cap = SnesDisplayAspect_ComputeAdaptiveFrame(256, 224, 1024, 1e10, aspect);
    assert(cap.width == 1024 && fabs(cap.aspect - 1024.0 / 224 * pixel_aspects[setting]) < 1e-9);
    assert(SnesDisplayAspect_ComputeAdaptiveFrame(256, 224, 256, 16.0 / 9, aspect).width == 256);
  }
  uint8_t parsed = 2;
  assert(!SnesDisplayAspect_Parse("8:9", &parsed) && parsed == 2);
  assert(SnesDisplayAspect_Parse("sQuArEpIxElS", &parsed) && parsed == 1);
  assert(SnesDisplayAspect_Parse("CRT", &parsed) && parsed == 0);
  assert(SnesDisplayAspect_ComputeAdaptiveFrame(256, 224, 1024, NAN, 0).width == 256);

  RecompLauncherCSettings settings = {0};
  RecompLauncherCGameInfo game = {0};
  SnesLauncherVideo_Configure(&settings, &game, 0, 0, 1, "unused.glslp");
  assert(!game.has_shader && !game.has_snes_display_aspect && !game.aspect_labels);
  assert(!settings.shader_path[0] && settings.aspect_index == 0);
  SnesLauncherVideo_Configure(&settings, &game, 1, 1, 1, "assets/shaders/crt-soft.glslp");
  assert(game.has_shader && game.has_snes_display_aspect && game.num_aspect_labels == 3);
  assert(settings.aspect_index == 1 && !strcmp(settings.shader_path, "assets/shaders/crt-soft.glslp"));
  assert(strstr(game.aspect_setting_help, "adaptive widescreen"));
  return 0;
}
