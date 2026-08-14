#!/usr/bin/env python3
"""Genera las imagenes de muestra que consume test_decoders.cpp.

Todas codifican la misma referencia de 4x2 (salvo las monocromas y MacPaint, que
tienen su propio patron), asi el test compara siempre contra los mismos colores.
Solo stdlib: struct, zlib, os.
"""
import os, struct, zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "samples")

# Referencia 4x2. Fila 0: rojo, verde, azul, blanco. Fila 1: negro, gris, amarillo, cian.
W, H = 4, 2
PAL = [(255,0,0), (0,255,0), (0,0,255), (255,255,255),
       (0,0,0), (128,128,128), (255,255,0), (0,255,255)]
IDX = [[0,1,2,3], [4,5,6,7]]                       # indice de paleta por pixel
RGB = [[PAL[i] for i in row] for row in IDX]


def write(name, data):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)
    print("  %-18s %6d B" % (name, len(data)))


def png_bytes(w, h, rgba):
    """PNG RGBA8 minimo (sin filtros)."""
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgba[y*w*4:(y+1)*w*4]) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def ref_rgba(w=W, h=H):
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = RGB[y % H][x % W]
            out += bytes((r, g, b, 255))
    return out


# --------------------------------------------------------------- Netpbm ASCII
def gen_pnm():
    # P1: bitmap. 1 = negro. Ajedrez: 1 0 1 0 / 0 1 0 1
    write("ascii_p1.pbm", b"# lux test\nP1\n4 2\n1 0 1 0\n0 1 0 1\n".replace(b"# lux test\n", b""))
    # P2: escala de grises con maxval 100 (fuerza el reescalado a 0..255)
    write("ascii_p2.pgm", b"P2\n# comentario en el medio\n4 2\n100\n0 25 50 100\n100 50 25 0\n")
    # P3: RGB, maxval 255, con los 8 colores de referencia
    rows = []
    for y in range(H):
        rows.append(" ".join("%d %d %d" % RGB[y][x] for x in range(W)))
    write("ascii_p3.ppm", ("P3\n4 2\n255\n" + "\n".join(rows) + "\n").encode())


# ------------------------------------------------------------------------ XPM
def gen_xpm():
    keys = ".,:;+@#$"
    lines = ['/* XPM */', 'static char * ref_xpm[] = {', '"4 2 8 1",']
    for i, (r, g, b) in enumerate(PAL):
        lines.append('"%s c #%02X%02X%02X",' % (keys[i], r, g, b))
    for y in range(H):
        lines.append('"%s",' % "".join(keys[IDX[y][x]] for x in range(W)))
    lines.append('};')
    write("ref.xpm", ("\n".join(lines) + "\n").encode())


# ------------------------------------------------------------------- IFF ILBM
def _bmhd(w, h, planes, masking, compression, transparent=0):
    return struct.pack(">HHhhBBBBHBBHH", w, h, 0, 0, planes, masking,
                       compression, 0, transparent, 1, 1, w, h)


def _iff(kind, chunks):
    body = kind + b"".join(chunks)
    return b"FORM" + struct.pack(">I", len(body)) + body


def _chunk(tag, data):
    pad = b"\x00" if len(data) & 1 else b""
    return tag + struct.pack(">I", len(data)) + data + pad


def _planar_rows(planes):
    """Convierte IDX a filas planares (rowbytes = ceil(w/16)*2 por plano)."""
    rowb = ((W + 15) // 16) * 2
    out = []
    for y in range(H):
        row = bytearray()
        for p in range(planes):
            plane = bytearray(rowb)
            for x in range(W):
                if (IDX[y][x] >> p) & 1:
                    plane[x // 8] |= 0x80 >> (x & 7)
            row += plane
        out.append(bytes(row))
    return out


def _packbits(data):
    """ByteRun1 sobre un bloque (una fila entera)."""
    out = bytearray()
    i = 0
    while i < len(data):
        run = 1
        while i + run < len(data) and run < 128 and data[i + run] == data[i]:
            run += 1
        if run > 1:
            out += bytes((256 - (run - 1), data[i]))
            i += run
        else:
            j = i
            lit = bytearray()
            while j < len(data) and len(lit) < 128:
                if j + 2 < len(data) and data[j] == data[j+1] == data[j+2]:
                    break
                lit.append(data[j]); j += 1
            out += bytes((len(lit) - 1,)) + lit
            i = j
    return bytes(out)


def gen_ilbm():
    cmap = _chunk(b"CMAP", b"".join(bytes(c) for c in PAL))
    rows = _planar_rows(3)
    # sin comprimir
    write("ilbm_raw.iff", _iff(b"ILBM", [
        _chunk(b"BMHD", _bmhd(W, H, 3, 0, 0)), cmap,
        _chunk(b"BODY", b"".join(rows))]))
    # ByteRun1 (cada fila se comprime por separado)
    write("ilbm_rle.iff", _iff(b"ILBM", [
        _chunk(b"BMHD", _bmhd(W, H, 3, 0, 1)), cmap,
        _chunk(b"BODY", b"".join(_packbits(r) for r in rows))]))
    # PBM chunky de DPaint IIe: un byte por pixel, filas a tamano par
    rowb = (W + 1) & ~1
    chunky = b"".join(bytes(IDX[y] + [0] * (rowb - W)) for y in range(H))
    write("pbm_chunky.iff", _iff(b"PBM ", [
        _chunk(b"BMHD", _bmhd(W, H, 8, 0, 0)), cmap, _chunk(b"BODY", chunky)]))
    # EHB: 6 planos, 32 colores en el CMAP; los indices 32..63 son la mitad de brillo.
    # Fila 0 usa 0..3 y fila 1 los mismos indices +32 (deberia dar la mitad exacta).
    ehb_pal = [(255, 128, 64)] * 32
    ehb_idx = [[0, 1, 2, 3], [32, 33, 34, 35]]
    rowb6 = ((W + 15) // 16) * 2
    body = bytearray()
    for y in range(H):
        for p in range(6):
            plane = bytearray(rowb6)
            for x in range(W):
                if (ehb_idx[y][x] >> p) & 1:
                    plane[x // 8] |= 0x80 >> (x & 7)
            body += plane
    write("ilbm_ehb.iff", _iff(b"ILBM", [
        _chunk(b"BMHD", _bmhd(W, H, 6, 0, 0)),
        _chunk(b"CMAP", b"".join(bytes(c) for c in ehb_pal)),
        _chunk(b"CAMG", struct.pack(">I", 0x80)), _chunk(b"BODY", bytes(body))]))


# ------------------------------------------------------------------- MacPaint
def gen_macpaint():
    """576x720 monocromo. Fila 0 = 0xAA (negro/blanco alternado), resto blanco."""
    rowb, rows = 72, 720
    data = bytearray()
    for y in range(rows):
        val = 0xAA if y == 0 else 0x00
        data += bytes((256 - (rowb - 1), val))     # un run de 72 bytes iguales
    write("ref.mac", b"\x00" * 512 + bytes(data))


# ------------------------------------------------------------------------ XWD
def gen_xwd():
    name = b"lux\x00"
    hdr_size = 100 + len(name)
    words = [hdr_size, 7, 2, 24, W, H, 0, 1, 32, 1, 32, 32, W * 4, 4,
             0xFF0000, 0x00FF00, 0x0000FF, 8, 0, 0, W, H, 0, 0, 0]
    hdr = b"".join(struct.pack(">I", v) for v in words) + name
    px = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = RGB[y][x]
            px += struct.pack(">I", (r << 16) | (g << 8) | b)
    write("ref.xwd", hdr + bytes(px))


# ------------------------------------------------------------- DPX  /  Cineon
def _dpx_header(w, h, bits, packing, descriptor, transfer, data_off):
    hdr = bytearray(2048)
    hdr[0:4] = b"SDPX"
    struct.pack_into(">I", hdr, 4, data_off)         # offset a los datos
    hdr[8:16] = b"V2.0\x00\x00\x00\x00"
    struct.pack_into(">I", hdr, 16, data_off + w * h * 4)
    struct.pack_into(">I", hdr, 24, 768)             # generic header size
    struct.pack_into(">H", hdr, 768, 0)              # orientation
    struct.pack_into(">H", hdr, 770, 1)              # image elements
    struct.pack_into(">I", hdr, 772, w)
    struct.pack_into(">I", hdr, 776, h)
    hdr[780 + 20] = descriptor
    hdr[780 + 21] = transfer
    hdr[780 + 22] = transfer
    hdr[780 + 23] = bits
    struct.pack_into(">H", hdr, 780 + 24, packing)
    struct.pack_into(">H", hdr, 780 + 26, 0)         # encoding: sin RLE
    struct.pack_into(">I", hdr, 780 + 28, data_off)
    return bytes(hdr)


def _pack10(samples):
    """3 muestras de 10 bits por palabra de 32 (2 bits de relleno abajo)."""
    out = bytearray()
    for i in range(0, len(samples), 3):
        tri = list(samples[i:i+3]) + [0] * (3 - len(samples[i:i+3]))
        out += struct.pack(">I", (tri[0] << 22) | (tri[1] << 12) | (tri[2] << 2))
    return bytes(out)


def gen_dpx():
    # 10 bits, lineal (transfer=2: sin curva log) -> el test espera 8 bits exactos
    rows = bytearray()
    for y in range(H):
        s = []
        for x in range(W):
            s += [round(c * 1023 / 255) for c in RGB[y][x]]
        rows += _pack10(s)
    write("ref10.dpx", _dpx_header(W, H, 10, 1, 50, 2, 2048) + bytes(rows))

    # 8 bits sin empaquetar
    flat = bytearray()
    for y in range(H):
        for x in range(W):
            flat += bytes(RGB[y][x])
    write("ref8.dpx", _dpx_header(W, H, 8, 0, 50, 2, 2048) + bytes(flat))

    # Cineon 10 bits log: el negro de pelicula (95) y el blanco (685) tienen que
    # caer en 0 y 255 despues de la curva.
    hdr = bytearray(2048)
    struct.pack_into(">I", hdr, 0, 0x802A5FD7)
    struct.pack_into(">I", hdr, 4, 2048)             # image offset
    hdr[200] = 0                                     # orientation
    hdr[201] = 3                                     # canales
    hdr[206] = 10                                    # bits por muestra
    struct.pack_into(">I", hdr, 208, W)
    struct.pack_into(">I", hdr, 212, H)
    codes = []
    for y in range(H):
        for x in range(W):
            # fila 0: negro puro; fila 1: blanco puro
            v = 95 if y == 0 else 685
            codes += [v, v, v]
    write("ref.cin", bytes(hdr) + _pack10(codes))


# ----------------------------------------------------------------------- ICNS
def _icns(chunks):
    body = b"".join(tag + struct.pack(">I", len(d) + 8) + d for tag, d in chunks)
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def _icns_rle_channel(values):
    """RLE de ICNS: >=0x80 -> run de (b-125); <0x80 -> literal de (b+1)."""
    out = bytearray()
    i = 0
    while i < len(values):
        run = 1
        while i + run < len(values) and run < 130 and values[i + run] == values[i]:
            run += 1
        if run >= 3:
            out += bytes((125 + run, values[i]))
            i += run
        else:
            j, lit = i, bytearray()
            while j < len(values) and len(lit) < 128:
                if j + 2 < len(values) and values[j] == values[j+1] == values[j+2]:
                    break
                lit.append(values[j]); j += 1
            out += bytes((len(lit) - 1,)) + bytes(lit)
            i = j
    return bytes(out)


def gen_icns():
    # variante moderna: un PNG de 32x32 dentro de ic11 y otro de 64x64 en ic12,
    # para verificar ademas que elige el de mayor resolucion.
    small = png_bytes(32, 32, ref_rgba(32, 32))
    big = png_bytes(64, 64, ref_rgba(64, 64))
    write("png.icns", _icns([(b"ic11", small), (b"ic12", big)]))

    # variante clasica: il32 (32x32 RLE de 3 canales) + su mascara l8mk
    px = 32 * 32
    r = [RGB[y % H][x % W][0] for y in range(32) for x in range(32)]
    g = [RGB[y % H][x % W][1] for y in range(32) for x in range(32)]
    b = [RGB[y % H][x % W][2] for y in range(32) for x in range(32)]
    rle = b"".join(_icns_rle_channel(c) for c in (r, g, b))
    write("rle.icns", _icns([(b"il32", rle), (b"l8mk", bytes([128] * px))]))


# --------------------------------------------------- contenedores comprimidos
def gen_containers():
    svg = (b'<?xml version="1.0"?>\n<svg xmlns="http://www.w3.org/2000/svg" '
           b'width="120" height="60" viewBox="0 0 120 60">'
           b'<rect width="120" height="60" fill="#ff0000"/></svg>')
    co = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    body = co.compress(svg) + co.flush()
    gz = (b"\x1f\x8b\x08\x08" + b"\x00" * 4 + b"\x00\x03" + b"ref.svg\x00"
          + body + struct.pack("<II", zlib.crc32(svg) & 0xFFFFFFFF, len(svg)))
    write("ref.svgz", gz)

    # cualquier formato soportado, gzipeado: la extension util es la de adentro
    ppm = b"P6\n4 2\n255\n" + b"".join(bytes(RGB[y][x]) for y in range(H) for x in range(W))
    co2 = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    gz2 = co2.compress(ppm) + co2.flush()
    write("ref.ppm.gz", b"\x1f\x8b\x08\x00" + b"\x00" * 4 + b"\x00\x03" + gz2
                        + struct.pack("<II", zlib.crc32(ppm) & 0xFFFFFFFF, len(ppm)))

    merged = png_bytes(W, H, ref_rgba())

    def zipfile_bytes(entries):
        """ZIP minimo. entries = [(nombre, datos, metodo)]."""
        out, central = bytearray(), bytearray()
        for name, data, method in entries:
            nb = name.encode()
            if method == 8:
                c = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
                comp = c.compress(data) + c.flush()
            else:
                comp = data
            crc = zlib.crc32(data) & 0xFFFFFFFF
            lho = len(out)
            out += (b"PK\x03\x04" + struct.pack("<HHHHHIIIHH", 20, 0, method, 0, 0,
                    crc, len(comp), len(data), len(nb), 0) + nb + comp)
            central += (b"PK\x01\x02" + struct.pack("<HHHHHHIIIHHHHHII", 20, 20, 0, method,
                        0, 0, crc, len(comp), len(data), len(nb), 0, 0, 0, 0, 0, lho) + nb)
        cd = len(out)
        out += central
        out += (b"PK\x05\x06" + struct.pack("<HHHHIIH", 0, 0, len(entries), len(entries),
                len(central), cd, 0))
        return bytes(out)

    # ORA: mergedimage.png con deflate, mas ruido alrededor para que tenga que buscar
    write("ref.ora", zipfile_bytes([
        ("mimetype", b"image/openraster", 0),
        ("stack.xml", b"<image w='4' h='2'></image>", 8),
        ("mergedimage.png", merged, 8),
    ]))
    # KRA: mergedimage.png guardado sin comprimir (metodo stored)
    write("ref.kra", zipfile_bytes([
        ("mimetype", b"application/x-krita", 0),
        ("mergedimage.png", merged, 0),
    ]))

    # ANI: RIFF ACON -> LIST fram -> icon (un .ico con PNG adentro)
    ico_png = png_bytes(32, 32, ref_rgba(32, 32))
    ico = (struct.pack("<HHH", 0, 1, 1)
           + struct.pack("<BBBBHHII", 32, 32, 0, 0, 1, 32, len(ico_png), 22) + ico_png)
    anih = struct.pack("<IIIIIIIII", 36, 1, 1, 0, 0, 32, 32, 1, 1)
    fram = b"fram" + b"icon" + struct.pack("<I", len(ico)) + ico + (b"\x00" if len(ico) & 1 else b"")
    lst = b"LIST" + struct.pack("<I", len(fram)) + fram
    body = b"ACON" + b"anih" + struct.pack("<I", len(anih)) + anih + lst
    write("ref.ani", b"RIFF" + struct.pack("<I", len(body)) + body)


# ------------------------------------------------------------------ ACBM
def gen_acbm():
    """IFF ACBM: los planos van enteros uno atras del otro, no intercalados."""
    rowb = ((W + 15) // 16) * 2
    body = bytearray()
    for p in range(3):
        for y in range(H):
            plane = bytearray(rowb)
            for x in range(W):
                if (IDX[y][x] >> p) & 1:
                    plane[x // 8] |= 0x80 >> (x & 7)
            body += plane
    write("ref.acbm", _iff(b"ACBM", [
        _chunk(b"BMHD", _bmhd(W, H, 3, 0, 0)),
        _chunk(b"CMAP", b"".join(bytes(c) for c in PAL)),
        _chunk(b"ABIT", bytes(body))]))


# ------------------------------------------------------- Sun Raster con RLE
def gen_sun_rle():
    """Tipo 2: 0x80 <cnt> <val> repite val cnt+1 veces; 0x80 0x00 es un 0x80 literal."""
    rowb = ((W * 3) + 1) & ~1                      # 24 bits, filas a byte par
    raw = bytearray()
    for y in range(H):
        row = bytearray()
        for x in range(W):
            r, g, b = RGB[y][x]
            row += bytes((b, g, r))                # Sun guarda BGR
        raw += row + bytes(rowb - len(row))
    enc = bytearray()
    i = 0
    while i < len(raw):
        run = 1
        while i + run < len(raw) and run < 256 and raw[i + run] == raw[i]:
            run += 1
        if run >= 3:
            enc += bytes((0x80, run - 1, raw[i])); i += run
        elif raw[i] == 0x80:
            enc += b"\x80\x00"; i += 1
        else:
            enc.append(raw[i]); i += 1
    hdr = struct.pack(">8I", 0x59A66A95, W, H, 24, len(enc), 2, 0, 0)
    write("rle.ras", hdr + bytes(enc))


# ------------------------------------------------------------------- PCX/DCX
def _pcx_rle(rows):
    """RLE de PCX, fila por fila (un run nunca cruza la scanline)."""
    out = bytearray()
    for data in rows:
        i = 0
        while i < len(data):
            run = 1
            while i + run < len(data) and run < 63 and data[i + run] == data[i]:
                run += 1
            if run > 1 or data[i] >= 0xC0:
                out += bytes((0xC0 | run, data[i])); i += run
            else:
                out.append(data[i]); i += 1
    return bytes(out)


def _pcx(bpp, planes, bpl, rows, ega=None, vga=None):
    hdr = bytearray(128)
    hdr[0] = 0x0A; hdr[1] = 5; hdr[2] = 1; hdr[3] = bpp
    struct.pack_into("<HHHH", hdr, 4, 0, 0, W - 1, H - 1)
    struct.pack_into("<HH", hdr, 12, 72, 72)
    if ega:
        for i, c in enumerate(ega[:16]):
            hdr[16 + i*3: 19 + i*3] = bytes(c)
    hdr[65] = planes
    struct.pack_into("<H", hdr, 66, bpl)
    struct.pack_into("<H", hdr, 68, 1)
    body = bytes(hdr) + _pcx_rle(rows)
    if vga:
        body += b"\x0c" + b"".join(bytes(c) for c in vga) + b"\x00" * (768 - 3 * len(vga))
    return body


def gen_pcx():
    # 8 bits con paleta VGA al final
    rows8 = [bytes(IDX[y]) for y in range(H)]
    write("ref8.pcx", _pcx(8, 1, W, rows8, vga=PAL))
    # 4 bits (2 pixeles por byte) con la paleta EGA de la cabecera
    rows4 = []
    for y in range(H):
        row = bytearray(2)
        for x in range(W):
            row[x // 2] |= IDX[y][x] << (4 if x % 2 == 0 else 0)
        rows4.append(bytes(row))
    write("ref4.pcx", _pcx(4, 1, 2, rows4, ega=PAL + [(0, 0, 0)] * 8))
    # 1 bit: patron 1010 / 0101 con paleta negro-blanco
    rows1 = [bytes((0xA0,)), bytes((0x50,))]
    write("ref1.pcx", _pcx(1, 1, 1, rows1, ega=[(0, 0, 0), (255, 255, 255)] + [(0, 0, 0)] * 14))
    # DCX: magic + tabla de offsets (terminada en 0) + el PCX de 8 bits
    pcx = _pcx(8, 1, W, rows8, vga=PAL)
    write("ref.dcx", struct.pack("<III", 0x3ADE68B1, 12, 0) + pcx)


# ------------------------------------------------------------- Atari ST
# Paleta de 3 bits por canal: solo colores exactos (0 o 7 por componente).
ST_PAL = [(7,0,0), (0,7,0), (0,0,7), (7,7,7), (0,0,0), (7,7,0), (0,7,7), (7,0,7)]
ST_RGB = [(255,0,0), (0,255,0), (0,0,255), (255,255,255),
          (0,0,0), (255,255,0), (0,255,255), (255,0,255)]
ST_W, ST_H = 320, 200


def _st_idx(x, y):
    """Fila 0: los 8 colores en los primeros 8 pixeles. El resto, negro (indice 4)."""
    return (x % 8) if y == 0 else 4


def _st_palette_words():
    return b"".join(struct.pack(">H", (r << 8) | (g << 4) | b) for r, g, b in ST_PAL) \
           + b"".join(struct.pack(">H", 0) for _ in range(16 - len(ST_PAL)))


def _st_screen_words():
    """Bitplanes intercalados de a palabras: 4 words por cada 16 pixeles."""
    out = bytearray()
    for y in range(ST_H):
        for grp in range(ST_W // 16):
            words = [0] * 4
            for bit in range(16):
                x = grp * 16 + bit
                idx = _st_idx(x, y)
                for p in range(4):
                    if (idx >> p) & 1:
                        words[p] |= 1 << (15 - bit)
            for p in range(4):
                out += struct.pack(">H", words[p])
    return bytes(out)


def gen_atari():
    pal = _st_palette_words()
    write("ref.pi1", struct.pack(">H", 0) + pal + _st_screen_words())
    write("ref.neo", struct.pack(">HH", 0, 0) + pal + b"\x00" * (128 - 4 - 32) + _st_screen_words())
    # Degas Elite: PackBits sobre el layout "por scanline, plano tras plano"
    lines = bytearray()
    for y in range(ST_H):
        for p in range(4):
            plane = bytearray(ST_W // 8)
            for x in range(ST_W):
                if (_st_idx(x, y) >> p) & 1:
                    plane[x // 8] |= 0x80 >> (x & 7)
            lines += plane
    write("ref.pc1", struct.pack(">H", 0x8000) + pal + _packbits(bytes(lines)))


# --------------------------------------------------------------- ZX Spectrum
def gen_zx():
    """6912 B. Pixeles 0xF0 (4 tinta + 4 papel) y atributo tinta=2 papel=5 bright."""
    px = bytearray(6144)
    for i in range(6144):
        px[i] = 0xF0
    attr = bytes([2 | (5 << 3) | 0x40] * 768)      # tinta roja, papel cian, brillo
    write("ref.scr", bytes(px) + attr)


# ------------------------------------------------------------- C64 Koala
def gen_koala():
    """El byte 0x1B da los 4 pares 00,01,10,11: fondo, pantalla-alto, pantalla-bajo, color."""
    bmp = bytearray(8000)
    for i in range(8000):
        bmp[i] = 0x1B
    scr = bytes([0x12] * 1000)                     # alto=1 (blanco), bajo=2 (rojo)
    col = bytes([0x03] * 1000)                     # cian
    write("ref.koa", struct.pack("<H", 0x6000) + bytes(bmp) + scr + col + b"\x00")


# ----------------------------------------------------------------- GEM IMG
def gen_gem():
    hdr = struct.pack(">8H", 1, 8, 1, 2, 372, 372, 16, 2)
    data = b"\x80\x02\xF0\x0F" + b"\x02"           # literal de 2 bytes; run solido de 2 ceros
    write("ref.img", hdr + data)


# ------------------------------------------------------------- PlayStation TIM
def gen_tim():
    clut = [(31,0,0,1), (0,31,0,1), (0,0,31,1), (31,31,31,1),
            (0,0,0,1), (31,31,0,1), (0,31,31,1), (0,0,0,0)]   # el ultimo: negro transparente
    cols = b"".join(struct.pack("<H", r | (g << 5) | (b << 10) | (s << 15)) for r, g, b, s in clut)
    clut_blk = struct.pack("<IHHHH", 12 + len(cols), 0, 0, 8, 1) + cols
    px = bytearray()
    for y in range(H):
        for x in range(W):
            px.append(IDX[y][x])
        px += bytes(4)                              # relleno hasta 8 bytes de fila
    img_blk = struct.pack("<IHHHH", 12 + len(px), 0, 0, 4, H) + bytes(px)
    write("ref.tim", struct.pack("<II", 0x10, 0x09) + clut_blk + img_blk)


# --------------------------------------------------------------- Alias PIX
def gen_pix():
    body = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = RGB[y][x]
            body += bytes((1, b, g, r))            # RLE de 1 pixel: cuenta + BGR
    write("ref.pix", struct.pack(">5H", W, H, 0, 0, 24) + bytes(body))


# ------------------------------------------------------------ DDS / VTF / KTX
def _bc1_block():
    """4x4: fila 0 = c0 (rojo), fila 1 = c1 (azul), filas 2 y 3 = los interpolados."""
    bits = 0
    for y in range(4):
        for x in range(4):
            bits |= y << ((y * 4 + x) * 2)
    return struct.pack("<HHI", 0xF800, 0x001F, bits)


def _bc3_block():
    """Alfa: a0=255 a1=0; filas 0-1 opacas (indice 0), filas 2-3 transparentes (indice 1)."""
    idx = 0
    for i in range(16):
        idx |= (0 if i < 8 else 1) << (i * 3)
    alpha = bytes((255, 0)) + idx.to_bytes(6, "little")
    return alpha + _bc1_block()


def gen_dds():
    def dds(fourcc, data, w=4, h=4):
        hdr = bytearray(128)
        hdr[0:4] = b"DDS "
        struct.pack_into("<I", hdr, 4, 124)
        struct.pack_into("<I", hdr, 8, 0x1007)      # caps|height|width|pixelformat
        struct.pack_into("<I", hdr, 12, h)
        struct.pack_into("<I", hdr, 16, w)
        struct.pack_into("<I", hdr, 20, len(data))
        struct.pack_into("<I", hdr, 76, 32)         # tamaño del pixelformat
        struct.pack_into("<I", hdr, 80, 4)          # DDPF_FOURCC
        hdr[84:88] = fourcc
        struct.pack_into("<I", hdr, 108, 0x1000)    # DDSCAPS_TEXTURE
        return bytes(hdr) + data
    write("bc1.dds", dds(b"DXT1", _bc1_block()))
    write("bc3.dds", dds(b"DXT5", _bc3_block()))

    # VTF 7.2 con un solo mip DXT1: el nivel 0 es lo ultimo del archivo
    vtf = bytearray(80)
    vtf[0:4] = b"VTF\x00"
    struct.pack_into("<II", vtf, 4, 7, 2)
    struct.pack_into("<I", vtf, 12, 80)
    struct.pack_into("<HH", vtf, 16, 4, 4)
    struct.pack_into("<I", vtf, 20, 0)
    struct.pack_into("<HH", vtf, 24, 1, 0)
    struct.pack_into("<I", vtf, 52, 13)             # IMAGE_FORMAT_DXT1
    vtf[56] = 1                                     # un solo mip
    struct.pack_into("<I", vtf, 57, 0xFFFFFFFF)     # sin miniatura de baja resolucion
    write("ref.vtf", bytes(vtf) + _bc1_block())

    # KTX 1.1 con DXT1 (OpenGL: la imagen va de abajo hacia arriba)
    ktx = bytearray()
    ktx += bytes((0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A))
    ktx += struct.pack("<I", 0x04030201)
    ktx += struct.pack("<IIIII", 0, 1, 0, 0x83F0, 0x1907)   # type, typeSize, format, internal, base
    ktx += struct.pack("<IIIIIII", 4, 4, 0, 0, 1, 1, 0)     # w, h, depth, arrays, faces, mips, kv
    blk = _bc1_block()
    ktx += struct.pack("<I", len(blk)) + blk
    write("ref.ktx", bytes(ktx))


# ------------------------------------------------------------------ FITS
def gen_fits():
    def card(k, v=None):
        if v is None:
            return k.ljust(80).encode()
        return ("%-8s= %20s" % (k, v)).ljust(80).encode()
    hdr = (card("SIMPLE", "T") + card("BITPIX", "8") + card("NAXIS", "2")
           + card("NAXIS1", "4") + card("NAXIS2", "2") + card("END"))
    hdr += b" " * (2880 - len(hdr) % 2880)
    # FITS arranca por la fila de abajo: la 1a fila del archivo es la ultima de la imagen
    data = bytes((0, 85, 170, 255, 255, 170, 85, 0))
    data += b"\x00" * (2880 - len(data) % 2880)
    write("ref.fits", hdr + data)


# ----------------------------------------------------------------- DICOM
def _dcm_elem(group, elem, vr, value):
    if len(value) % 2:
        value += b"\x00" if vr in (b"UI", b"OB") else b" "
    b = struct.pack("<HH", group, elem) + vr
    if vr in (b"OB", b"OW", b"OF", b"SQ", b"UT", b"UN"):
        b += b"\x00\x00" + struct.pack("<I", len(value))
    else:
        b += struct.pack("<H", len(value))
    return b + value


def _dcm_impl(group, elem, value):
    if len(value) % 2:
        value += b"\x00"
    return struct.pack("<HHI", group, elem, len(value)) + value


def gen_dicom():
    px = bytes((0, 85, 170, 255, 255, 170, 85, 0))
    us = lambda v: struct.pack("<H", v)
    body = (_dcm_elem(0x0028, 0x0002, b"US", us(1))
            + _dcm_elem(0x0028, 0x0004, b"CS", b"MONOCHROME2")
            + _dcm_elem(0x0028, 0x0010, b"US", us(2))          # filas
            + _dcm_elem(0x0028, 0x0011, b"US", us(4))          # columnas
            + _dcm_elem(0x0028, 0x0100, b"US", us(8))
            + _dcm_elem(0x0028, 0x0101, b"US", us(8))
            + _dcm_elem(0x0028, 0x0103, b"US", us(0))
            + _dcm_elem(0x7FE0, 0x0010, b"OB", px))
    ts = b"1.2.840.10008.1.2.1\x00"
    meta = _dcm_elem(0x0002, 0x0010, b"UI", ts)
    meta = _dcm_elem(0x0002, 0x0000, b"UL", struct.pack("<I", len(meta))) + meta
    write("ref.dcm", b"\x00" * 128 + b"DICM" + meta + body)

    # variante en VR implicita (transfer syntax 1.2.840.10008.1.2), la del ACR-NEMA
    ibody = (_dcm_impl(0x0028, 0x0002, us(1))
             + _dcm_impl(0x0028, 0x0004, b"MONOCHROME1 ")      # invertida a proposito
             + _dcm_impl(0x0028, 0x0010, us(2))
             + _dcm_impl(0x0028, 0x0011, us(4))
             + _dcm_impl(0x0028, 0x0100, us(8))
             + _dcm_impl(0x0028, 0x0101, us(8))
             + _dcm_impl(0x0028, 0x0103, us(0))
             + _dcm_impl(0x7FE0, 0x0010, px))
    ts2 = b"1.2.840.10008.1.2\x00"
    meta2 = _dcm_elem(0x0002, 0x0010, b"UI", ts2)
    meta2 = _dcm_elem(0x0002, 0x0000, b"UL", struct.pack("<I", len(meta2))) + meta2
    write("impl.dcm", b"\x00" * 128 + b"DICM" + meta2 + ibody)


# --------------------------------------------- TIFF con orientacion EXIF
def gen_tiff_rot():
    """1200x700 en gris con Orientation=6: Lux tiene que mostrarlo girado (700x1200),
    o sea con la ventana mas alta que ancha. Tiene que ser bastante mas grande que
    el minimo de ventana (420x280 logicos) para que la prueba signifique algo.
    Va con PackBits para que el archivo no pese: cada fila es de un solo tono."""
    w2, h2 = 1200, 700
    data = b"".join(_packbits(bytes([y * 255 // (h2 - 1)] * w2)) for y in range(h2))
    ifd_off, n = 8, 10
    data_off = ifd_off + 2 + 12 * n + 4
    entries = [
        (256, 3, 1, w2),          # ImageWidth
        (257, 3, 1, h2),          # ImageLength
        (258, 3, 1, 8),           # BitsPerSample
        (259, 3, 1, 32773),       # Compression: PackBits
        (262, 3, 1, 1),           # PhotometricInterpretation: gris, 0 = negro
        (273, 4, 1, data_off),    # StripOffsets
        (274, 3, 1, 6),           # Orientation = 6 (girar 90 horario)
        (277, 3, 1, 1),           # SamplesPerPixel
        (278, 3, 1, h2),          # RowsPerStrip
        (279, 4, 1, len(data)),   # StripByteCounts
    ]
    ifd = struct.pack("<H", n)
    for tag, typ, cnt, val in entries:
        ifd += struct.pack("<HHII", tag, typ, cnt, val)
    ifd += struct.pack("<I", 0)
    write("rot90.tif", b"II\x2a\x00" + struct.pack("<I", ifd_off) + ifd + data)


# ------------------------------------------------------------------- WMF
def gen_wmf():
    """WMF placeable con un rectangulo rojo de 200x120: prueba el camino GDI."""
    def rec(func, params):
        return (struct.pack("<IH", 3 + len(params), func)
                + b"".join(struct.pack("<H", p & 0xFFFF) for p in params))
    body = (rec(0x020B, (0, 0))                 # SetWindowOrg (y, x)
            + rec(0x020C, (120, 200))           # SetWindowExt (y, x)
            + rec(0x02FC, (0, 0x00FF, 0, 0))    # CreateBrushIndirect: solido, rojo
            + rec(0x012D, (0,))                 # SelectObject
            + rec(0x041B, (120, 200, 0, 0))     # Rectangle (bottom, right, top, left)
            + rec(0x0000, ()))                  # fin del metarchivo
    hdr = struct.pack("<HHHIHIH", 1, 9, 0x0300, (18 + len(body)) // 2, 1, 7, 0)
    place = bytearray(struct.pack("<IHhhhhHIH", 0x9AC6CDD7, 0, 0, 0, 200, 120, 96, 0, 0))
    chk = 0
    for i in range(10):
        chk ^= struct.unpack_from("<H", place, i * 2)[0]
    struct.pack_into("<H", place, 20, chk)
    write("ref.wmf", bytes(place) + hdr + body)


# ------------------------------------------------------------------- XCF
def gen_xcf(ver, name):
    """XCF de una capa RGBA con los 8 colores, comprimido con el RLE de GIMP.
    ver=1 usa punteros de 32 bits; ver=11 los de 64 y agrega el campo precision."""
    p64 = ver >= 11
    def ptr(v):
        return struct.pack(">Q", v) if p64 else struct.pack(">I", v)
    psz = 8 if p64 else 4
    def prop(t, payload):
        return struct.pack(">II", t, len(payload)) + payload

    # tile con RLE: un plano por canal; cada plano entra en un literal corto
    planes = []
    for ch in range(4):
        vals = bytes([([RGB[y][x][0], RGB[y][x][1], RGB[y][x][2], 255][ch])
                      for y in range(H) for x in range(W)])
        planes.append(bytes((256 - len(vals),)) + vals)
    tile = b"".join(planes)

    magic = b"gimp xcf file\x00" if ver == 0 else ("gimp xcf v%03d\x00" % ver).encode()
    head = magic + struct.pack(">III", W, H, 0)
    if ver >= 4:
        head += struct.pack(">I", 150)          # precision: 8 bits con gamma
    head += prop(17, b"\x01") + prop(0, b"")    # compresion RLE + fin
    lists_len = psz * 2 + psz                   # [capa, 0] + [0]
    layer_off = len(head) + lists_len

    lname = b"capa\x00"
    layer = struct.pack(">III", W, H, 1) + struct.pack(">I", len(lname)) + lname
    layer += (prop(6, struct.pack(">I", 255)) + prop(8, struct.pack(">I", 1))
              + prop(15, struct.pack(">ii", 0, 0)) + prop(0, b""))
    hier_off = layer_off + len(layer) + psz * 2
    layer += ptr(hier_off) + ptr(0)
    lvl_off = hier_off + 12 + psz * 2
    hier = struct.pack(">III", W, H, 4) + ptr(lvl_off) + ptr(0)
    tile_off = lvl_off + 8 + psz * 2
    lvl = struct.pack(">II", W, H) + ptr(tile_off) + ptr(0)
    write(name, head + ptr(layer_off) + ptr(0) + ptr(0) + layer + hier + lvl + tile)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("Generando muestras en %s" % OUT)
    gen_pnm(); gen_xpm(); gen_ilbm(); gen_macpaint(); gen_xwd()
    gen_dpx(); gen_icns(); gen_containers()
    gen_acbm(); gen_sun_rle(); gen_pcx(); gen_atari(); gen_zx(); gen_koala()
    gen_gem(); gen_tim(); gen_pix(); gen_dds(); gen_fits(); gen_dicom()
    gen_tiff_rot(); gen_wmf()
    gen_xcf(1, "v1.xcf"); gen_xcf(11, "v11.xcf")
    print("Listo.")
