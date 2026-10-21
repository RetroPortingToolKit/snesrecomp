#include "diagnostics/trace_writer.h"
#include "desktop/sdl_compat.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define writer_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define writer_mkdir(path) mkdir(path, 0755)
#endif

enum { ROW_BYTES = 8192, QUEUE_BYTES = 4*1024*1024, FILE_BUFFER_BYTES = 65536 };
enum { WRITER_OK, WRITER_FULL, WRITER_ROW, WRITER_IO };
struct SnesTraceWriter {
  SDL_Thread *thread;
  SDL_atomic_t stop, error;
  SDL_atomic_t read_pos, write_pos;
  unsigned head; /* Producer only. */
  char stem[2048];
  char *header;
  size_t segment_bytes;
  unsigned keep;
  unsigned char bytes[QUEUE_BYTES];
};

static void ring_write(SnesTraceWriter *w,unsigned at,const void *source,size_t size) {
  size_t first=QUEUE_BYTES-at;
  if(first>size) first=size;
  memcpy(w->bytes+at,source,first);
  memcpy(w->bytes,(const unsigned char *)source+first,size-first);
}
static void ring_read(SnesTraceWriter *w,unsigned at,void *dest,size_t size) {
  size_t first=QUEUE_BYTES-at;
  if(first>size) first=size;
  memcpy(dest,w->bytes+at,first);
  memcpy((unsigned char *)dest+first,w->bytes,size-first);
}

static void writer_error(SnesTraceWriter *w, int error) {
  /* Preserve the first cause; a full queue can still drain its valid prefix. */
  SDL_AtomicCAS(&w->error, WRITER_OK, error);
}
static const char *error_text(int error) {
  switch(error) {
    case WRITER_FULL: return "disk writer fell behind; bounded queue full";
    case WRITER_ROW: return "diagnostic row exceeds its bounded buffer";
    case WRITER_IO: return "cannot write diagnostic file";
    default: return NULL;
  }
}
static FILE *open_segment(SnesTraceWriter *w, unsigned segment,
                          char buffer[FILE_BUFFER_BYTES], size_t *bytes) {
  char path[2064];
  if(segment>w->keep) {
    snprintf(path,sizeof(path),"%s-%03u.csv",w->stem,segment-w->keep);
    if(remove(path) && errno!=ENOENT) return NULL;
  }
  snprintf(path,sizeof(path),"%s-%03u.csv",w->stem,segment);
  FILE *file=fopen(path,"wb");
  if(!file) return NULL;
  if(setvbuf(file,buffer,_IOFBF,FILE_BUFFER_BYTES)) { fclose(file); return NULL; }
  *bytes=strlen(w->header);
  if(fwrite(w->header,1,*bytes,file)!=*bytes) { fclose(file); return NULL; }
  return file;
}
static int SDLCALL writer_main(void *opaque) {
  SnesTraceWriter *w=opaque;
  char directory[2048];
  snprintf(directory,sizeof(directory),"%s",w->stem);
  char *slash=strrchr(directory,'/'), *backslash=strrchr(directory,'\\');
  if(backslash && (!slash || backslash>slash)) slash=backslash;
  if(slash) { *slash=0; writer_mkdir(directory); }
  char buffer[FILE_BUFFER_BYTES];
  size_t bytes=0;
  unsigned segment=1, tail=0;
  FILE *file=open_segment(w,segment,buffer,&bytes);
  if(!file) { writer_error(w,WRITER_IO); return 1; }
  uint32_t last_flush=(uint32_t)SDL_GetTicks();
  bool dirty=true;
#ifdef SNES_TRACE_WRITER_TEST
  /* Test-only gate models a blocked sink without depending on real disk lag. */
  extern void SnesTraceWriterTestGate(void);
  SnesTraceWriterTestGate();
#endif
  for(;;) {
    if((unsigned)SDL_AtomicGet(&w->write_pos)!=tail) {
      uint32_t size;
      char row[ROW_BYTES];
      ring_read(w,tail,&size,sizeof(size));
      if(!size || size>ROW_BYTES) { writer_error(w,WRITER_ROW); break; }
      ring_read(w,(tail+sizeof(size))%QUEUE_BYTES,row,size);
      tail=(tail+sizeof(size)+size)%QUEUE_BYTES;
      /* Publish freed space only after copying the complete row. The worker
       * then owns its local copy while slow disk I/O runs independently. */
      SDL_AtomicSet(&w->read_pos,(int)tail);
      if(bytes>=w->segment_bytes) {
        if(fclose(file)) { file=NULL; writer_error(w,WRITER_IO); break; }
        file=open_segment(w,++segment,buffer,&bytes);
        if(!file) { writer_error(w,WRITER_IO); break; }
      }
      if(fwrite(row,1,size,file)!=size) {
        writer_error(w,WRITER_IO); break;
      }
      bytes+=size; dirty=true;
    } else {
      if(SDL_AtomicGet(&w->stop) || SDL_AtomicGet(&w->error)) {
        /* Close can race the first empty-slot read. Its stop publication
         * follows the final row publication, so check the slot again. */
        if((unsigned)SDL_AtomicGet(&w->write_pos)==tail) break;
        continue;
      }
      SDL_Delay(2);
    }
    uint32_t now=(uint32_t)SDL_GetTicks();
    if(dirty && now-last_flush>=1000) {
      if(fflush(file)) { writer_error(w,WRITER_IO); break; }
      dirty=false; last_flush=now;
    }
  }
  if(file && fclose(file)) writer_error(w,WRITER_IO);
  return SDL_AtomicGet(&w->error)!=WRITER_OK;
}

SnesTraceWriter *SnesTraceWriterOpen(const char *stem,const char *header,
                                           size_t segment_bytes,unsigned keep) {
  if(!stem || !header || !segment_bytes || !keep) return NULL;
  SnesTraceWriter *w=calloc(1,sizeof(*w));
  if(!w) return NULL;
  if(strlen(stem)>=sizeof(w->stem)) { free(w); return NULL; }
  strcpy(w->stem,stem);
  w->header=malloc(strlen(header)+1);
  if(!w->header) { free(w); return NULL; }
  strcpy(w->header,header);
  w->segment_bytes=segment_bytes; w->keep=keep;
  w->thread=SDL_CreateThread(writer_main,"snes-trace-writer",w);
  if(!w->thread) { free(w->header); free(w); return NULL; }
  return w;
}
bool SnesTraceWriterWrite(SnesTraceWriter *w,const char *row,size_t size) {
  if(!w || SDL_AtomicGet(&w->error) || SDL_AtomicGet(&w->stop)) return false;
  if(!row || !size || size>ROW_BYTES) { writer_error(w,WRITER_ROW); return false; }
  unsigned tail=(unsigned)SDL_AtomicGet(&w->read_pos);
  size_t free_bytes=(tail+QUEUE_BYTES-w->head-1)%QUEUE_BYTES;
  uint32_t length=(uint32_t)size;
  if(size+sizeof(length)>free_bytes) { writer_error(w,WRITER_FULL); return false; }
  ring_write(w,w->head,&length,sizeof(length));
  ring_write(w,(w->head+sizeof(length))%QUEUE_BYTES,row,size);
  w->head=(w->head+sizeof(length)+size)%QUEUE_BYTES;
  /* SDL's atomic publication supplies the memory barrier on all SDL targets. */
  SDL_AtomicSet(&w->write_pos,(int)w->head);
  return true;
}
const char *SnesTraceWriterError(SnesTraceWriter *w) {
  return w?error_text(SDL_AtomicGet(&w->error)):NULL;
}
const char *SnesTraceWriterClose(SnesTraceWriter *w) {
  if(!w) return NULL;
  SDL_AtomicSet(&w->stop,1);
  SDL_WaitThread(w->thread,NULL);
  const char *error=SnesTraceWriterError(w);
  free(w->header); free(w);
  return error;
}
