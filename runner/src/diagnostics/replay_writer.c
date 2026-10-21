#include "diagnostics/replay_writer.h"
#include "desktop/sdl_compat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { QUEUE_BYTES=16*1024*1024, FILE_BUFFER_BYTES=65536 };
enum { BEGIN, SNAPSHOT, LINE, FRAME, REMOVE, END };
enum { OK, FULL, INVALID, IO };
typedef struct Command { uint32_t op, size, frame, sizes[4]; } Command;
typedef struct Begin {
  char path[1280], header[512], config[1200], mods[1200], effective[16384];
} Begin;
struct SnesReplayWriter {
  SDL_Thread *thread;
  SDL_atomic_t stop, error, read_pos, write_pos;
  unsigned head; /* Producer only. */
  unsigned char bytes[QUEUE_BYTES];
};
static void fail(SnesReplayWriter *w,int error) { SDL_AtomicCAS(&w->error,OK,error); }
static void put(SnesReplayWriter *w,unsigned at,const void *data,size_t n) {
  size_t first=QUEUE_BYTES-at;if(first>n)first=n;
  if(first)memcpy(w->bytes+at,data,first);
  if(n>first)memcpy(w->bytes,(const char *)data+first,n-first);
}
static void get(SnesReplayWriter *w,unsigned at,void *data,size_t n) {
  size_t first=QUEUE_BYTES-at;if(first>n)first=n;
  if(first)memcpy(data,w->bytes+at,first);
  if(n>first)memcpy((char *)data+first,w->bytes,n-first);
}
static bool enqueue(SnesReplayWriter *w,Command c,const void *const *data,unsigned parts) {
  if(!w || SDL_AtomicGet(&w->stop) || SDL_AtomicGet(&w->error))return false;
  unsigned tail=(unsigned)SDL_AtomicGet(&w->read_pos);
  if(sizeof(c)+c.size>=(size_t)QUEUE_BYTES) {fail(w,INVALID);return false;}
  if(sizeof(c)+c.size>(tail+QUEUE_BYTES-w->head-1)%QUEUE_BYTES) {fail(w,FULL);return false;}
  unsigned at=w->head;put(w,at,&c,sizeof(c));at=(at+sizeof(c))%QUEUE_BYTES;
  for(unsigned i=0;i<parts;++i) {
    if(c.sizes[i] && !data[i]) {fail(w,INVALID);return false;}
    put(w,at,data[i],c.sizes[i]);at=(at+c.sizes[i])%QUEUE_BYTES;
  }
  w->head=at;
  /* SDL atomics publish all payload bytes before the consumer sees the head. */
  SDL_AtomicSet(&w->write_pos,(int)at);return true;
}
static bool one(SnesReplayWriter *w,unsigned op,const void *data,size_t n) {
  if(n>=QUEUE_BYTES) {if(w)fail(w,INVALID);return false;}
  Command c={op,(uint32_t)n,0,{(uint32_t)n,0,0,0}};
  return enqueue(w,c,&data,1);
}
static uint32_t digest(SnesReplayWriter *w,unsigned at,size_t n) {
  uint32_t h=2166136261u;
  while(n) {
    size_t count=QUEUE_BYTES-at;if(count>n)count=n;
    const unsigned char *p=w->bytes+at;
    for(size_t i=0;i<count;++i)h=(h^p[i])*16777619u;
    n-=count;at=0;
  }
  return h;
}
static bool write_ring(SnesReplayWriter *w,FILE *f,unsigned at,size_t n) {
  size_t first=QUEUE_BYTES-at;if(first>n)first=n;
  return f && fwrite(w->bytes+at,1,first,f)==first &&
      (n==first || fwrite(w->bytes,1,n-first,f)==n-first);
}
static bool close_files(FILE **text,FILE **states) {
  bool ok=true;
  if(*states && fclose(*states))ok=false;
  if(*text && fclose(*text))ok=false;
  *text=*states=NULL;return ok;
}
static bool copy_optional(const char *source,const char *dest) {
  FILE *in=fopen(source,"rb");
  if(!in)return errno==ENOENT;
  FILE *out=fopen(dest,"wb");bool ok=out!=NULL;
  char bytes[8192];size_t n;
  while(ok && (n=fread(bytes,1,sizeof(bytes),in)))ok=fwrite(bytes,1,n,out)==n;
  if(ferror(in))ok=false;
  if(fclose(in))ok=false;
  if(out && fclose(out))ok=false;
  return ok;
}
static int SDLCALL worker(void *opaque) {
  SnesReplayWriter *w=opaque;
  FILE *text=NULL,*states=NULL;
  char text_buffer[FILE_BUFFER_BYTES],state_buffer[FILE_BUFFER_BYTES];
  unsigned tail=0;size_t offset=0;
  uint32_t last_flush=(uint32_t)SDL_GetTicks();bool dirty=false;
#ifdef SNES_REPLAY_WRITER_TEST
  extern void SnesReplayWriterTestGate(void);SnesReplayWriterTestGate();
#endif
  for(;;) {
    if((unsigned)SDL_AtomicGet(&w->write_pos)!=tail) {
      Command c;get(w,tail,&c,sizeof(c));unsigned at=(tail+sizeof(c))%QUEUE_BYTES;
      bool ok=true;
      switch(c.op) {
      case BEGIN: {
        Begin b;get(w,at,&b,sizeof(b));char path[1400];
        ok=close_files(&text,&states);offset=0;
        if(!ok)break;
        text=fopen(b.path,"wb");snprintf(path,sizeof(path),"%s.sav",b.path);states=fopen(path,"wb");
        ok=text && states && !setvbuf(text,text_buffer,_IOFBF,sizeof(text_buffer)) &&
            !setvbuf(states,state_buffer,_IOFBF,sizeof(state_buffer));
        if(!ok)break;
        ok=fputs(b.header,text)!=EOF;
        snprintf(path,sizeof(path),"%s.config.ini",b.path);ok=copy_optional(b.config,path)&&ok;
        snprintf(path,sizeof(path),"%s.mods.toml",b.path);ok=copy_optional(b.mods,path)&&ok;
        snprintf(path,sizeof(path),"%s.mods.txt",b.path);
        FILE *selected=fopen(path,"wb");
        if(!selected)ok=false;
        else {if(fputs(b.effective,selected)==EOF)ok=false;if(fclose(selected))ok=false;}
        break;
      }
      case SNAPSHOT:
        ok=write_ring(w,states,at,c.size);
        if(ok)ok=text && fprintf(text,"S %zu %u %08x\n",offset,c.size,digest(w,at,c.size))>=0;
        offset+=c.size;break;
      case LINE:ok=write_ring(w,text,at,c.size);break;
      case FRAME: {
        uint32_t hashes[4];
        for(unsigned i=0;i<4;++i) {hashes[i]=digest(w,at,c.sizes[i]);at=(at+c.sizes[i])%QUEUE_BYTES;}
        ok=text && fprintf(text,"H %u %08x %08x %08x %08x\n",c.frame,hashes[0],hashes[1],hashes[2],hashes[3])>=0;
        break;
      }
      case REMOVE: {
        char path[1400];get(w,at,path,c.size);
        ok=!remove(path) || errno==ENOENT;break;
      }
      case END:ok=close_files(&text,&states);break;
      default:ok=false;break;
      }
      if(!ok) {fail(w,IO);break;}
      /* Keep the payload owned until disk I/O and hashing finish. The producer
       * stops at capacity instead of waiting for this publication. */
      tail=(tail+sizeof(c)+c.size)%QUEUE_BYTES;SDL_AtomicSet(&w->read_pos,(int)tail);
      dirty=text!=NULL;
    } else {
      if(SDL_AtomicGet(&w->stop) || SDL_AtomicGet(&w->error)) {
        if((unsigned)SDL_AtomicGet(&w->write_pos)==tail)break;
        continue;
      }
      SDL_Delay(2);
    }
    uint32_t now=(uint32_t)SDL_GetTicks();
    if(dirty && now-last_flush>=100) {
      /* Sidecars reach stdio's sink before records that refer to them. A
       * hung simulation still leaves its last queued input in this flush. */
      if(fflush(states) || fflush(text)) {fail(w,IO);break;}
      last_flush=now;dirty=false;
    }
  }
  if(!close_files(&text,&states))fail(w,IO);
  return SDL_AtomicGet(&w->error)!=OK;
}
SnesReplayWriter *SnesReplayWriterCreate(void) {
  SnesReplayWriter *w=calloc(1,sizeof(*w));if(!w)return NULL;
  w->thread=SDL_CreateThread(worker,"snes-replay-writer",w);
  if(!w->thread) {free(w);return NULL;}return w;
}
bool SnesReplayWriterBegin(SnesReplayWriter *w,const char *path,const char *header,
    const char *config,const char *mods,const char *effective) {
  Begin b={0};
#define FIELD(name) if(!name || strlen(name)>=sizeof(b.name)) {if(w)fail(w,INVALID);return false;} else strcpy(b.name,name)
  FIELD(path);FIELD(header);FIELD(config);FIELD(mods);FIELD(effective);
#undef FIELD
  return one(w,BEGIN,&b,sizeof(b));
}
bool SnesReplayWriterSnapshot(SnesReplayWriter *w,const void *p,size_t n) {return one(w,SNAPSHOT,p,n);}
bool SnesReplayWriterLine(SnesReplayWriter *w,const char *line) {return line && one(w,LINE,line,strlen(line));}
bool SnesReplayWriterFrame(SnesReplayWriter *w,unsigned frame,
    const void *a,size_t na,const void *b,size_t nb,const void *c,size_t nc,const void *d,size_t nd) {
  if(na>=QUEUE_BYTES || nb>=QUEUE_BYTES || nc>=QUEUE_BYTES || nd>=QUEUE_BYTES) {
    if(w)fail(w,INVALID);return false;
  }
  Command cmd={FRAME,(uint32_t)(na+nb+nc+nd),frame,{(uint32_t)na,(uint32_t)nb,(uint32_t)nc,(uint32_t)nd}};
  const void *parts[]={a,b,c,d};return enqueue(w,cmd,parts,4);
}
bool SnesReplayWriterRemove(SnesReplayWriter *w,const char *path) {
  if(!path || strlen(path)>=1400) {if(w)fail(w,INVALID);return false;}
  return one(w,REMOVE,path,strlen(path)+1);
}
bool SnesReplayWriterEnd(SnesReplayWriter *w) {return one(w,END,NULL,0);}
const char *SnesReplayWriterError(SnesReplayWriter *w) {
  switch(w?SDL_AtomicGet(&w->error):OK) {
    case FULL:return "replay disk writer fell behind; bounded queue full";
    case INVALID:return "invalid or oversized replay write";
    case IO:return "replay file write failed";
    default:return NULL;
  }
}
const char *SnesReplayWriterDestroy(SnesReplayWriter *w) {
  if(!w)return NULL;
  SDL_AtomicSet(&w->stop,1);SDL_WaitThread(w->thread,NULL);
  const char *error=SnesReplayWriterError(w);free(w);return error;
}
