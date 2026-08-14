// retro.h — decoders header-only para formatos de las computadoras de 8 y 16 bits
// y de las consolas. Mismo contrato que exotic.h: RGBA8 con malloc (el caller
// libera con free) o NULL. Se incluye DESPUES de exotic.h: reusa sus helpers
// (ex_be32/ex_le16/ex_ok) y su decoder de PCX para el contenedor DCX.
//
// Formatos: Degas y Degas Elite (Atari ST), NEOchrome, Tiny Stuff, ZX Spectrum
//           SCR, Commodore 64 Koala, GEM IMG/XIMG, PlayStation TIM, Alias PIX,
//           DCX (PCX multipagina) y Kodak Photo CD.
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
//  Atari ST: paleta y bitplanes
// ---------------------------------------------------------------------------
// Palabra de color del ST: 0x0RGB con 3 bits utiles por canal (0-7). El STE
// sumo un 4o bit, pero lo metio ABAJO de todo (el bit 3 es el menos
// significativo), asi que hay que rotarlo. Si alguna componente pasa de 7 el
// archivo es STE.
static void rt_st_palette(const unsigned char* p, int n, unsigned char pal[][3]){
    int ste=0;
    for(int i=0;i<n;i++){
        uint16_t w=ex_be16(p+i*2);
        if(((w>>8)&0xF)>7 || ((w>>4)&0xF)>7 || (w&0xF)>7) ste=1;
    }
    for(int i=0;i<n;i++){
        uint16_t w=ex_be16(p+i*2);
        int c[3]={ (w>>8)&0xF, (w>>4)&0xF, w&0xF };
        for(int k=0;k<3;k++){
            int v=c[k];
            if(ste){ v=((v&7)<<1)|((v>>3)&1); pal[i][k]=(unsigned char)(v*17); }
            else   { v&=7;                    pal[i][k]=(unsigned char)(v*255/7); }
        }
    }
}
// Bitplanes intercalados de a palabras (el formato de la memoria de video del
// ST): cada grupo de 16 pixeles son `planes` palabras seguidas.
static void rt_planar_words(const unsigned char* s, int W, int H, int planes,
                            unsigned char pal[][3], unsigned char* out){
    size_t rowb=(size_t)(W/16)*2*planes;
    for(int y=0;y<H;y++){
        const unsigned char* row=s+(size_t)y*rowb;
        for(int x=0;x<W;x++){
            int grp=x>>4, bit=15-(x&15), idx=0;
            for(int p=0;p<planes;p++)
                if((ex_be16(row+((size_t)grp*planes+p)*2)>>bit)&1) idx|=1<<p;
            unsigned char* o=out+((size_t)y*W+x)*4;
            o[0]=pal[idx][0]; o[1]=pal[idx][1]; o[2]=pal[idx][2]; o[3]=255;
        }
    }
}
// Bitplanes por linea (lo que sale de descomprimir un Degas Elite): para cada
// scanline van los `planes` planos enteros, uno atras del otro.
static void rt_planar_lines(const unsigned char* s, int W, int H, int planes,
                            unsigned char pal[][3], unsigned char* out){
    size_t lb=(size_t)W/8;
    for(int y=0;y<H;y++){
        const unsigned char* row=s+(size_t)y*lb*planes;
        for(int x=0;x<W;x++){
            int idx=0;
            for(int p=0;p<planes;p++)
                if((row[(size_t)p*lb + x/8]>>(7-(x&7)))&1) idx|=1<<p;
            unsigned char* o=out+((size_t)y*W+x)*4;
            o[0]=pal[idx][0]; o[1]=pal[idx][1]; o[2]=pal[idx][2]; o[3]=255;
        }
    }
}
static int rt_res_dims(int res, int* W, int* H, int* planes){
    if(res==0){ *W=320; *H=200; *planes=4; return 1; }
    if(res==1){ *W=640; *H=200; *planes=2; return 1; }
    if(res==2){ *W=640; *H=400; *planes=1; return 1; }
    return 0;
}
// PackBits clasico (el mismo de ILBM/MacPaint) hacia un buffer de tamano fijo.
static void rt_packbits(const unsigned char* s, size_t n, unsigned char* dst, size_t want){
    size_t o=0,i=0;
    while(o<want && i<n){
        signed char c=(signed char)s[i++];
        if(c>=0){ int cnt=c+1; while(cnt-- && o<want && i<n) dst[o++]=s[i++]; }
        else if(c!=-128){ int cnt=-c+1; if(i>=n) break; unsigned char v=s[i++]; while(cnt-- && o<want) dst[o++]=v; }
    }
    while(o<want) dst[o++]=0;
}

// ---------------------------------------------------------------- Degas (.pi1/.pi2/.pi3)
// 32034 bytes: palabra de resolucion + 16 palabras de paleta + 32000 de pantalla.
static unsigned char* rt_degas(const unsigned char* d, size_t n, int* w, int* h){
    if(n<34) return NULL;
    uint16_t rw=ex_be16(d);
    int comp=(rw&0x8000)!=0, res=rw&3;
    if((rw & 0x7FFC)!=0) return NULL;                  // el resto de los bits va en cero
    int W,H,planes; if(!rt_res_dims(res,&W,&H,&planes)) return NULL;
    unsigned char pal[16][3]; rt_st_palette(d+2,16,pal);
    if(res==2 && !memcmp(pal[0],pal[1],3)){            // alta resolucion sin paleta util
        pal[0][0]=pal[0][1]=pal[0][2]=255; pal[1][0]=pal[1][1]=pal[1][2]=0;
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    if(!comp){
        if(n<34+(size_t)32000){ free(out); return NULL; }
        rt_planar_words(d+34,W,H,planes,pal,out);
    } else {
        size_t need=(size_t)(W/8)*planes*H;
        unsigned char* raw=(unsigned char*)malloc(need);
        if(!raw){ free(out); return NULL; }
        rt_packbits(d+34,n-34,raw,need);
        rt_planar_lines(raw,W,H,planes,pal,out);
        free(raw);
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- NEOchrome (.neo)
// 32128 bytes: cabecera de 128 (flag, resolucion, paleta, nombre…) + pantalla.
static unsigned char* rt_neo(const unsigned char* d, size_t n, int* w, int* h){
    if(n<128+32000 || ex_be16(d)!=0) return NULL;
    int res=ex_be16(d+2); int W,H,planes;
    if(!rt_res_dims(res,&W,&H,&planes)) return NULL;
    unsigned char pal[16][3]; rt_st_palette(d+4,16,pal);
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    rt_planar_words(d+128,W,H,planes,pal,out);
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- ZX Spectrum (.scr)
// 6912 bytes exactos: 6144 de pixeles con el famoso direccionamiento entrelazado
// (tercio / linea / fila) y 768 de atributos (tinta, papel, brillo) por celda de 8x8.
static unsigned char* rt_zxscr(const unsigned char* d, size_t n, int* w, int* h){
    // el tamano es la unica firma que tiene: 6912 exactos (o con un colgajo chico).
    // Asi ademas no confundimos un salvapantallas .scr con una pantalla del ZX.
    if(n<6912 || n>7040) return NULL;
    static const unsigned char zx[8][3]={{0,0,0},{0,0,215},{215,0,0},{215,0,215},
                                         {0,215,0},{0,215,215},{215,215,0},{215,215,215}};
    const int W=256,H=192;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(int y=0;y<H;y++){
        size_t addr=(size_t)(((y&0xC0)<<5)|((y&0x07)<<8)|((y&0x38)<<2));
        const unsigned char* attr=d+6144+(size_t)(y/8)*32;
        for(int x=0;x<W;x++){
            unsigned char bits=d[addr+(x>>3)];
            unsigned char a=attr[x>>3];
            int on=(bits>>(7-(x&7)))&1;
            int c=on ? (a&7) : ((a>>3)&7);
            int bright=(a>>6)&1;
            unsigned char* o=out+((size_t)y*W+x)*4;
            for(int k=0;k<3;k++) o[k]=(unsigned char)(bright && zx[c][k] ? 255 : zx[c][k]);
            o[3]=255;
        }
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- C64 Koala (.koa/.kla)
// 10003 bytes: direccion de carga + bitmap + RAM de pantalla + RAM de color + fondo.
// Multicolor: 160 pixeles de doble ancho, se duplican para dar 320x200.
static unsigned char* rt_koala(const unsigned char* d, size_t n, int* w, int* h){
    if(n<10003) return NULL;
    static const unsigned char c64[16][3]={
        {0,0,0},{255,255,255},{104,55,43},{112,164,178},{111,61,134},{88,141,67},
        {53,40,121},{184,199,111},{111,79,37},{67,57,0},{154,103,89},{68,68,68},
        {108,108,108},{154,210,132},{108,94,181},{149,149,149}};
    const unsigned char* bmp=d+2, *scr=d+2+8000, *colr=d+2+9000; unsigned char bg=d[10002]&15;
    const int W=320,H=200;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(int y=0;y<H;y++) for(int px=0;px<160;px++){
        int cx=px/4, cy=y/8;
        unsigned char b=bmp[((size_t)cy*40+cx)*8 + (y&7)];
        int pair=(b>>(6-2*(px&3)))&3;
        int ci;
        if(pair==0)      ci=bg;
        else if(pair==1) ci=scr[(size_t)cy*40+cx]>>4;
        else if(pair==2) ci=scr[(size_t)cy*40+cx]&15;
        else             ci=colr[(size_t)cy*40+cx]&15;
        for(int k=0;k<2;k++){
            unsigned char* o=out+((size_t)y*W+px*2+k)*4;
            o[0]=c64[ci][0]; o[1]=c64[ci][1]; o[2]=c64[ci][2]; o[3]=255;
        }
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- GEM IMG / XIMG
// El formato de imagen de GEM (Atari/Ventura Publisher). Cabecera en palabras BE
// y un RLE con tres casos: run solido, run literal (0x80) y repeticion de la
// scanline anterior (00 00 FF nn). XIMG agrega una paleta en 0..1000 por canal.
static unsigned char* rt_gem(const unsigned char* d, size_t n, int* w, int* h){
    if(n<16) return NULL;
    int ver=ex_be16(d), hlen=ex_be16(d+2), planes=ex_be16(d+4), patlen=ex_be16(d+6);
    int W=ex_be16(d+12), H=ex_be16(d+14);
    if(ver>2 || hlen<8 || hlen>1024 || patlen<1 || patlen>8) return NULL;
    if(planes!=1 && planes!=2 && planes!=4 && planes!=8) return NULL;
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    size_t hdr=(size_t)hlen*2; if(hdr>=n) return NULL;

    int ncol=1<<planes;
    unsigned char pal[256][3];
    if(planes==1){ pal[0][0]=pal[0][1]=pal[0][2]=255; pal[1][0]=pal[1][1]=pal[1][2]=0; }
    else {  // paleta EGA/GEM por defecto (indices en el orden del VDI)
        static const unsigned char gem16[16][3]={
            {255,255,255},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{170,170,170},
            {85,85,85},{170,0,0},{0,170,0},{170,170,0},{0,0,170},{170,0,170},{0,170,170},{0,0,0}};
        for(int i=0;i<ncol && i<256;i++){ const unsigned char* c=gem16[i&15];
            pal[i][0]=c[0]; pal[i][1]=c[1]; pal[i][2]=c[2]; }
    }
    if(hlen>=11 && !memcmp(d+16,"XIMG",4)){          // paleta explicita (0..1000)
        const unsigned char* p=d+22;
        for(int i=0;i<ncol && i<256 && (size_t)(p-d)+6<=hdr; i++, p+=6)
            for(int k=0;k<3;k++) pal[i][k]=(unsigned char)(ex_be16(p+k*2)*255/1000);
    }

    size_t lb=((size_t)W+7)/8, plane=lb*planes;
    unsigned char* raw=(unsigned char*)calloc(plane*(size_t)H,1); if(!raw) return NULL;
    const unsigned char* p=d+hdr, *end=d+n;
    for(int y=0;y<H;){
        // repeticion de scanline: 00 00 FF nn  (se aplica a la linea completa)
        if(y>0 && p+4<=end && p[0]==0 && p[1]==0 && p[2]==0xFF){
            int rep=p[3]; p+=4;
            for(int k=0;k<rep && y<H;k++,y++) memcpy(raw+(size_t)y*plane, raw+(size_t)(y-1)*plane, plane);
            continue;
        }
        for(int pl=0; pl<planes; pl++){
            unsigned char* dst=raw+(size_t)y*plane+(size_t)pl*lb; size_t o=0;
            while(o<lb && p<end){
                unsigned char b=*p++;
                if(b==0x80){ if(p>=end) break; int cnt=*p++;
                    while(cnt-- && o<lb && p<end) dst[o++]=*p++; }
                else if(b==0x00){ if(p>=end) break; int cnt=*p++;
                    if(cnt==0){ if(p+1<end && p[0]==0xFF){ p+=2; } break; }   // fin raro: cortar
                    while(cnt-- && o<lb){ for(int k=0;k<patlen && o<lb;k++) dst[o++]= (p+k<end)?p[k]:0; }
                    p+=patlen; }
                else { int cnt=b&0x7F; unsigned char v=(b&0x80)?0xFF:0x00;
                    while(cnt-- && o<lb) dst[o++]=v; }
            }
        }
        y++;
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4);
    if(!out){ free(raw); return NULL; }
    for(int y=0;y<H;y++){
        const unsigned char* row=raw+(size_t)y*plane;
        for(int x=0;x<W;x++){
            int idx=0;
            for(int pl=0;pl<planes;pl++) if((row[(size_t)pl*lb + x/8]>>(7-(x&7)))&1) idx|=1<<pl;
            if(idx>=ncol) idx=ncol-1;
            unsigned char* o=out+((size_t)y*W+x)*4;
            o[0]=pal[idx][0]; o[1]=pal[idx][1]; o[2]=pal[idx][2]; o[3]=255;
        }
    }
    free(raw); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- PlayStation TIM
// Textura de PSX: 4/8 bits con CLUT o 16/24 bits directos. El color de 16 bits es
// BGR555 con un bit STP (semitransparencia); negro puro con STP=0 es transparente.
static unsigned char* rt_tim(const unsigned char* d, size_t n, int* w, int* h){
    if(n<20 || ex_le32(d)!=0x10) return NULL;
    uint32_t flags=ex_le32(d+4);
    int bpp=flags&3, hasClut=(flags>>3)&1;
    size_t p=8;
    const unsigned char* clut=NULL; int clutN=0;
    if(hasClut){
        if(p+12>n) return NULL;
        uint32_t sz=ex_le32(d+p); if(sz<12 || p+sz>n) return NULL;
        clutN=ex_le16(d+p+8); clut=d+p+12;
        if(clutN<1 || (size_t)clutN*2 > sz-12) return NULL;
        p+=sz;
    }
    if(p+12>n) return NULL;
    uint32_t sz=ex_le32(d+p); if(sz<12 || p+sz>n) return NULL;
    int units=ex_le16(d+p+8), H=ex_le16(d+p+10);
    const unsigned char* px=d+p+12;
    int W = bpp==0 ? units*4 : bpp==1 ? units*2 : bpp==2 ? units : units*2/3;
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    if((size_t)units*2*(size_t)H > sz-12) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    size_t rowb=(size_t)units*2;
    for(int y=0;y<H;y++){
        const unsigned char* row=px+(size_t)y*rowb;
        for(int x=0;x<W;x++){
            unsigned char* o=out+((size_t)y*W+x)*4; o[3]=255;
            uint16_t c=0;
            if(bpp==3){ const unsigned char* q=row+(size_t)x*3;
                o[0]=q[0]; o[1]=q[1]; o[2]=q[2]; continue; }
            if(bpp==2)      c=ex_le16(row+(size_t)x*2);
            else {
                int idx = bpp==0 ? ((row[x/2]>>((x&1)*4))&0xF) : row[x];
                if(!clut || idx>=clutN){ o[0]=o[1]=o[2]=0; continue; }
                c=ex_le16(clut+(size_t)idx*2);
            }
            int r=(c&31), g=((c>>5)&31), b=((c>>10)&31), stp=(c>>15)&1;
            o[0]=(unsigned char)((r<<3)|(r>>2)); o[1]=(unsigned char)((g<<3)|(g>>2)); o[2]=(unsigned char)((b<<3)|(b>>2));
            if(!stp && !c) o[3]=0;                    // negro sin STP = transparente
        }
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- Alias/Wavefront PIX
// Cabecera de 10 bytes BE y un RLE de (cuenta, BGR) o (cuenta, gris).
static unsigned char* rt_pix(const unsigned char* d, size_t n, int* w, int* h){
    if(n<10) return NULL;
    int W=ex_be16(d), H=ex_be16(d+2), bpp=ex_be16(d+8);
    if(!ex_ok((uint32_t)W,(uint32_t)H) || (bpp!=24 && bpp!=8)) return NULL;
    if(ex_be16(d+4)>W || ex_be16(d+6)>H) return NULL;   // offsets sanos
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    size_t i=10, o=0, total=(size_t)W*H;
    int step = bpp==24 ? 4 : 2;
    while(o<total && i+step<=n){
        int cnt=d[i];
        unsigned char r,g,b;
        if(bpp==24){ b=d[i+1]; g=d[i+2]; r=d[i+3]; } else r=g=b=d[i+1];
        i+=step;
        if(!cnt) cnt=1;
        while(cnt-- && o<total){ unsigned char* q=out+o*4; q[0]=r;q[1]=g;q[2]=b;q[3]=255; o++; }
    }
    if(!o){ free(out); return NULL; }
    while(o<total){ unsigned char* q=out+o*4; q[0]=q[1]=q[2]=0; q[3]=255; o++; }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- DCX (PCX multipagina)
// Contenedor de fax/scanner: magic + tabla de offsets a PCX. Mostramos la 1a pagina.
static unsigned char* rt_dcx(const unsigned char* d, size_t n, int* w, int* h){
    if(n<8 || ex_le32(d)!=0x3ADE68B1u) return NULL;
    for(size_t k=1;k<1024 && 4*k+4<=n;k++){
        uint32_t off=ex_le32(d+4*k);
        if(!off) break;
        if(off>=n) break;
        unsigned char* r=ex_pcx(d+off, n-off, w, h);
        if(r) return r;
    }
    return NULL;
}

// ---------------------------------------------------------------- Kodak Photo CD (.pcd)
// Image Pac: varias resoluciones en YCC. Decodificamos la "Base" (768x512), que
// es la unica que va sin comprimir: dos filas de luma y una de cada croma
// (submuestreadas a la mitad) por cada par de lineas.
static unsigned char* rt_pcd(const unsigned char* d, size_t n, int* w, int* h){
    if(n<0xC0000) return NULL;
    size_t sig=(size_t)-1;
    size_t lim = n<0x10000 ? n : 0x10000;
    for(size_t i=0;i+7<=lim;i++) if(!memcmp(d+i,"PCD_IPI",7)){ sig=i; break; }
    if(sig==(size_t)-1) return NULL;
    size_t base = sig>=0x800 ? sig-0x800+0x30000 : 0x30000;
    const int W=768,H=512;
    if(base + (size_t)0x90000 > n) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    const unsigned char* p=d+base;
    for(int pair=0; pair<H/2; pair++){
        const unsigned char* l0=p+(size_t)pair*2304;
        const unsigned char* l1=l0+768;
        const unsigned char* c1=l1+768;
        const unsigned char* c2=c1+384;
        for(int k=0;k<2;k++){
            const unsigned char* L=k?l1:l0;
            int y=pair*2+k;
            for(int x=0;x<W;x++){
                double Y=L[x]*1.3584, C1=(c1[x>>1]-156)*2.2179, C2=(c2[x>>1]-137)*1.8215;
                double rr=Y+C2, gg=Y-0.194*C1-0.509*C2, bb=Y+C1;
                unsigned char* o=out+((size_t)y*W+x)*4;
                o[0]=(unsigned char)(rr<0?0:(rr>255?255:rr));
                o[1]=(unsigned char)(gg<0?0:(gg>255?255:gg));
                o[2]=(unsigned char)(bb<0?0:(bb>255?255:bb));
                o[3]=255;
            }
        }
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- despacho
static unsigned char* retro_load(const unsigned char* d, size_t n, const char* ext, int* w, int* h){
    if(!d || n<8) return NULL;
    if(ext){
        if(!strcmp(ext,"pi1")||!strcmp(ext,"pi2")||!strcmp(ext,"pi3")||
           !strcmp(ext,"pc1")||!strcmp(ext,"pc2")||!strcmp(ext,"pc3")) return rt_degas(d,n,w,h);
        if(!strcmp(ext,"neo")) return rt_neo(d,n,w,h);
        if(!strcmp(ext,"scr")) return rt_zxscr(d,n,w,h);
        if(!strcmp(ext,"koa")||!strcmp(ext,"kla")) return rt_koala(d,n,w,h);
        if(!strcmp(ext,"img")||!strcmp(ext,"ximg")) return rt_gem(d,n,w,h);
        if(!strcmp(ext,"tim")) return rt_tim(d,n,w,h);
        if(!strcmp(ext,"pix")||!strcmp(ext,"als")) return rt_pix(d,n,w,h);
        if(!strcmp(ext,"dcx")) return rt_dcx(d,n,w,h);
        if(!strcmp(ext,"pcd")) return rt_pcd(d,n,w,h);
    }
    unsigned char* r;
    if(ex_le32(d)==0x3ADE68B1u && (r=rt_dcx(d,n,w,h))) return r;   // firma fuerte
    if(n>=0xC0000 && (r=rt_pcd(d,n,w,h))) return r;                // busca "PCD_IPI"
    if(n==6912 && (r=rt_zxscr(d,n,w,h))) return r;                 // tamano exacto
    if(n==10003 && (r=rt_koala(d,n,w,h))) return r;
    if(n==32128 && (r=rt_neo(d,n,w,h))) return r;
    if(n==32034 && (r=rt_degas(d,n,w,h))) return r;
    return NULL;
}
