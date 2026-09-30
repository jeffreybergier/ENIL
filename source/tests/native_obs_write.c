/* Exercise the actual OBS byte publisher with a delayed close failure. */
#include "enil_obs.c"
#include <assert.h>
#include <dirent.h>
static int fail_close;
int __real_fclose(FILE *stream);
int __wrap_fclose(FILE *stream) {
  int rc = __real_fclose(stream);
  return fail_close ? EOF : rc;
}
void enil_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int main(int argc, char **argv) {
  char path[1024], data[4] = {0};
  FILE *f;
  DIR *dir;
  struct dirent *entry;
  assert(argc == 2);
  snprintf(path,sizeof(path),"%s/photo",argv[1]);
  assert(write_bytes(path,(const unsigned char *)"old",3)==0);
  fail_close=1;
  assert(write_bytes(path,(const unsigned char *)"new",3)<0);
  fail_close=0;
  f=fopen(path,"rb"); assert(f && fread(data,1,3,f)==3); fclose(f);
  assert(!strcmp(data,"old"));
  assert(write_bytes(path,(const unsigned char *)"new",3)==0);
  f=fopen(path,"rb"); assert(f && fread(data,1,3,f)==3); fclose(f);
  assert(!strcmp(data,"new"));
  assert(write_bytes(path,(const unsigned char *)"",0)<0);
  dir=opendir(argv[1]); assert(dir);
  while ((entry=readdir(dir))) assert(!strstr(entry->d_name,".download-"));
  closedir(dir);
  return 0;
}
