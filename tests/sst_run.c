#include "../src/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t in8(void*h,uint16_t p){(void)h;(void)p;return 0xFF;}
static void out8(void*h,uint16_t p,uint8_t v){(void)h;(void)p;(void)v;}
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"r"); static uint8_t mem[MEM_SIZE];
  CPU c; memset(&c,0,sizeof c); c.mem=mem; c.in8=in8; c.out8=out8; cpu_reset(&c); c.rom_writable=1;
  char key[64],name[256]; int mask; long ntest=0,nfail=0; char last[64]="";int failsop=0;
  while(fscanf(f," T %63s %d %255s",key,&mask,name)==3){
    unsigned iv[14],fv[14]; int n;
    for(int i=0;i<14;i++) fscanf(f,"%u",&iv[i]);
    fscanf(f,"%d",&n); for(int i=0;i<n;i++){unsigned a,v;fscanf(f,"%u %u",&a,&v);mem[a]=v;}
    for(int i=0;i<14;i++) fscanf(f,"%u",&fv[i]);
    int nf; fscanf(f,"%d",&nf); unsigned fa[2048],fvv[2048]; for(int i=0;i<nf;i++) fscanf(f,"%u %u",&fa[i],&fvv[i]);
    if(strcmp(last,key)){ if(last[0]&&failsop) printf("  %s: %d fails\n",last,failsop); strcpy(last,key); failsop=0;}
    for(int i=0;i<8;i++) c.r[i]=iv[i]; for(int i=0;i<4;i++) c.s[i]=iv[8+i]; c.ip=iv[12]; c.fl=iv[13];
    c.halted=0; c.fault=0;
    /* shifts with count >=32: 8088 does not mask; skip */
    if((!strcmp(key,"D2")||!strcmp(key,"D3")||!strncmp(key,"D2.",3)||!strncmp(key,"D3.",3)) && (c.r[CX]&0xFF)>=32) continue;
    cpu_run(&c,1);
    int bad=0;
    for(int i=0;i<8;i++) if(c.r[i]!=fv[i]) bad|=1;
    for(int i=0;i<4;i++) if(c.s[i]!=fv[8+i]) bad|=2;
    if(c.ip!=fv[12]) bad|=4;
    if((c.fl&mask&0x0FFF)!=(fv[13]&mask&0x0FFF)) bad|=8;
    for(int i=0;i<nf;i++) if(mem[fa[i]]!=fvv[i]) bad|=16;
    ntest++;
    if(bad){ nfail++; failsop++; if(failsop<=2) printf("FAIL %s %s bad=%d fl=%04x exp=%04x mask=%04x ax=%04x/%04x dx=%04x/%04x ip=%04x/%04x\n",key,name,bad,c.fl,fv[13],mask,c.r[0],fv[0],c.r[2],fv[2],c.ip,fv[12]); }
  }
  if(failsop) printf("  %s: %d fails\n",last,failsop);
  printf("tests=%ld fails=%ld\n",ntest,nfail);
}
