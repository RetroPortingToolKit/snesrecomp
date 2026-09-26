#include "data_pack.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
static void error(void *u,const char *path,const char *message) {
  (void)u;(void)path;printf("ERROR\t%s\n",message);
}
int main(int argc,char **argv) {
  if(argc<2)return 2;
  uint8_t base[32]={0};
  const char *caps[]={"test.known"};
  SnesDataPacks *p=snes_data_packs_scan(argv[1],"test-game","test.data",base,caps,1,error,NULL);
  for(size_t i=0;i<snes_data_packs_count(p);++i) {
    const SnesDataPack *pack=snes_data_packs_get(p,i);
    printf("PACK\t%s\t%zu\n",pack->id,pack->payload_size);
    if(argc>2) {
      uint8_t *bytes=NULL;size_t size=0;
      if(snes_data_pack_read(p,i,argv[2],16,&bytes,&size,error,NULL)) {
        printf("ASSET\t%zu\n",size);free(bytes);
      }
    }
  }
  snes_data_packs_destroy(p);
  return 0;
}
