"""Convert P6 LVGL renders to PNG without third-party dependencies."""
from pathlib import Path
import struct
import sys
import zlib

def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\0' + pixels[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

root=Path(sys.argv[1])
files=sorted(root.glob('*.ppm'))
width,height=137*6,242*((len(files)+5)//6)
canvas=bytearray(b'\x20\x27\x33' * width*height)
for i,path in enumerate(files):
    magic, dimensions, maxvalue, pixels=path.read_bytes().split(b'\n',3)
    w,h=map(int,dimensions.split())
    assert magic==b'P6' and maxvalue==b'255' and len(pixels)==w*h*3
    png(path.with_suffix('.png'), w, h, pixels)
    x,y=(i%6)*137,(i//6)*242
    for row in range(h):
        dest=((y+row)*width+x)*3
        canvas[dest:dest+w*3]=pixels[row*w*3:(row+1)*w*3]
png(root/'contact-sheet.png',width,height,canvas)
(root/'contact-sheet.txt').write_text('\n'.join(path.stem for path in files))
print(len(files),'LVGL previews exported')
