/* Offline line-oriented test harness, never a network listener. Each input is
 * {"operation":"/sign", "requestJson":"{...}"}. The string envelope avoids
 * cJSON number reserialization rounding before testing the public API.
 * Only use synthetic fixtures: stdout deliberately contains test results. */
#include "enil_ltsm.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  size_t capacity=4096,length=0;
  char *line=(char *)malloc(capacity); int ch;
  if(!line) return 2;
  while((ch=getchar())!=EOF) {
    if(ch!='\n') {
      if(length>=9U*1024U*1024U) { free(line); return 2; }
      if(length+1>=capacity) {
        char *next; capacity*=2; next=(char *)realloc(line,capacity);
        if(!next) { free(line); return 2; } line=next;
      }
      line[length++]=(char)ch; continue;
    }
    line[length]=0;
    {
      cJSON *input=cJSON_Parse(line),*op,*request,*out=cJSON_CreateObject();
      char *json=NULL,*response=NULL,*encoded; char error[160]="invalid harness input";
      int ok=0;
      op=cJSON_GetObjectItemCaseSensitive(input,"operation");
      request=cJSON_GetObjectItemCaseSensitive(input,"requestJson");
      if(cJSON_IsString(op) && cJSON_IsString(request)) {
        json=(char *)malloc(strlen(request->valuestring)+1);
        if(json) strcpy(json,request->valuestring);
        if(json) ok=enil_ltsm_request(op->valuestring,json,&response,error,sizeof(error));
      }
      cJSON_AddBoolToObject(out,"ok",ok);
      if(ok) cJSON_AddItemToObject(out,"response",cJSON_Parse(response));
      else cJSON_AddStringToObject(out,"error",error);
      encoded=cJSON_PrintUnformatted(out);
      ok=encoded!=NULL;
      if(ok) { puts(encoded); fflush(stdout); }
      enil_ltsm_free(encoded); enil_ltsm_free(response); enil_ltsm_free(json);
      cJSON_Delete(input); cJSON_Delete(out);
      if(!ok) { free(line); return 2; }
    }
    memset(line,0,length); length=0;
  }
  free(line); return length?2:0;
}
