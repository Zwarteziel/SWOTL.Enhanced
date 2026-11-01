import sys
from PIL import Image
for n in sys.argv[1:]:
    im=Image.open(n); s=2 if im.width<=320 else 1
    im=im.resize((im.width*s,im.height*s*(2 if im.height==200 and im.width==640 else 1)),Image.NEAREST); im.save(n.replace('.ppm','.png'))
