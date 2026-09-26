/* Namespace switches must retain the prior SRAM when publication fails. */
#include "common_rtl.h"
#include <assert.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p,0755)
#define RMDIR(p) rmdir(p)
#endif
static void expect(const char *path, int value) {
  FILE *f=fopen(path,"rb"); assert(f);
  for(int i=0;i<4;++i)assert(fgetc(f)==value);
  assert(fgetc(f)==EOF); assert(!fclose(f));
}
int main(void) {
  uint8 bytes[4]={1,1,1,1};g_sram=bytes;g_sram_size=4;
  RtlSetSaveRoot("sram-test");MKDIR("sram-test");
  remove("sram-test/save.srm");remove("sram-test/save.srm.bak");
  assert(RtlTryWriteSram());expect("sram-test/save.srm",1);
  memset(bytes,2,4);assert(RtlTryWriteSram());
  expect("sram-test/save.srm",2);expect("sram-test/save.srm.bak",1);
  MKDIR("sram-test/save.srm.tmp");memset(bytes,3,4);
  assert(!RtlTryWriteSram());expect("sram-test/save.srm",2);expect("sram-test/save.srm.bak",1);
  RMDIR("sram-test/save.srm.tmp");
  RtlWriteSram();expect("sram-test/save.srm",3);expect("sram-test/save.srm.bak",2);
  remove("sram-test/save.srm");remove("sram-test/save.srm.bak");RMDIR("sram-test");
  puts("SRAM publication, backup and failed-write preservation passed");return 0;
}
