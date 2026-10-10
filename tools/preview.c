/* Linux preview: dumps N frames of a state as raw ARGB to stdout.
 * Authors: Jeffrey Landgrave, Claude (Anthropic). License: MIT. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/pet_draw.h"
int main(int c, char **v){
  int st=atoi(v[1]), n=atoi(v[2]); uint32_t cv[CW*CH];
  for(int i=0;i<n;i++){ DrawIn in={st,i,i,1,(i%120)>110,0,0,i/7,(i<n/2)?-1:1,0};
    if(st==ST_IDLE){ in.lookX=((i/40)%3)-1; }
    pet_draw(cv,&in); fwrite(cv,4,CW*CH,stdout);}
  return 0;}
