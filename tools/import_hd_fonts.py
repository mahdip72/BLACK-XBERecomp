"""Convert the two reviewed HD Reimagined font atlases for the Xbox version. Requires Pillow and NumPy."""
from pathlib import Path
from PIL import Image,ImageDraw
import json,struct,unicodedata,hashlib,io,argparse
from pbrify_assets import Disc, fingerprint, sha256
import numpy as np
parser=argparse.ArgumentParser(description="Repack the mod font atlases for the Xbox glyph layout")
parser.add_argument("--iso",type=Path,required=True)
parser.add_argument("--mod",type=Path,required=True)
parser.add_argument("--output",type=Path,required=True)
args=parser.parse_args()
disc=Disc(args.iso)
r=Path(__file__).parent/"hd_textures"
out=args.output
out.mkdir(parents=True,exist_ok=True)
blobs={}
originals={}
for name in ("Big","Small"):
 at,length=disc.find(f"language/fonts/{name}.bin");blob=disc.read(at,length);blobs[name]=blob
 tex=struct.unpack_from("<I",blob,4)[0];fmt=struct.unpack_from("<I",blob,tex+12)[0];w,h=1<<((fmt>>20)&15),1<<((fmt>>24)&15)
 header=bytearray(128);header[:4]=b"DDS ";struct.pack_into("<7I",header,4,124,0x81007,h,w,w*h,0,0);struct.pack_into("<II4s",header,76,32,4,b"DXT3");struct.pack_into("<I",header,108,0x1000)
 originals[name]=Image.open(io.BytesIO(header+blob[tex+128:tex+128+w*h])).convert("RGBA")
font_files={"Big":"90e4dbc2b70dbdb0-94742894391acc0a-00002213.dds","Small":"69d81114d84a8ba9-5f8f8c1f98775ed7-000021d3.dds"}

maps=json.loads((r/'font-mapping.json').read_text())
# Explicit identities resolve characters with nearly identical low-resolution silhouettes.
big={'S':1,'0':97,'O':134,'Q':135}
small={'O':52,'Q':125,'\u00a5':120}
marks={'Big':{'\u0300':[9],'\u0301':[8],'\u0302':[37],'\u0308':[25,26],'\u0303':[70]},'Small':{'\u0300':[15],'\u0301':[14],'\u0302':[6],'\u0308':[28,29],'\u0303':[80]}}
records={}

def ink(im):
 box=im.getchannel('A').point(lambda x:255 if x>60 else 0).getbbox()
 return im.crop(box) if box else im

def white(im):
 a=im.getchannel('A');im=Image.new('RGBA',im.size,'white');im.putalpha(a);return im

for name,num,scale,factor in [('Big',65,4,4),('Small',56,10,8)]:
 orig=originals[name];hd=Image.open(args.mod/font_files[name]).convert('RGBA'); comps=json.loads((r/f'{name}-components.json').read_text());m={x['code']:x for x in maps[name]}
 def part(ids):
  boxes=[comps[i]['box'] for i in ids]; b=[min(x[0] for x in boxes),min(x[1] for x in boxes),max(x[2] for x in boxes),max(x[3] for x in boxes)]
  # Include fractional high-resolution coverage around the component without adjacent glyphs.
  return ink(hd.crop(tuple(v*scale for v in b)))
 patches={}
 for code,item in m.items():
  x,y,w,h=item['ps2'];patch=ink(hd.crop((x*scale,y*scale,(x+w)*scale,(y+h)*scale)))
  manual=(big if name=='Big' else small).get(chr(code))
  if manual is not None:patch=part([manual])
  patches[code]=white(patch)
 # Rebuild quotation marks from straight strokes rather than ambiguous bitmap-template matches.
 for code in (0x22,0x27):
  box=m[code]['native'];native=orig.crop(box);a=np.asarray(native.getchannel('A')); mask=a.max(axis=0)>70
  p=Image.new('RGBA',(native.width*factor,native.height*factor),(255,255,255,0));d=ImageDraw.Draw(p);start=None
  for x,val in enumerate(list(mask)+[False]):
   if val and start is None:start=x
   if not val and start is not None:
    d.rectangle((start*factor,0,x*factor-1,(native.height-1)*factor-1),fill='white');start=None
  patches[code]=ink(p)
 atlas=Image.new('RGBA',(orig.width*factor,orig.height*factor),(255,255,255,0));placed=set();counts={'mod':0,'accent':0,'preserved':0}
 for code,item in m.items():
  box=item['native'];glyph=orig.crop(box);ib=item['ink'];dest=Image.new('RGBA',(glyph.width*factor,glyph.height*factor),(255,255,255,0))
  dec=unicodedata.normalize('NFD',chr(code)); bodyh=22 if name=='Big' else 11
  accent=len(dec)==2 and ord(dec[0]) in patches and dec[1] in marks[name]
  if accent:
   # The glyph's lower cap-height rows contain the base letter; marks have their own native ink bounds.
   y0=glyph.height-bodyh;alpha=glyph.getchannel('A').point(lambda x:255 if x>70 else 0)
   bb=alpha.crop((0,y0,glyph.width,glyph.height)).getbbox();ab=alpha.crop((0,0,glyph.width,y0)).getbbox()
   assert bb and ab,(name,code,y0)
   bb=(bb[0],bb[1]+y0,bb[2],bb[3]+y0)
   for patch,b in ((patches[ord(dec[0])],bb),(white(part(marks[name][dec[1]])),ab)):
    patch=patch.resize(((b[2]-b[0])*factor,(b[3]-b[1])*factor),Image.Resampling.LANCZOS);dest.paste(patch,(b[0]*factor,b[1]*factor))
   counts['accent']+=1
  else:
   patch=patches[code].resize(((ib[2]-ib[0])*factor,(ib[3]-ib[1])*factor),Image.Resampling.LANCZOS);dest.paste(patch,(ib[0]*factor,ib[1]*factor));counts['mod']+=1
  atlas.paste(dest,(box[0]*factor,box[1]*factor));placed.add(tuple(box))
 # All other glyph rectangles, including Xbox-only control diagrams, remain recognizable.
 b=blobs[name];end=struct.unpack_from('<I',b,28)[0]
 for pos in range(0x220,end+1,40):
  u,v,w,h,*_=struct.unpack_from('<7f',b,pos);box=tuple(round(z*t) for z,t in zip((u,v,u+w,v+h),orig.size*2))
  if u<0 or h<=0 or box in placed:continue
  glyph=orig.crop(box).resize(((box[2]-box[0])*factor,(box[3]-box[1])*factor),Image.Resampling.LANCZOS);atlas.paste(glyph,(box[0]*factor,box[1]*factor));placed.add(box);counts['preserved']+=1
 atlas.save(out/f'{name}.png');print(name,atlas.size,counts)
 # Match the runtime key against the original compressed texels.
 off=struct.unpack_from('<I',b,4)[0];key=14695981039346656037^((orig.width<<32)|orig.height)
 for byte in b[off+128:off+128+orig.width*orig.height]:key=((key^byte)*1099511628211)&((1<<64)-1)
 assert key == {'Big':0xc64bc320db173a0c,'Small':0xd9489cbe3cdb34f1}[name], 'Unsupported original font layout'
 print('key',hex(key),'sha256',hashlib.sha256((out/f'{name}.png').read_bytes()).hexdigest())
 pixels=b[off+128:off+128+orig.width*orig.height]
 key=fingerprint(orig.width,orig.height,14,pixels)
 # Preserve the reviewed font edges exactly; the original font also has one mip.
 atlas.save(out/f'{key}.dds',format='DDS')
 records[name]=dict(key=key,source='BLACK HD Reimagined font',source_file=font_files[name],
                    source_sha256=sha256(args.mod/font_files[name]),original_sha256=hashlib.sha256(pixels).hexdigest(),
                    png_sha256=sha256(out/f'{name}.png'),dds_sha256=sha256(out/f'{key}.dds'),size=list(atlas.size),format='BGRA8')
(out/'fonts.json').write_text(json.dumps(records,indent=2)+'\n')
