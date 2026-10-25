#!/usr/bin/env python3
"""Build the dated design reference from its reviewed chapter sources."""
from pathlib import Path
from html import escape
import re, json, textwrap, os
from reportlab.pdfgen import canvas
from reportlab.platypus import BaseDocTemplate, PageTemplate, Frame, Paragraph, Spacer, PageBreak, Table, TableStyle, Preformatted, Flowable, KeepTogether
from reportlab.platypus.tableofcontents import TableOfContents
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT, TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from pypdf import PdfReader

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT/'output'/'pdf'
OUT.mkdir(parents=True, exist_ok=True)
TMP = ROOT/'tmp'/'pdfs'
TMP.mkdir(parents=True, exist_ok=True)
font_candidates = [Path(os.environ.get('RECORDER_DESIGN_FONT_DIR','/usr/share/fonts/truetype/dejavu')), Path('/opt/codex/runtimes/codex-primary-runtime/dependencies/native/libreoffice-headless/libreoffice/share/fonts/truetype')]
required_fonts=['DejaVuSans.ttf','DejaVuSans-Bold.ttf','DejaVuSans-Oblique.ttf','DejaVuSans-BoldOblique.ttf','DejaVuSansMono.ttf']
fontroot = next((p for p in font_candidates if all((p/f).exists() for f in required_fonts)),None)
if fontroot is None: raise RuntimeError('Install DejaVu fonts or set RECORDER_DESIGN_FONT_DIR to their directory')
for name, file in [('Body','DejaVuSans.ttf'),('BodyBold','DejaVuSans-Bold.ttf'),('BodyItalic','DejaVuSans-Oblique.ttf'),('BodyBoldItalic','DejaVuSans-BoldOblique.ttf'),('Code','DejaVuSansMono.ttf')]:
    pdfmetrics.registerFont(TTFont(name,str(fontroot/file)))
pdfmetrics.registerFontFamily('Body',normal='Body',bold='BodyBold',italic='BodyItalic',boldItalic='BodyBoldItalic')
NAVY=colors.HexColor('#16324f'); TEAL=colors.HexColor('#006d77'); INK=colors.HexColor('#233442'); GRAY=colors.HexColor('#60717e'); LINE=colors.HexColor('#d7e0e6'); PALE=colors.HexColor('#f2f6f8')
PAGE_W,PAGE_H=A4
LEFT=52; RIGHT=52; TOP=57; BOTTOM=47
WIDTH=PAGE_W-LEFT-RIGHT
styles={
 'body':ParagraphStyle('body',fontName='Body',fontSize=9.6,leading=14.2,textColor=INK,spaceAfter=7.2,splitLongWords=True,allowWidows=0,allowOrphans=0),
 'h1':ParagraphStyle('h1',fontName='BodyBold',fontSize=19,leading=24,textColor=NAVY,spaceBefore=0,spaceAfter=14,keepWithNext=True),
 'h2':ParagraphStyle('h2',fontName='BodyBold',fontSize=12,leading=16,textColor=NAVY,spaceBefore=13,spaceAfter=7,keepWithNext=True),
 'h3':ParagraphStyle('h3',fontName='BodyBold',fontSize=10.2,leading=14,textColor=TEAL,spaceBefore=10,spaceAfter=5,keepWithNext=True),
 'table':ParagraphStyle('table',fontName='Body',fontSize=8.15,leading=11.5,textColor=INK,spaceAfter=0,splitLongWords=True),
 'thead':ParagraphStyle('thead',fontName='BodyBold',fontSize=8.15,leading=11.5,textColor=colors.white,spaceAfter=0,splitLongWords=True),
 'small':ParagraphStyle('small',fontName='Body',fontSize=8,leading=11.5,textColor=GRAY,spaceAfter=6,splitLongWords=True),
 'ref':ParagraphStyle('ref',fontName='Body',fontSize=8.1,leading=11.4,textColor=INK,spaceAfter=8,splitLongWords=True),
 'bullet':ParagraphStyle('bullet',fontName='Body',fontSize=9.6,leading=14.2,textColor=INK,spaceAfter=5,leftIndent=13,firstLineIndent=-10,splitLongWords=True),
 'code':ParagraphStyle('code',fontName='Code',fontSize=7.45,leading=10.4,textColor=INK,backColor=PALE,borderPadding=9,spaceBefore=5,spaceAfter=10),
 'caption':ParagraphStyle('caption',fontName='Body',fontSize=8.7,leading=12.5,textColor=GRAY,spaceBefore=5,spaceAfter=9),
}

def norm(text):
    for c in '\u2010\u2011\u2012\u2013\u2014\u2212': text=text.replace(c,'-')
    return text.replace('\u00a0',' ').replace('\u202f',' ')

chapters=['chapter_foundations.md','chapter_gpu.md','chapter_timing.md','chapter_queues.md','chapter_io.md','chapter_operations.md']
parts=[]; source_lines=[]
for f in chapters:
    raw=norm((ROOT/'research'/f).read_text())
    m=re.search(r'^## (?:GPU chapter sources|Sources|References)\s*$',raw,re.M)
    if m:
        body=raw[:m.start()].rstrip()
        source_lines.extend(x.strip() for x in raw[m.end():].splitlines() if x.strip().startswith('- '))
    else: body=raw
    # The independently reviewed PipeWire cross-check is fully integrated in chapter 5.
    if f=='chapter_queues.md':
        body=re.sub(r'## 15\.7 PipeWire:.*?(?=## 15\.8)',
                    '## PipeWire lifetime cross-check\n\nThe early-return, release-promise and buffer-removal contract is integrated in Chapter 5. The independent review agrees with the official producer/consumer contract; its source pointers are retained here for audit. [Q13, Q14, Q15, Q16, Q17]\n\n',body,flags=re.S)
    parts.append(body)
core=norm((ROOT/'research'/'sources_core.md').read_text())
source_lines=[x.strip() for x in core.splitlines() if x.strip().startswith('- ')]+source_lines
refs={}
for s in source_lines:
    m=re.match(r'-\s*(?:\*\*)?\[([A-Z]+\d+)\](?:\*\*)?\s*(.*)',s)
    if not m: raise ValueError('Bad source line '+s)
    if m[1] in refs: raise ValueError('Duplicate source id '+m[1])
    refs[m[1]]=m[2]
body='\n\n'.join(parts)
# Re-number headings once; remove agent draft numbering.
chapter=0; section=0; output=[]
for line in body.splitlines():
    m=re.match(r'^(#{1,3})\s+(.*)',line)
    if not m: output.append(line); continue
    level=len(m[1]); title=re.sub(r'^\d+(?:\.\d+)*\.?\s+','',m[2])
    if level==1:
        chapter+=1; section=0; title=f'{chapter}. {title}'
    elif level==2:
        section+=1; title=f'{chapter}.{section} {title}'
    output.append(m[1]+' '+title)
body='\n'.join(output)

def cited_ids(block):
    out=[]
    for a,b,c in re.findall(r'((?:IO|C|G|Q)\d{2})(?:\s*-\s*((?:IO|C|G|Q)?)(\d{2}))?',block):
        if c:
            p=re.match(r'[A-Z]+',a)[0]; q=b or p
            if p!=q: raise ValueError('Cross-prefix citation range')
            out.extend(f'{p}{i:02d}' for i in range(int(re.search(r'\d+',a)[0]),int(c)+1))
        else: out.append(a)
    return out
used=set()
for b in re.findall(r'\[([^\]]+)\]',body): used.update(cited_ids(b))
missing=used-set(refs)
if missing: raise ValueError('Unresolved citations '+str(missing))
(ROOT/'research'/'source_registry.json').write_text(json.dumps(refs,indent=2))

# The source is included in the companion bundle for exact review/reproduction.
master='# Wayland GPU Recorder Design Reference\n\nResearch snapshot: 19 October 2026. Design proposal; hardware validation pending.\n\n'+body+'\n\n# Primary sources and verification artifacts\n\n'+ '\n\n'.join(source_lines)+'\n'
(ROOT/'research'/'Wayland_GPU_Recorder_Design.md').write_text(master)

url_pattern=re.compile(r'https?://[^\s<>]+')
def inline(text, reference=False):
    text=norm(text)
    # Save code spans/links as HTML tokens before general escaping.
    saved=[]
    def token(x):
        saved.append(x); return f'ZZZTOKEN{len(saved)-1}ZZZ'
    text=re.sub(r'`([^`]+)`',lambda m:token('<font name="Code" size="8.4">'+escape(m[1])+'</font>'),text)
    if reference:
        def url_sub(m):
            url=m[0].rstrip('.,;')
            suffix=m[0][len(url):]
            return token('<link href="'+escape(url,quote=True)+'" color="#006d77">'+escape(url)+'</link>')+suffix
        text=url_pattern.sub(url_sub,text)
    text=escape(text)
    text=re.sub(r'\*\*(.+?)\*\*',r'<b>\1</b>',text)
    text=re.sub(r'(?<!\*)\*([^*]+)\*(?!\*)',r'<i>\1</i>',text)
    def csub(m):
        ids=cited_ids(m[1])
        if not ids or not all(x in refs for x in ids): return m[0]
        return '<font size="8.1">['+', '.join('<link href="#src-'+x+'" color="#006d77">'+x+'</link>' for x in ids)+']</font>'
    text=re.sub(r'\[([^\]]+)\]',csub,text)
    for i,x in enumerate(saved): text=text.replace(f'ZZZTOKEN{i}ZZZ',x)
    return text

class RecorderDoc(BaseDocTemplate):
    def __init__(self,path):
        super().__init__(str(path),pagesize=A4,leftMargin=LEFT,rightMargin=RIGHT,topMargin=TOP,bottomMargin=BOTTOM,title='Wayland GPU Recorder - Design Reference',author='OpenAI',subject='AV1, Linux 6.12+, native AMD/NVIDIA/Intel paths, ownership and validation')
        self.current_heading=''
        self.heading_pages=[]
        self.addPageTemplates(PageTemplate(id='main',frames=[Frame(LEFT,BOTTOM,WIDTH,PAGE_H-TOP-BOTTOM,id='normal',leftPadding=0,rightPadding=0,topPadding=0,bottomPadding=0)],onPage=self.decorate))
    def beforeDocument(self):
        self.heading_pages=[]; self.current_heading=''
    def decorate(self,c,doc):
        c.saveState()
        if doc.page>1:
            c.setFont('BodyBold',7.1);c.setFillColor(NAVY)
            c.drawString(LEFT,PAGE_H-29,'WAYLAND GPU RECORDER  /  DESIGN REFERENCE')
            c.setFont('Body',7.1); c.setFillColor(GRAY);c.drawRightString(PAGE_W-RIGHT,PAGE_H-29,'19 OCTOBER 2026')
            c.setStrokeColor(LINE);c.setLineWidth(.6);c.line(LEFT,PAGE_H-36,PAGE_W-RIGHT,PAGE_H-36)
            c.line(LEFT,34,PAGE_W-RIGHT,34)
            c.setFont('Body',6.7);c.drawString(LEFT,23,'Design proposal  |  Native hardware validation remains required')
            c.setFont('BodyBold',8);c.drawRightString(PAGE_W-RIGHT,23,str(doc.page))
        c.restoreState()
    def afterFlowable(self,f):
        if isinstance(f,Paragraph) and hasattr(f,'bookmark_id'):
            self.canv.bookmarkPage(f.bookmark_id)
            self.canv.addOutlineEntry(f.getPlainText(),f.bookmark_id,level=f.bookmark_level,closed=f.bookmark_level>0)
            if f.bookmark_level==0:
                self.notify('TOCEntry',(0,f.getPlainText(),self.page,f.bookmark_id))
                self.heading_pages.append({'title':f.getPlainText(),'page':self.page})

class ArchitectureMap(Flowable):
    def __init__(self): super().__init__();self.width=WIDTH;self.height=385
    def draw(self):
        c=self.canv
        def box(x,y,w,h,title,subtitle='',fill=PALE):
            c.setFillColor(fill);c.setStrokeColor(LINE);c.roundRect(x,y,w,h,6,fill=1,stroke=1)
            p=Paragraph('<b>'+escape(title)+'</b>'+('<br/>'+escape(subtitle) if subtitle else ''),ParagraphStyle('node',fontName='Body',fontSize=8.6,leading=12,textColor=NAVY,alignment=TA_CENTER))
            aw,ah=p.wrap(w-14,h-8);p.drawOn(c,x+7,y+(h-ah)/2)
        def arrow(x1,y1,x2,y2):
            c.setStrokeColor(TEAL);c.setLineWidth(1);c.line(x1,y1,x2,y2)
            p=c.beginPath();p.moveTo(x2,y2);p.lineTo(x2-3,y2+6);p.lineTo(x2+3,y2+6);p.close();c.setFillColor(TEAL);c.drawPath(p,fill=1,stroke=0)
        mid=WIDTH/2
        box(mid-156,324,312,48,'Authorized capture','Portal / PipeWire or eligible direct backend')
        arrow(mid,324,mid,305)
        box(mid-156,256,312,49,'Capability and semantic checks','Device, modifier, color, transforms and synchronization')
        bw=(WIDTH-26)/3
        xs=[0,bw+13,2*(bw+13)]
        for x,title,sub in zip(xs,['Direct native input','Dedicated processing','Fused GPU conversion'],['Eligible RGB / YUV','VPE / VPP where supported','Write the final encoder allocation']):
            cx=x+bw/2
            c.setStrokeColor(TEAL);c.line(mid,256,mid,240);c.line(mid,240,cx,240)
            arrow(cx,240,cx,223);box(x,166,bw,57,title,sub)
            c.setStrokeColor(TEAL);c.line(cx,166,cx,149);c.line(cx,149,mid,149)
        arrow(mid,149,mid,130)
        box(mid-156,81,312,49,'Native AV1 backend','Separate source, input, DPB and packet retirement')
        arrow(mid,81,mid,62)
        box(mid-200,8,400,54,'Ordered mux and owned encoded output','Audio joins here; buffered writer / ordinary io_uring candidates')

story=[]
cover_title=ParagraphStyle('coverTitle',fontName='BodyBold',fontSize=31,leading=38,textColor=NAVY,spaceAfter=18)
cover_sub=ParagraphStyle('coverSub',fontName='Body',fontSize=14,leading=21,textColor=TEAL,spaceAfter=16)
story += [Spacer(1,48),Paragraph('WAYLAND<br/>GPU RECORDER',cover_title),Paragraph('Design Reference',cover_sub),Paragraph('AV1  /  C23  /  Linux 6.12 and later',styles['h2']),Spacer(1,18)]
story.append(Paragraph('A complete architecture for modern AMD, NVIDIA and Intel GPUs, with explicit resource lifetimes, capability-selected image paths, detailed I/O and concurrency analysis, and reproducible design checks.',ParagraphStyle('coverBody',parent=styles['body'],fontSize=11,leading=17)))
story += [Spacer(1,24),Table([[Paragraph('<b>RESEARCH SNAPSHOT</b><br/>19 October 2026',styles['small']),Paragraph('<b>EVIDENCE STATUS</b><br/>Design established; hardware performance unresolved',styles['small'])]],colWidths=[WIDTH*.38,WIDTH*.62],style=TableStyle([('BACKGROUND',(0,0),(-1,-1),PALE),('BOX',(0,0),(-1,-1),.7,LINE),('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),12),('RIGHTPADDING',(0,0),(-1,-1),12),('TOPPADDING',(0,0),(-1,-1),12),('BOTTOMPADDING',(0,0),(-1,-1),10)])),Spacer(1,24)]
for t in ['Remove avoidable image materialization before tuning bookkeeping.','Select complete GPU paths from actual capabilities and media semantics.','Distinguish wrapper return, GPU retirement, packet readiness and durable output.','Compare strong alternatives under matched quality, latency and whole-system cost.']:
    story.append(Paragraph('• '+t,styles['bullet']))
story.append(Spacer(1,25));story.append(Paragraph('This reference interprets the requested “AVI1” as AV1. It contains no claimed GPU benchmark, universal optimal preset or proof of a complete recorder. The companion bundle includes the reviewed source and three executable abstract checks.',styles['small']))
story.append(PageBreak())
story.append(Paragraph('Contents',styles['h1']))
toc=TableOfContents();toc.dotsMinLevel=0;toc.levelStyles=[ParagraphStyle('TOC0',fontName='Body',fontSize=9.5,leading=14.5,textColor=NAVY,spaceBefore=4,leftIndent=0,rightIndent=34,firstLineIndent=0)]
story.append(toc);story.append(PageBreak())
mapheading=Paragraph('Architecture map',styles['h1']);mapheading.bookmark_id='architecture-map';mapheading.bookmark_level=0
story.append(mapheading)
story.append(Paragraph('One semantic core, several complete image paths',styles['h2']))
story.append(ArchitectureMap())
story.append(Paragraph('The branches are alternatives selected by actual capabilities and required image operations. They are not mandatory consecutive stages. An intentional GPU copy can be added where ownership isolation wins the complete comparison.',styles['caption']))
story.append(Paragraph('<b>Critical feedback:</b> source reuse depends on the actual last source reader and the negotiated release signal. Direct encoder input and a conversion into owned storage can release the source at different times. No queue dequeue or CPU submission substitutes for that event.',styles['body']))
story.append(Paragraph('<b>Independent budgets:</b> source leases, owned image slots, encoder references, packet bytes, mux staging and sink-in-flight bytes. Admission prevents one slow consumer from turning every upstream pool into an uncontrolled latency queue.',styles['body']))

lines=body.splitlines();i=0;heading_counter=0
while i<len(lines):
    line=lines[i].strip()
    if not line:i+=1;continue
    h=re.match(r'^(#{1,3})\s+(.*)',line)
    if h:
        level=len(h[1]);title=h[2]
        if level==1:story.append(PageBreak())
        p=Paragraph(inline(title),styles['h'+str(level)])
        if level<=2:
            heading_counter+=1;p.bookmark_id=f'h-{heading_counter}';p.bookmark_level=level-1
        story.append(p);i+=1;continue
    if line.startswith('```'):
        code=[];i+=1
        while i<len(lines) and not lines[i].startswith('```'):
            rawline=norm(lines[i]);
            if len(rawline)>108:
                code.extend(textwrap.wrap(rawline,width=108,subsequent_indent='    ',break_long_words=False,break_on_hyphens=False,replace_whitespace=False))
            else:code.append(rawline)
            i+=1
        story.append(Preformatted('\n'.join(code),styles['code']));i+=1;continue
    if line.startswith('|'):
        rows=[]
        while i<len(lines) and lines[i].strip().startswith('|'):
            cells=[x.strip() for x in lines[i].strip().strip('|').split('|')]
            if not all(re.fullmatch(r'[:\- ]+',x or '-') for x in cells):rows.append(cells)
            i+=1
        n=len(rows[0]); assert all(len(r)==n for r in rows),(n,rows)
        if n==2: widths=[WIDTH*.37,WIDTH*.63]
        elif n==3: widths=[WIDTH*.25,WIDTH*.365,WIDTH*.385]
        elif n==4: widths=[WIDTH*.38,WIDTH*.28,WIDTH*.17,WIDTH*.17]
        else: widths=[WIDTH/n]*n
        data=[[Paragraph(inline(x),styles['thead' if ri==0 else 'table']) for x in row] for ri,row in enumerate(rows)]
        table=Table(data,colWidths=widths,repeatRows=1,hAlign='LEFT')
        ts=[('BACKGROUND',(0,0),(-1,0),NAVY),('VALIGN',(0,0),(-1,-1),'TOP'),('LEFTPADDING',(0,0),(-1,-1),7),('RIGHTPADDING',(0,0),(-1,-1),7),('TOPPADDING',(0,0),(-1,-1),7),('BOTTOMPADDING',(0,0),(-1,-1),7),('LINEBELOW',(0,0),(-1,0),.7,NAVY),('LINEBELOW',(0,1),(-1,-1),.4,LINE),('BOX',(0,0),(-1,-1),.5,LINE)]
        for ri in range(1,len(rows)):
            if ri%2:ts.append(('BACKGROUND',(0,ri),(-1,ri),PALE))
        table.setStyle(TableStyle(ts));story.extend([Spacer(1,4),table,Spacer(1,11)]);continue
    if line.startswith('- ') or re.match(r'^\d+\. ',line):
        isbullet=line.startswith('- '); m=re.match(r'^(\d+)\. (.*)',line)
        content=line[2:] if isbullet else m[2]
        prefix='• ' if isbullet else m[1]+'. '
        story.append(Paragraph(prefix+inline(content),styles['bullet']));i+=1;continue
    para=[line];i+=1
    while i<len(lines) and lines[i].strip() and not re.match(r'^(#{1,3}\s|\||```|- |\d+\. )',lines[i].strip()):
        para.append(lines[i].strip());i+=1
    story.append(Paragraph(inline(' '.join(para)),styles['body']))

story.append(PageBreak())
p=Paragraph('Primary sources and verification artifacts',styles['h1']);p.bookmark_id='bibliography';p.bookmark_level=0;story.append(p)
story.append(Paragraph(f'{len(refs)} source identifiers are retained for claim-level traceability. Some identify a small group of closely related primary documents. Current upstream pages and source snapshots were inspected on 19 October 2026; capability probing remains required on the deployed versions. Protocol mirrors and inaccessible full-text limitations are identified explicitly.',styles['body']))
for prefix,title in [('C','Capture, semantics and validation'),('G','GPU backends and AV1'),('Q','Concurrency and progress'),('IO','Containers and Linux I/O')]:
    story.append(Paragraph(title,styles['h2']))
    for key,desc in refs.items():
        if re.match(r'[A-Z]+',key)[0]!=prefix:continue
        story.append(Paragraph('<a name="src-'+key+'"/><b>['+key+']</b> '+inline(desc,reference=True),styles['ref']))

pdf=OUT/'Wayland_GPU_Recorder_Design.pdf'
doc=RecorderDoc(pdf);doc.multiBuild(story)
reader=PdfReader(str(pdf))
text='\n'.join(p.extract_text() or '' for p in reader.pages)
(TMP/'extracted_design.txt').write_text(text)
check={'pdf':str(pdf),'pages':len(reader.pages),'bytes':pdf.stat().st_size,'source_identifiers':len(refs),'cited_identifiers':len(used),'missing_citations':sorted(missing),'unused_source_identifiers':sorted(set(refs)-used),'body_words':len(body.split()),'source_words':len(master.split()),'headings':doc.heading_pages,'extracted_characters':len(text),'links':sum(len(p.get('/Annots',[])) for p in reader.pages)}
assert len(text)>100000
assert len(reader.pages)>20
assert 'QVBR' in text and 'SINGLE_ISSUER' in text and '1,833' in text
assert 'ZZZTOKEN' not in text
(TMP/'build_manifest.json').write_text(json.dumps(check,indent=2))
print(json.dumps({k:v for k,v in check.items() if k!='headings'},indent=2))
