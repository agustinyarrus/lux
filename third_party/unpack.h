// unpack.h — desenvoltura de contenedores comprimidos (gzip y ZIP), header-only.
// Son operaciones puras sobre buffers: no tocan Direct2D ni WIC, asi que se pueden
// probar sueltas. Reusa el inflate que ya trae stb_image (incluilo antes que esto).
#pragma once
#include <vector>
#include <string.h>
#include <algorithm>

// deflate crudo (sin cabecera zlib) -> buffer nuevo. `hint` es el tamano esperado.
static bool up_inflate_raw(const unsigned char* src, size_t len, size_t hint,
                           std::vector<unsigned char>& out) {
    if (!src || !len || len > INT_MAX) return false;
    size_t guess = std::min<size_t>(std::max<size_t>(hint, 4096), 64u << 20);
    int olen = 0;
    char* p = stbi_zlib_decode_malloc_guesssize_headerflag(
        (const char*)src, (int)len, (int)guess, &olen, 0);
    if (!p) return false;
    if (olen > 0) out.assign((unsigned char*)p, (unsigned char*)p + olen);
    STBI_FREE(p);
    return olen > 0;
}

// Saca la envoltura gzip (RFC 1952) y descomprime el deflate que hay adentro.
static bool up_gunzip(const std::vector<unsigned char>& in, std::vector<unsigned char>& out) {
    if (in.size() < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return false;
    unsigned flags = in[3];
    size_t i = 10;
    if (flags & 4) {                                   // campo extra
        if (i + 2 > in.size()) return false;
        i += 2 + (size_t)(in[i] | (in[i+1] << 8));
    }
    if (flags & 8)  { while (i < in.size() && in[i]) i++; i++; }   // nombre original
    if (flags & 16) { while (i < in.size() && in[i]) i++; i++; }   // comentario
    if (flags & 2)  i += 2;                                        // CRC16 del header
    if (i + 8 >= in.size()) return false;
    // los ultimos 4 bytes del gzip son el tamano original: sirve de hint exacto
    const unsigned char* t = in.data() + in.size() - 4;
    size_t isize = (size_t)t[0] | ((size_t)t[1] << 8) | ((size_t)t[2] << 16) | ((size_t)t[3] << 24);
    return up_inflate_raw(in.data() + i, in.size() - 8 - i, isize ? isize : 65536, out);
}

// ANI (cursor animado): RIFF 'ACON' cuya lista 'fram' guarda un .ico/.cur completo
// por cuadro. Devuelve el rango del primer cuadro dentro del mismo buffer.
static bool up_ani_frame(const std::vector<unsigned char>& b, size_t* off, size_t* len) {
    if (b.size() < 24 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "ACON", 4))
        return false;
    auto le32 = [&](size_t o) { return (size_t)b[o] | ((size_t)b[o+1] << 8) |
                                       ((size_t)b[o+2] << 16) | ((size_t)b[o+3] << 24); };
    for (size_t p = 12; p + 8 <= b.size(); ) {
        const unsigned char* id = b.data() + p;
        size_t sz = le32(p + 4);
        if (sz > b.size() - p - 8) sz = b.size() - p - 8;
        if (!memcmp(id, "LIST", 4)) { p += 12; continue; }   // entrar en la lista, no saltarla
        if (!memcmp(id, "icon", 4) && sz >= 8) { *off = p + 8; *len = sz; return true; }
        p += 8 + sz + (sz & 1);                              // los chunks RIFF van a byte par
    }
    return false;
}

// Busca una entrada del ZIP por nombre (sin distinguir mayusculas) y la descomprime.
// Soporta los dos metodos que importan: stored (0) y deflate (8).
static bool up_zip_extract(const std::vector<unsigned char>& z, const char* want,
                           std::vector<unsigned char>& out) {
    if (z.size() < 22 || !want) return false;
    auto le16 = [&](size_t o) { return (unsigned)z[o] | ((unsigned)z[o+1] << 8); };
    auto le32 = [&](size_t o) { return (size_t)z[o] | ((size_t)z[o+1] << 8) |
                                       ((size_t)z[o+2] << 16) | ((size_t)z[o+3] << 24); };
    // el End Of Central Directory vive al final (puede haber hasta 64 KB de comentario)
    size_t eocd = 0; bool found = false;
    size_t lo = z.size() > 65557 ? z.size() - 65557 : 0;
    for (size_t i = z.size() - 21; i-- > lo; )
        if (z[i] == 0x50 && z[i+1] == 0x4B && z[i+2] == 0x05 && z[i+3] == 0x06) { eocd = i; found = true; break; }
    if (!found || eocd + 22 > z.size()) return false;

    size_t n = le16(eocd + 10), cd = le32(eocd + 16), wl = strlen(want);
    for (size_t k = 0, p = cd; k < n && p + 46 <= z.size(); k++) {
        if (z[p] != 0x50 || z[p+1] != 0x4B || z[p+2] != 0x01 || z[p+3] != 0x02) break;
        unsigned method = le16(p + 10);
        size_t csz = le32(p + 20), usz = le32(p + 24);
        size_t nl = le16(p + 28), el = le16(p + 30), cl = le16(p + 32), lho = le32(p + 42);
        if (p + 46 + nl > z.size()) break;
        bool hit = (nl == wl && !_strnicmp((const char*)z.data() + p + 46, want, wl));
        p += 46 + nl + el + cl;
        if (!hit) continue;

        if (lho + 30 > z.size()) return false;
        if (z[lho] != 0x50 || z[lho+1] != 0x4B || z[lho+2] != 0x03 || z[lho+3] != 0x04) return false;
        size_t dof = lho + 30 + le16(lho + 26) + le16(lho + 28);
        if (!csz || dof + csz > z.size()) return false;
        if (method == 0) { out.assign(z.begin() + dof, z.begin() + dof + csz); return true; }
        if (method == 8) return up_inflate_raw(z.data() + dof, csz, usz ? usz : csz * 4, out);
        return false;                                  // bzip2/lzma/zstd: no
    }
    return false;
}
