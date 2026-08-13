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


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    print("Generando muestras en %s" % OUT)
    gen_pnm(); gen_xpm(); gen_ilbm(); gen_macpaint(); gen_xwd()
    gen_dpx(); gen_icns(); gen_containers()
    print("Listo.")
