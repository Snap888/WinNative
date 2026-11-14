#include "anr_frame_lab.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <stdint.h>
#define ANR_FL_MAGIC 0x31464e41u
#define ANR_FL_MAX_PAYLOAD (256u * 1024u * 1024u)
typedef struct frame_index { uint32_t magic,stage,width,height,format,reserved; uint64_t frame_id,timestamp_ns,data_offset,data_bytes,params_offset,params_bytes; } frame_index;
struct anr_fl_replay_session { FILE *index_file,*frames_file,*params_file; };
static int seek_to(FILE *f,uint64_t offset){ if(offset>(uint64_t)INT64_MAX)return EOVERFLOW; return fseeko(f,(off_t)offset,SEEK_SET)==0?0:(errno?errno:EIO); }
int anr_fl_replay_open(const char *dir,anr_fl_replay_session **out){ if(!dir||!out)return EINVAL; *out=NULL; anr_fl_replay_session*s=calloc(1,sizeof(*s)); if(!s)return ENOMEM; char p[1024];
#define OPEN(member,name) do{if(snprintf(p,sizeof(p),"%s/%s",dir,name)>=(int)sizeof(p)){anr_fl_replay_close(s);return ENAMETOOLONG;} s->member=fopen(p,"rb"); if(!s->member){int e=errno?errno:EIO;anr_fl_replay_close(s);return e;}}while(0)
OPEN(index_file,"frames.idx"); OPEN(frames_file,"frames.bin"); OPEN(params_file,"params.bin");
#undef OPEN
*out=s;return 0; }
int anr_fl_replay_next(anr_fl_replay_session*s,anr_fl_replay_frame*out){if(!s||!out)return -EINVAL;memset(out,0,sizeof(*out));frame_index x;size_t n=fread(&x,1,sizeof(x),s->index_file);if(!n&&feof(s->index_file))return 0;if(n!=sizeof(x))return -(errno?errno:EIO);if(x.magic!=ANR_FL_MAGIC||x.stage<ANR_FL_STAGE_SOURCE||x.stage>ANR_FL_STAGE_FINAL_OUTPUT||!x.width||!x.height||!x.data_bytes||x.data_bytes>ANR_FL_MAX_PAYLOAD||x.params_bytes>1024u*1024u)return -EINVAL;out->pixels=malloc((size_t)x.data_bytes);out->params=x.params_bytes?malloc((size_t)x.params_bytes):NULL;if(!out->pixels||(x.params_bytes&&!out->params)){anr_fl_replay_frame_release(out);return -ENOMEM;}int e=seek_to(s->frames_file,x.data_offset);if(!e&&fread(out->pixels,1,(size_t)x.data_bytes,s->frames_file)!=x.data_bytes)e=errno?errno:EIO;if(!e&&x.params_bytes){e=seek_to(s->params_file,x.params_offset);if(!e&&fread(out->params,1,(size_t)x.params_bytes,s->params_file)!=x.params_bytes)e=errno?errno:EIO;}if(e){anr_fl_replay_frame_release(out);return -e;}out->stage=x.stage;out->frame_id=x.frame_id;out->timestamp_ns=x.timestamp_ns;out->width=x.width;out->height=x.height;out->format=x.format;out->pixel_bytes=(size_t)x.data_bytes;out->params_bytes=(size_t)x.params_bytes;return 1;}
void anr_fl_replay_frame_release(anr_fl_replay_frame*f){if(!f)return;free(f->pixels);free(f->params);memset(f,0,sizeof(*f));}
void anr_fl_replay_close(anr_fl_replay_session*s){if(!s)return;if(s->params_file)fclose(s->params_file);if(s->frames_file)fclose(s->frames_file);if(s->index_file)fclose(s->index_file);free(s);}
