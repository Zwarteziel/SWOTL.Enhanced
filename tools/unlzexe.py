import struct,sys
d=open(sys.argv[1],'rb').read()
hdr=struct.unpack('<14H',d[:28])
hsz=hdr[4]*16; cs=hdr[11]; ip=hdr[10]
base=hsz+cs*16
oip,ocs,osp,oss,cmpsz,incsz,decsz,chk=struct.unpack('<8H',d[base:base+16])
src=d[hsz:base]; p=0; out=bytearray()
bits=0;cnt=0
def getbit():
    global bits,cnt,p
    if cnt==0:
        bits=src[p]|src[p+1]<<8; p+=2; cnt=16
    b=bits&1; bits>>=1; cnt-=1
    if cnt==0:
        bits=src[p]|src[p+1]<<8; p+=2; cnt=16
    return b
def gb():
    global p
    v=src[p];p+=1;return v
# LZEXE: bit buffer refills immediately when empty
while True:
    if getbit():
        out.append(gb()); continue
    if getbit()==0:
        ln=getbit()*2; ln+=getbit(); ln+=2
        span=gb()-0x100
    else:
        b1=gb();b2=gb()
        span=(b1|((b2&0xf8)<<5)|0xe000)-0x10000
        ln=b2&7
        if ln==0:
            ln=gb()
            if ln==0: break
            if ln==1: continue
            ln+=1
        else: ln+=2
    for i in range(ln): out.append(out[span])
# relocations (0.91)
r=base+0x158; relocs=[]; seg=0; off=0
while True:
    s=d[r];r+=1
    if s==0:
        s=d[r]|d[r+1]<<8; r+=2
        if s==0: seg+=0x0fff; continue
        if s==1: break
    off+=s; seg+=(off&~0xf)>>4; off&=0xf; relocs.append((off,seg))
print('out',len(out),'relocs',len(relocs),'entry %04x:%04x ss:sp %04x:%04x'%(ocs,oip,oss,osp),file=sys.stderr)
nh=0x20+((len(relocs)*4+15)//16)*16
total=nh+len(out)
H=struct.pack('<14H',0x5a4d,total%512,(total+511)//512,len(relocs),nh//16,hdr[5],0xffff,oss,osp,0,oip,ocs,0x1c,0)
H+=b''.join(struct.pack('<HH',o,s) for o,s in relocs)
H=H.ljust(nh,b'\0')
open(sys.argv[2],'wb').write(H+out)
