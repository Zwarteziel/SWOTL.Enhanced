import json,gzip,sys,os
src=sys.argv[1]; out=sys.argv[2]
meta=json.load(open(os.path.join(src,'metadata.json')))['opcodes']
R=['ax','cx','dx','bx','sp','bp','si','di','es','cs','ss','ds','ip','flags']
skip={'0F','9C','9D','54','9B','F4','D8','D9','DA','DB','DC','DD','DE','DF','CC','CD','CE','CF'}
with open(out,'w') as o:
  for fn in sorted(os.listdir(src)):
    if not fn.endswith('.json.gz'): continue
    key=fn[:-8]; parts=key.split('.')
    op=parts[0]
    md=meta.get(op,{})
    if len(parts)>1: md=md.get('reg',{}).get(parts[1],{})
    if md.get('status') in ('alias','prefix','undefined','fpu') or op in skip or (op>='60' and op<='6F') or op in ('C0','C1','C8','C9','D6','F1'): continue
    mask=md.get('flags-mask',0xFFFF)&0x0FFF
    tests=json.load(gzip.open(os.path.join(src,fn)))[:int(sys.argv[3])]
    for t in tests:
      ini=t['initial']; fin=t['final']
      iv=[ini['regs'][r] for r in R]
      fv=[fin['regs'].get(r,ini['regs'][r]) for r in R]
      o.write('T %s %d %s\n'%(key,mask,t['name'].replace(' ','_')))
      o.write(' '.join(map(str,iv))+'\n')
      o.write('%d %s\n'%(len(ini['ram']),' '.join('%d %d'%(a,v) for a,v in ini['ram'])))
      o.write(' '.join(map(str,fv))+'\n')
      o.write('%d %s\n'%(len(fin['ram']),' '.join('%d %d'%(a,v) for a,v in fin['ram'])))
