/* NDK/API test: app_process must never own a consumer's settings directory. */
#include "host_paths.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  char storage[]="/data/local/tmp/snes-paths-XXXXXX", path[1024], cwd[1024];
  assert(mkdtemp(storage));
  assert(setenv("SNESRECOMP_DATA_DIR",storage,1)==0);
  assert(snesrecomp_exe_dir_path("config.ini",path,sizeof(path)));
  assert(strncmp(path,storage,strlen(storage))==0);
  assert(strcmp(path+strlen(storage),"/config.ini")==0);
  assert(snesrecomp_anchor_to_exe_dir());
  assert(getcwd(cwd,sizeof(cwd)) && strcmp(cwd,storage)==0);
  assert(chdir("/data/local/tmp")==0);
  assert(rmdir(storage)==0);
  puts("PASS: Android paths anchor config/saves to writable app storage");
  return 0;
}
