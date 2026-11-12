"""Dump a .fng screen's resolved widget layout: world position/size/color, the
font each String picks via OP's resource index, justification and wrap width.

Reads the tag stream straight out of the file (`ObjD` records, then 2-char tag +
u16 length + payload) rather than going through fng_parse.py's JSON, because the
OP tag's 4th dword -- the index into the screen's own resource table, i.e. WHICH
FONT a String is authored in -- is what the sizing work needs and the JSON
flattens it away.
"""
import struct, sys

f = lambda b, o: struct.unpack_from('<f', b, o)[0]
u = lambda b, o: struct.unpack_from('<I', b, o)[0]


def parse(path):
    b = open(path, 'rb').read()
    # Resource table: the run of 24-byte records that precede the first ObjD.
    first = b.find(b'ObjD')
    res, names = [], {}
    ro = b.find(b'RECs')
    # Names live in a string blob; recover them by scanning for the .tga/.ffn/.bmp
    # text and pairing each 24-byte record's nameOffset against that blob.
    nb = b.find(b'(Untextured')
    blob = b[nb:b.find(b'\x00\x00', nb)] if nb != -1 else b''
    o = nb - 0x30
    # Records are 24 bytes: hash, nameOffset, kind, f3, f4, f5 -- walk back from
    # the name blob until the hashes stop looking like a table.
    ro = 0x120
    while ro + 24 <= first:
        h = u(b, ro); no = u(b, ro + 4); kind = u(b, ro + 8)
        if kind > 4: break
        nm = blob[no:blob.find(b'\x00', no)].decode('ascii', 'replace') if blob else '?'
        res.append(dict(hash=h, name=nm, kind=kind))
        ro += 24
    widgets = []
    o = first
    while o != -1:
        end = b.find(b'ObjD', o + 4)
        rec = b[o + 8: (end - 4 if end != -1 else len(b))]
        w = dict(guid=None, par=None, tex=None, ridx=None, ty=None, flags=0,
                 px=0.0, py=0.0, pz=0.0, sw=None, sh=None, col=None,
                 lit=None, strh=None, Sj=None, Sw=None, Sb=None)
        p = 0
        while p + 4 <= len(rec):
            tag = rec[p:p + 2].decode('latin1'); ln = struct.unpack_from('<H', rec, p + 2)[0]
            d = rec[p + 4: p + 4 + ln]; p += 4 + ln
            if tag == 'Ot' and ln >= 4: w['ty'] = u(d, 0)
            elif tag == 'OP' and ln >= 16:
                w['guid'] = u(d, 0); w['tex'] = u(d, 4); w['flags'] = u(d, 8); w['ridx'] = u(d, 12)
            elif tag == 'PA' and ln >= 4: w['par'] = u(d, 0)
            elif tag == 'SH' and ln >= 4: w['strh'] = u(d, 0)
            elif tag == 'Sj' and ln >= 4: w['Sj'] = u(d, 0)
            elif tag == 'Sw' and ln >= 4: w['Sw'] = u(d, 0)
            elif tag == 'Sb' and ln >= 4: w['Sb'] = u(d, 0)
            elif tag == 'St': w['lit'] = d.decode('utf-16-le', 'replace').split('\x00')[0]
            elif tag == 'SA':
                if ln >= 0x10: w['col'] = (u(d, 0) & 255, u(d, 4) & 255, u(d, 8) & 255, u(d, 0xc) & 255)
                if ln >= 0x28: w['px'], w['py'], w['pz'] = f(d, 0x1c), f(d, 0x20), f(d, 0x24)
                if ln >= 0x44: w['sw'], w['sh'] = f(d, 0x38), f(d, 0x3c)
        widgets.append(w)
        o = end
    return res, widgets


TYPE = {0: 'Group', 1: 'Image', 2: 'String', 3: 'Type3', 4: 'Type4'}


def main(path):
    res, W = parse(path)
    print("== resources ==")
    for i, r in enumerate(res):
        print("   [%d] %s kind=%d %s" % (i, hex(r['hash']), r['kind'], r['name']))
    byg = {w['guid']: w for w in W if w['guid'] is not None}

    def world(w):
        x, y, z = w['px'], w['py'], w['pz']; p = byg.get(w['par']); n = 0
        while p is not None and n < 32:
            x += p['px']; y += p['py']; z += p['pz']; p = byg.get(p['par']); n += 1
        return x, y, z
    print("== %d widgets, far-to-near ==" % len(W))
    for i, w in sorted(enumerate(W), key=lambda t: -world(t[1])[2]):
        wx, wy, wz = world(w)
        ty = TYPE.get(w['ty'], str(w['ty']))
        det = ''
        if ty == 'String':
            ri = w['ridx']
            fn = res[ri]['name'] if isinstance(ri, int) and 0 <= ri < len(res) else '?'
            det = 'font[%s]=%s Sj=%s Sw=%s Sb=%s SH=%s %r' % (
                ri, fn, w['Sj'], w['Sw'], w['Sb'], hex(w['strh'] or 0), w['lit'])
        elif ty == 'Image':
            det = 'tex=%s ridx=%s' % (hex(w['tex'] or 0), w['ridx'])
        print("%-3d %-6s guid=%-8s par=%-8s w=(%7.1f,%7.1f,%7.1f) sz=%-11s col=%-20s fl=%08x %s" % (
            i, ty, w['guid'], w['par'], wx, wy, wz,
            "%.0fx%.0f" % (w['sw'], w['sh']) if w['sw'] is not None else '-',
            str(w['col']), w['flags'], det))


if __name__ == '__main__':
    main(sys.argv[1])
