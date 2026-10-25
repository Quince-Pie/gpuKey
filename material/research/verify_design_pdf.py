#!/usr/bin/env python3
from pathlib import Path
import json
import fitz
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parent.parent
pdf=ROOT/'output/pdf/Wayland_GPU_Recorder_Design.pdf'
out=ROOT/'tmp/pdfs';out.mkdir(parents=True,exist_ok=True)
doc=fitz.open(pdf)
issues=[]; pages=[]
for i,page in enumerate(doc):
    text=page.get_text()
    if '\ufffd' in text:issues.append({'page':i+1,'issue':'replacement glyph'})
    for block in page.get_text('dict')['blocks']:
        if block.get('type')!=0:continue
        for line in block['lines']:
            for span in line['spans']:
                x0,y0,x1,y1=span['bbox']
                if x0 < 48 or x1 > page.rect.width-48 or y0<12 or y1>page.rect.height-12:
                    issues.append({'page':i+1,'issue':'text bounds','text':span['text'],'bbox':list(span['bbox'])})
    pix=page.get_pixmap(matrix=fitz.Matrix(1.25,1.25),alpha=False)
    name=out/f'design-{i+1:02d}.png';pix.save(name)
    pages.append({'page':i+1,'characters':len(text),'path':str(name)})
font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',17)
for start in range(0,len(pages),9):
    thumb_w=430;thumb_h=609;pad=16;label=30
    sheet=Image.new('RGB',(3*(thumb_w+pad)+pad,3*(thumb_h+label+pad)+pad),'#dfe5ea')
    draw=ImageDraw.Draw(sheet)
    for offset,info in enumerate(pages[start:start+9]):
        im=Image.open(info['path']);im.thumbnail((thumb_w,thumb_h))
        col=offset%3;row=offset//3;x=pad+col*(thumb_w+pad);y=pad+row*(thumb_h+label+pad)
        sheet.paste(im,(x,y));draw.text((x+4,y+thumb_h+5),f"Page {info['page']}",font=font,fill='#16324f')
    sheet.save(out/f'contact-{start//9+1:02d}.png')
result={'pdf':str(pdf),'pages':len(pages),'text_bounds_or_glyph_issues':issues,'page_text':pages,'render_dpi':90,'renderer':'PyMuPDF','scope':'Automated text-bounds/glyph extraction plus full-page rasterization. Visual review is separate.'}
(out/'pdf_qa_results.json').write_text(json.dumps(result,indent=2))
print(json.dumps({'pages':len(pages),'issues':issues,'contact_sheets':(len(pages)+8)//9},indent=2))
