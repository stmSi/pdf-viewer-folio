"""Small deterministic PDFs for native engine and reader integration checks."""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)

def document(path, count, complex_page=False, annotations=False):
    objects = []
    def add(value=b''):
        objects.append(value.encode('latin-1') if isinstance(value, str) else value)
        return len(objects)
    catalog, pages = add(), add()
    font = add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
    outline = add()
    ids = [add() for _ in range(count)]
    for i, page in enumerate(ids):
        if count == 4:
            if i == 0:
                content = (b'BT /F1 24 Tf 48 740 Td (Folio Feature Test) Tj ET\n'
                           b'BT /F1 16 Tf 48 690 Td (Find needle and NEEDLE.) Tj 0 -24 Td (Caf\351 na\357ve) Tj '
                           b'0 -24 Td (A phrase across) Tj 0 -24 Td (two lines.) Tj 0 -86 Td '
                           b'(Jump to Chapter Two) Tj 0 -36 Td (Visit example.com) Tj ET\n')
                content += b'1 0 0 rg 48 430 60 40 re f 0 1 0 rg 120 430 60 40 re f 0 0 1 rg 192 430 60 40 re f\n'
            elif i == 1:
                content = b'BT /F1 24 Tf 48 700 Td (Chapter Two) Tj 0 -48 Td /F1 16 Tf (Another needle appears here.) Tj ET'
            elif i == 2:
                content = b'.7 .7 .7 rg 48 100 500 500 re f'
            else:
                content = b'BT /F1 24 Tf 48 300 Td (Rotated page) Tj ET'
        else:
            content = f'BT /F1 20 Tf 48 740 Td (Large document page {i+1}: needle) Tj ET'.encode()
        if complex_page:
            content += b'\n' + b' '.join(f'{x%500} {(x//500)%600} 1 1 re f'.encode() for x in range(60000))
        stream = add(b'<< /Length ' + str(len(content)).encode() + b' >>\nstream\n' + content + b'\nendstream')
        extra = ''
        if annotations:
            annots = []
            for label, flags, y in [('Printable note.', 4, 600), ('Screen-only note.', 0, 550)]:
                appearance = f'BT /F1 16 Tf 0 8 Td ({label}) Tj ET'.encode()
                ap = add(f'<< /Type /XObject /Subtype /Form /BBox [0 0 252 30] /Resources << /Font << /F1 {font} 0 R >> >> /Length {len(appearance)} >>\nstream\n'.encode()+appearance+b'\nendstream')
                annots.append(add(f'<< /Type /Annot /Subtype /FreeText /F {flags} /Rect [48 {y} 300 {y+30}] /Contents ({label}) /DA (/F1 16 Tf 0 g) /AP << /N {ap} 0 R >> >>'))
            extra = '/Annots ['+' '.join(f'{a} 0 R' for a in annots)+'] '
        if i == 0 and count == 4:
            internal = add(f'<< /Type /Annot /Subtype /Link /Rect [46 524 260 548] /Border [0 0 1] /Dest [{ids[1]} 0 R /XYZ 48 700 0] >>')
            external = add('<< /Type /Annot /Subtype /Link /Rect [46 488 260 512] /Border [0 0 1] /A << /S /URI /URI (https://example.com/docs) >> >>')
            extra = f'/Annots [{internal} 0 R {external} 0 R] '
        box = '[0 0 400 600]' if count == 4 and i == 3 else '[0 0 612 792]'
        if count == 4 and i == 3: extra += '/Rotate 90 '
        objects[page-1] = f'<< /Type /Page /Parent {pages} 0 R /MediaBox {box} /Resources << /Font << /F1 {font} 0 R >> >> /Contents {stream} 0 R {extra} >>'.encode()
    first = add(f'<< /Title (Chapter One) /Parent {outline} 0 R /Dest [{ids[0]} 0 R /Fit] >>')
    second = add(f'<< /Title (Chapter Two) /Parent {outline} 0 R /Prev {first} 0 R /Dest [{ids[min(1,count-1)]} 0 R /Fit] >>')
    objects[first-1] = f'<< /Title (Chapter One) /Parent {outline} 0 R /Next {second} 0 R /Dest [{ids[0]} 0 R /Fit] >>'.encode()
    objects[outline-1] = f'<< /Type /Outlines /First {first} 0 R /Last {second} 0 R /Count 2 >>'.encode()
    objects[catalog-1] = f'<< /Type /Catalog /Pages {pages} 0 R /Outlines {outline} 0 R >>'.encode()
    objects[pages-1] = ('<< /Type /Pages /Count '+str(count)+' /Kids ['+' '.join(f'{p} 0 R' for p in ids)+'] >>').encode()
    data=bytearray(b'%PDF-1.7\n')
    offsets=[0]
    for i,obj in enumerate(objects,1):
        offsets.append(len(data));data+=f'{i} 0 obj\n'.encode()+obj+b'\nendobj\n'
    xref=len(data);data+=f'xref\n0 {len(objects)+1}\n0000000000 65535 f \n'.encode()
    for offset in offsets[1:]: data+=f'{offset:010d} 00000 n \n'.encode()
    data+=f'trailer\n<< /Size {len(objects)+1} /Root {catalog} 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
    path.write_bytes(data)

document(root/'features.pdf',4)
document(root/'large.pdf',1500)
document(root/'complex.pdf',1,True)
document(root/'annotated.pdf',1,annotations=True)
(root/'invalid.pdf').write_text('This is not a PDF.\n')
