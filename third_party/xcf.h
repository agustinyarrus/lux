// xcf.h — decoder header-only del formato nativo de GIMP (.xcf).
// Mismo contrato que exotic.h: RGBA8 con malloc (el caller libera) o NULL.
// Se incluye DESPUES de exotic.h (usa ex_be32/ex_ok).
//
// El XCF no guarda la imagen aplanada: hay que componer las capas visibles a
// mano, de abajo hacia arriba, en modo normal y respetando opacidad y offsets.
// Cubre v0 a v13, precision de 8 y 16 bits, RGB/gris/indexado, y los tres modos
// de almacenamiento de tiles (crudo, el RLE de GIMP y zlib).
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Inflate (zlib con cabecera) para los tiles comprimidos: lo cablea el host.
// Devuelve los bytes escritos o <0. Sin esto, los XCF con zlib se descartan.
typedef int (*xcf_inflate_fn)(unsigned char* dst, int dlen, const unsigned char* src, int slen);
static xcf_inflate_fn xcf_inflate = NULL;

#define XCF_TILE 64

typedef struct {
    const unsigned char* d;
    size_t n;
    int ver;          // 0 = "file", si no el numero de vNNN
    int off64;        // v11+ usa punteros de 64 bits
    int compress;     // 0 crudo, 1 RLE, 2 zlib
    const unsigned char* cmap;
    int ncmap;
} xcf_ctx;

static uint64_t xcf_ptr(xcf_ctx* c, size_t at){
    if(at + (c->off64 ? 8u : 4u) > c->n) return 0;
    if(!c->off64) return ex_be32(c->d + at);
    return ((uint64_t)ex_be32(c->d + at) << 32) | ex_be32(c->d + at + 4);
}
static size_t xcf_psz(xcf_ctx* c){ return c->off64 ? 8 : 4; }

// Un tile: pixeles intercalados (crudo/zlib) o un plano atras de otro (RLE).
// Sale siempre como bpp bytes por pixel, intercalado. El RLE de GIMP es:
//   op 0..126 -> run de op+1 del byte siguiente
//   op 127    -> run largo: dos bytes de longitud y despues el valor
//   op 128    -> literal largo: dos bytes de longitud y despues los bytes
//   op 129..255 -> literal corto de 256-op bytes
// Se decodifica plano por plano y hay que ir contando lo consumido, porque los
// planos comparten un unico flujo.
static int xcf_tile(xcf_ctx* c, size_t at, int tw, int th, int bpp, unsigned char* out){
    size_t want = (size_t)tw * th * bpp;
    if(at >= c->n || !want) return 0;
    size_t avail = c->n - at;
    if(c->compress == 0){
        if(avail < want) return 0;
        memcpy(out, c->d + at, want);
        return 1;
    }
    if(c->compress == 2){
        if(!xcf_inflate) return 0;
        if(want > (size_t)INT_MAX || avail > (size_t)INT_MAX) return 0;
        return xcf_inflate(out, (int)want, c->d + at, (int)avail) == (int)want;
    }
    // RLE: los planos van completos, uno atras del otro
    unsigned char* plane = (unsigned char*)malloc((size_t)tw * th);
    if(!plane) return 0;
    const unsigned char* s = c->d + at;
    size_t used = 0, px = (size_t)tw * th;
    for(int b = 0; b < bpp; b++){
        size_t i = 0, o = 0;
        const unsigned char* sp = s + used;
        size_t slen = (avail > used) ? avail - used : 0;
        while(o < px){
            if(i >= slen){ free(plane); return 0; }
            int op = sp[i++];
            size_t len;
            if(op < 127){ len = (size_t)op + 1; if(i >= slen){ free(plane); return 0; }
                unsigned char v = sp[i++]; size_t k = len > px - o ? px - o : len;
                memset(plane + o, v, k); o += k; }
            else if(op == 127){ if(i + 3 > slen){ free(plane); return 0; }
                len = ((size_t)sp[i] << 8) | sp[i+1]; i += 2; unsigned char v = sp[i++];
                if(!len){ free(plane); return 0; }
                size_t k = len > px - o ? px - o : len; memset(plane + o, v, k); o += k; }
            else if(op == 128){ if(i + 2 > slen){ free(plane); return 0; }
                len = ((size_t)sp[i] << 8) | sp[i+1]; i += 2;
                if(!len || i + len > slen){ free(plane); return 0; }
                size_t k = len > px - o ? px - o : len; memcpy(plane + o, sp + i, k);
                i += len; o += k; }
            else { len = (size_t)256 - op; if(i + len > slen){ free(plane); return 0; }
                size_t k = len > px - o ? px - o : len; memcpy(plane + o, sp + i, k);
                i += len; o += k; }
        }
        used += i;
        for(size_t k = 0; k < px; k++) out[k * bpp + b] = plane[k];
    }
    free(plane);
    return 1;
}

// Lee una jerarquia (el nivel 0, que es la resolucion completa) a RGBA8.
// `type` es el tipo de capa de GIMP: 0 RGB, 1 RGBA, 2 gris, 3 gris+alfa, 4/5 indexado.
static unsigned char* xcf_hierarchy(xcf_ctx* c, uint64_t hp, int lw, int lh, int type){
    if(!hp || hp + 12 > c->n) return NULL;
    uint32_t hw = ex_be32(c->d + hp), hh = ex_be32(c->d + hp + 4), bpp = ex_be32(c->d + hp + 8);
    if((int)hw != lw || (int)hh != lh || bpp < 1 || bpp > 16) return NULL;
    uint64_t lp = xcf_ptr(c, (size_t)hp + 12);
    if(!lp || lp + 8 > c->n) return NULL;
    uint32_t levw = ex_be32(c->d + lp), levh = ex_be32(c->d + lp + 4);
    if((int)levw != lw || (int)levh != lh) return NULL;

    int chans = (type == 0) ? 3 : (type == 1) ? 4 : (type == 2) ? 1 : (type == 3) ? 2 :
                (type == 4) ? 1 : 2;
    int bpc = (int)bpp / chans;
    if(bpc < 1 || bpc > 4 || bpc * chans != (int)bpp) return NULL;

    size_t tilesX = ((size_t)lw + XCF_TILE - 1) / XCF_TILE;
    size_t tilesY = ((size_t)lh + XCF_TILE - 1) / XCF_TILE;
    unsigned char* out = (unsigned char*)calloc((size_t)lw * lh * 4, 1);
    unsigned char* tile = (unsigned char*)malloc((size_t)XCF_TILE * XCF_TILE * bpp);
    if(!out || !tile){ free(out); free(tile); return NULL; }

    size_t tp = (size_t)lp + 8;
    for(size_t ty = 0; ty < tilesY; ty++){
        for(size_t tx = 0; tx < tilesX; tx++, tp += xcf_psz(c)){
            uint64_t at = xcf_ptr(c, tp);
            int tw = (int)((tx + 1) * XCF_TILE <= (size_t)lw ? XCF_TILE : lw - tx * XCF_TILE);
            int th = (int)((ty + 1) * XCF_TILE <= (size_t)lh ? XCF_TILE : lh - ty * XCF_TILE);
            if(!at || !xcf_tile(c, (size_t)at, tw, th, (int)bpp, tile)) continue;  // tile vacio
            for(int y = 0; y < th; y++) for(int x = 0; x < tw; x++){
                const unsigned char* s = tile + ((size_t)y * tw + x) * bpp;
                unsigned char* o = out + (((size_t)ty * XCF_TILE + y) * lw + tx * XCF_TILE + x) * 4;
                unsigned char v[4] = {0,0,0,255};
                for(int ch = 0; ch < chans && ch < 4; ch++) v[ch] = s[ch * bpc];  // big endian: byte alto
                if(type == 0)      { o[0]=v[0]; o[1]=v[1]; o[2]=v[2]; o[3]=255; }
                else if(type == 1) { o[0]=v[0]; o[1]=v[1]; o[2]=v[2]; o[3]=v[3]; }
                else if(type == 2) { o[0]=o[1]=o[2]=v[0]; o[3]=255; }
                else if(type == 3) { o[0]=o[1]=o[2]=v[0]; o[3]=v[1]; }
                else {                                        // indexado
                    int idx = v[0];
                    if(c->cmap && idx < c->ncmap){ o[0]=c->cmap[idx*3]; o[1]=c->cmap[idx*3+1]; o[2]=c->cmap[idx*3+2]; }
                    else o[0]=o[1]=o[2]=(unsigned char)idx;
                    o[3] = (type == 5) ? v[1] : 255;
                }
            }
        }
    }
    free(tile);
    return out;
}

static unsigned char* xcf_load(const unsigned char* d, size_t n, const char* ext, int* w, int* h){
    (void)ext;
    if(!d || n < 32 || memcmp(d, "gimp xcf ", 9)) return NULL;
    xcf_ctx c; memset(&c, 0, sizeof(c));
    c.d = d; c.n = n; c.compress = 1;
    if(!memcmp(d + 9, "file", 4)) c.ver = 0;
    else if(d[9] == 'v' && d[10] >= '0' && d[10] <= '9')
        c.ver = (d[10]-'0')*100 + (d[11]-'0')*10 + (d[12]-'0');
    else return NULL;
    if(d[13] != 0) return NULL;
    c.off64 = (c.ver >= 11);

    size_t p = 14;
    uint32_t W = ex_be32(d+p), H = ex_be32(d+p+4), base = ex_be32(d+p+8);
    p += 12;
    if(c.ver >= 4){
        if(p + 4 > n) return NULL;
        uint32_t prec = ex_be32(d+p); p += 4;
        // coma flotante (half/float/double): no lo interpretamos
        if(c.ver >= 7 ? (prec >= 500) : (prec >= 3)) return NULL;
    }
    if(!ex_ok(W, H) || base > 2) return NULL;

    // propiedades de la imagen: nos interesan la paleta y el modo de compresion
    while(p + 8 <= n){
        uint32_t type = ex_be32(d+p), len = ex_be32(d+p+4);
        p += 8;
        if(type == 0) break;                           // PROP_END
        if(len > n - p) return NULL;
        if(type == 1 && len >= 4){                     // PROP_COLORMAP
            uint32_t nc = ex_be32(d+p);
            if(nc <= 256 && 4 + (uint64_t)nc*3 <= len){ c.cmap = d+p+4; c.ncmap = (int)nc; }
        } else if(type == 17 && len >= 1){             // PROP_COMPRESSION
            c.compress = d[p];
            if(c.compress > 2) return NULL;
        }
        p += len;
    }

    // lista de capas (de arriba hacia abajo), terminada en un puntero nulo
    size_t lp = p, count = 0;
    while(lp + xcf_psz(&c) <= n && xcf_ptr(&c, lp)){ count++; lp += xcf_psz(&c); }
    if(!count || count > 4096) return NULL;

    unsigned char* canvas = (unsigned char*)calloc((size_t)W * H * 4, 1);
    if(!canvas) return NULL;

    // componer de abajo hacia arriba: la ultima de la lista es la de mas atras
    for(size_t li = count; li-- > 0; ){
        uint64_t off = xcf_ptr(&c, p + li * xcf_psz(&c));
        if(!off || off + 12 > n) continue;
        size_t q = (size_t)off;
        int lw = (int)ex_be32(d+q), lh = (int)ex_be32(d+q+4), type = (int)ex_be32(d+q+8);
        q += 12;
        if(lw <= 0 || lh <= 0 || type < 0 || type > 5) continue;
        if((uint64_t)lw * lh > 200000000ull) continue;
        if(q + 4 > n) continue;
        uint32_t nameLen = ex_be32(d+q); q += 4;        // nombre (largo + bytes)
        if(nameLen > n - q) continue;
        q += nameLen;

        int visible = 1, opacity = 255, ox = 0, oy = 0;
        while(q + 8 <= n){
            uint32_t type2 = ex_be32(d+q), len = ex_be32(d+q+4);
            q += 8;
            if(type2 == 0) break;
            if(len > n - q) { q = n; break; }
            if(type2 == 6 && len >= 4)  opacity = (int)ex_be32(d+q);            // PROP_OPACITY
            else if(type2 == 8 && len >= 4) visible = ex_be32(d+q) != 0;        // PROP_VISIBLE
            else if(type2 == 15 && len >= 8){                                   // PROP_OFFSETS
                ox = (int)(int32_t)ex_be32(d+q); oy = (int)(int32_t)ex_be32(d+q+4);
            }
            q += len;
        }
        if(!visible || opacity <= 0) continue;
        if(q + xcf_psz(&c) > n) continue;
        uint64_t hp = xcf_ptr(&c, q);
        unsigned char* lay = xcf_hierarchy(&c, hp, lw, lh, type);
        if(!lay) continue;

        for(int y = 0; y < lh; y++){
            int cy = y + oy; if(cy < 0 || cy >= (int)H) continue;
            for(int x = 0; x < lw; x++){
                int cx = x + ox; if(cx < 0 || cx >= (int)W) continue;
                const unsigned char* s = lay + ((size_t)y * lw + x) * 4;
                unsigned char* o = canvas + ((size_t)cy * W + cx) * 4;
                int a = s[3] * opacity / 255;
                if(!a) continue;
                if(a == 255){ o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=255; continue; }
                int ia = 255 - a;
                int na = a + o[3] * ia / 255;
                for(int k = 0; k < 3; k++)
                    o[k] = (unsigned char)(na ? (s[k]*a + o[k]*o[3]*ia/255) / na : 0);
                o[3] = (unsigned char)na;
            }
        }
        free(lay);
    }
    *w = (int)W; *h = (int)H;
    return canvas;
}
