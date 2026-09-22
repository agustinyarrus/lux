// exotic.h — decoders header-only propios para formatos de imagen raros.
// Cada uno devuelve RGBA8 (malloc, el caller libera con free) o NULL.
// Formatos: Farbfeld, PCX, PFM, Sun Raster, SGI/RGB, WBMP, PAM, XBM,
//           PNM ASCII (P1/P2/P3), XPM, IFF ILBM/PBM/ACBM (HAM/EHB), MacPaint,
//           XWD, DPX, Cineon, ICNS.
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "tonemap.h"   // HDR -> 8 bits (PFM)

static uint32_t ex_be32(const unsigned char* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint32_t ex_le32(const unsigned char* p){ return ((uint32_t)p[3]<<24)|((uint32_t)p[2]<<16)|((uint32_t)p[1]<<8)|p[0]; }
static uint16_t ex_be16(const unsigned char* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint16_t ex_le16(const unsigned char* p){ return (uint16_t)(((uint16_t)p[1]<<8)|p[0]); }
static int      ex_ok(uint32_t w,uint32_t h){ return w && h && w<=30000u && h<=30000u && (uint64_t)w*h<=200000000ull; }
static int      ex_hex1(unsigned char c){
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

// Decoder de imagenes embebidas (PNG/JPEG dentro de un contenedor como ICNS).
// El host lo cablea a stb_image; si queda en NULL esos casos se descartan.
typedef unsigned char* (*ex_embedded_fn)(const unsigned char* data, size_t n, int* w, int* h);
static ex_embedded_fn ex_decode_embedded = NULL;


// ---------------------------------------------------------------- Farbfeld
static unsigned char* ex_farbfeld(const unsigned char* d, size_t n, int* w, int* h){
    if(n<16 || memcmp(d,"farbfeld",8)!=0) return NULL;
    uint32_t W=ex_be32(d+8), H=ex_be32(d+12);
    if(!ex_ok(W,H) || n < 16 + (uint64_t)W*H*8) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    const unsigned char* p=d+16;
    for(size_t i=0;i<(size_t)W*H;i++){ // RGBA 16-bit BE -> byte alto
        out[i*4+0]=p[i*8+0]; out[i*4+1]=p[i*8+2]; out[i*4+2]=p[i*8+4]; out[i*4+3]=p[i*8+6];
    }
    *w=(int)W; *h=(int)H; return out;
}

// ---------------------------------------------------------------- PCX (1/2/4/8 bits, 1..4 planos)
// El PCX de los 80 venia en todos los sabores: monocromo, CGA de 4 colores, EGA
// de 16 (planar), VGA de 256 (paleta al final) y RGB de 24 bits en 3 planos.
static unsigned char* ex_pcx(const unsigned char* d, size_t n, int* w, int* h){
    if(n<128 || d[0]!=0x0A) return NULL;
    int bpp=d[3];
    int xmin=ex_le16(d+4), ymin=ex_le16(d+6), xmax=ex_le16(d+8), ymax=ex_le16(d+10);
    int W=xmax-xmin+1, H=ymax-ymin+1, planes=d[65], bpl=ex_le16(d+66);
    if(W<=0||H<=0||W>20000||H>20000||bpl<=0) return NULL;
    if(bpp!=1 && bpp!=2 && bpp!=4 && bpp!=8) return NULL;
    if(planes<1 || planes>4) return NULL;
    int indexed = !(bpp==8 && planes>=3);           // 8 bits x 3-4 planos = RGB directo
    int ncol = indexed ? (1<<(bpp*planes)) : 0;
    if(indexed && ncol>256) return NULL;
    int total=bpl*planes;
    // paleta: la VGA de 256 va al final tras un 0x0C; la EGA de 16 vive en la cabecera
    const unsigned char* pal=(planes==1 && bpp==8 && n>=769 && d[n-769]==0x0C) ? d+n-768 : NULL;
    unsigned char ega[48];
    if(indexed && !pal){
        memcpy(ega, d+16, 48);
        int allZero=1; for(int i=0;i<48;i++) if(ega[i]) { allZero=0; break; }
        if(allZero){                                 // sin paleta util: blanco y negro / EGA por defecto
            static const unsigned char def[16][3]={
                {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
                {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}};
            if(ncol==2){ memset(ega,0,48); ega[3]=ega[4]=ega[5]=255; }
            else for(int i=0;i<16;i++){ ega[i*3]=def[i][0]; ega[i*3+1]=def[i][1]; ega[i*3+2]=def[i][2]; }
        }
        pal=ega;
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    unsigned char* line=(unsigned char*)malloc(total); if(!line){free(out);return NULL;}
    const unsigned char* p=d+128, *end=d+n;
    for(int y=0;y<H;y++){
        int idx=0;
        while(idx<total && p<end){
            unsigned char b=*p++; int cnt=1; unsigned char val=b;
            if((b&0xC0)==0xC0){ cnt=b&0x3F; if(p>=end)break; val=*p++; }
            while(cnt-->0 && idx<total) line[idx++]=val;
        }
        while(idx<total) line[idx++]=0;
        for(int x=0;x<W;x++){
            unsigned char r,g,b2,a=255;
            if(!indexed){ r=line[x]; g=line[bpl+x]; b2=line[2*bpl+x]; if(planes==4) a=line[3*bpl+x]; }
            else {
                unsigned int v=0;
                if(bpp==8) v=line[x];
                else {
                    int per=8/bpp, mask=(1<<bpp)-1;
                    for(int pl=0;pl<planes;pl++){
                        int byte=line[(size_t)pl*bpl + x/per];
                        int sh=(per-1-(x%per))*bpp;
                        v |= (unsigned)((byte>>sh)&mask) << (pl*bpp);
                    }
                }
                if((int)v>=ncol) v=ncol-1;
                r=pal[v*3]; g=pal[v*3+1]; b2=pal[v*3+2];
            }
            unsigned char* o=out+((size_t)y*W+x)*4; o[0]=r;o[1]=g;o[2]=b2;o[3]=a;
        }
    }
    free(line); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- PFM (float, HDR)
// Cabecera "PF" (color) o "Pf" (gris), ancho, alto y escala (negativa = little endian); los datos
// van de ABAJO hacia arriba. ex_pfm_float devuelve el float lineal (malloc, *channels = 1 o 3)
// para que el host lo pase por el tone mapping; ex_pfm lo entrega ya en 8 bits.
static float* ex_pfm_float(const unsigned char* d, size_t n, int* w, int* h, int* channels){
    if(n<3 || d[0]!='P' || (d[1]!='F'&&d[1]!='f')) return NULL;
    int color=(d[1]=='F'); size_t i=2;
    long vals[2]={0,0}; double sc=0;
    char num[64];
    for(int t=0;t<3;t++){
        while(i<n && (d[i]==' '||d[i]=='\n'||d[i]=='\r'||d[i]=='\t')) i++;
        int k=0; while(i<n && k<63 && !(d[i]==' '||d[i]=='\n'||d[i]=='\r'||d[i]=='\t')) num[k++]=(char)d[i++];
        num[k]=0; if(!k) return NULL;
        if(t<2) vals[t]=atol(num); else sc=atof(num);
    }
    i++; // un whitespace tras el scale
    int W=(int)vals[0], H=(int)vals[1];
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    int ch=color?3:1; int little=(sc<0); size_t need=(size_t)W*H*ch*4;
    if(i+need>n) return NULL;
    const unsigned char* p=d+i;
    float* out=(float*)malloc(need); if(!out) return NULL;
    for(int y=0;y<H;y++){ int sy=H-1-y;
        const unsigned char* row=p+(size_t)sy*W*ch*4;
        float* o=out+(size_t)y*W*ch;
        for(size_t j=0;j<(size_t)W*ch;j++){ const unsigned char* q=row+j*4; unsigned char b[4];
            if(little){b[0]=q[0];b[1]=q[1];b[2]=q[2];b[3]=q[3];} else {b[0]=q[3];b[1]=q[2];b[2]=q[1];b[3]=q[0];}
            memcpy(&o[j],b,4); }
    }
    *w=W; *h=H; *channels=ch; return out;
}

static unsigned char* ex_pfm(const unsigned char* d, size_t n, int* w, int* h){
    int ch=0; float* f=ex_pfm_float(d,n,w,h,&ch);
    if(!f) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)(*w)*(*h)*4);
    if(out) tm_to_rgba8(f,ch,*w,*h,out);
    free(f);
    return out;
}

// ---------------------------------------------------------------- Sun Raster (.ras)
// Tipo 0/1 = crudo; tipo 2 = el RLE de Sun (0x80 <cnt> <val>, y 0x80 0x00 = un 0x80 literal).
static unsigned char* ex_sun(const unsigned char* d, size_t n, int* w, int* h){
    if(n<32 || ex_be32(d)!=0x59A66A95u) return NULL;
    uint32_t W=ex_be32(d+4),H=ex_be32(d+8),depth=ex_be32(d+12),type=ex_be32(d+20),maplen=ex_be32(d+28);
    if(!ex_ok(W,H) || type>2) return NULL;
    if(depth!=8 && depth!=24 && depth!=32) return NULL;
    if((uint64_t)maplen > n-32) return NULL;
    const unsigned char* cmap=d+32; const unsigned char* p=d+32+maplen;
    int bypp=depth/8; size_t rowbytes=((size_t)W*bypp+1)&~(size_t)1; // padded a 16-bit
    unsigned char* unrle=NULL;
    if(type==2){
        size_t need=rowbytes*(size_t)H;
        unrle=(unsigned char*)malloc(need); if(!unrle) return NULL;
        size_t o=0,i=0,avail=(size_t)(d+n-p);
        while(o<need && i<avail){
            unsigned char b=p[i++];
            if(b!=0x80){ unrle[o++]=b; continue; }
            if(i>=avail) break;
            unsigned char cnt=p[i++];
            if(cnt==0){ unrle[o++]=0x80; continue; }
            if(i>=avail) break;
            unsigned char v=p[i++];
            for(int k=0;k<=cnt && o<need;k++) unrle[o++]=v;
        }
        while(o<need) unrle[o++]=0;
        p=unrle;
    }
    if(type!=2 && p+rowbytes*H > d+n){ free(unrle); return NULL; }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4);
    if(!out){ free(unrle); return NULL; }
    for(uint32_t y=0;y<H;y++){ const unsigned char* row=p+(size_t)y*rowbytes;
        for(uint32_t x=0;x<W;x++){ unsigned char r,g,b,a=255;
            if(depth==8){ unsigned char v=row[x];
                if(maplen>=768){ r=cmap[v]; g=cmap[256+v]; b=cmap[512+v]; } else r=g=b=v; }
            else if(depth==24){ b=row[x*3+0]; g=row[x*3+1]; r=row[x*3+2]; } // BGR
            else { a=row[x*4+0]; b=row[x*4+1]; g=row[x*4+2]; r=row[x*4+3]; } // xBGR
            unsigned char* o=out+((size_t)y*W+x)*4; o[0]=r;o[1]=g;o[2]=b;o[3]=a;
        }
    }
    free(unrle);
    *w=(int)W; *h=(int)H; return out;
}

// ---------------------------------------------------------------- SGI / RGB (verbatim + RLE, 8 bpc)
static unsigned char* ex_sgi(const unsigned char* d, size_t n, int* w, int* h){
    if(n<512 || ex_be16(d)!=0x01DAu) return NULL;
    int storage=d[2], bpc=d[3];
    int W=ex_be16(d+6), H=ex_be16(d+8), ch=ex_be16(d+10);
    if(bpc!=1 || !ex_ok((uint32_t)W,(uint32_t)H) || ch<1 || ch>4) return NULL;
    unsigned char* chan=(unsigned char*)malloc((size_t)W*H*ch); if(!chan) return NULL;
    if(storage==0){ // verbatim
        const unsigned char* p=d+512; if(p+(size_t)W*H*ch>d+n){ free(chan); return NULL; }
        memcpy(chan,p,(size_t)W*H*ch);
    } else { // RLE: tablas de offsets/lengths (BE uint32) por scanline*canal
        int nl=H*ch; const unsigned char* starts=d+512; const unsigned char* lengths=starts+(size_t)nl*4;
        if(lengths+(size_t)nl*4>d+n){ free(chan); return NULL; }
        for(int c=0;c<ch;c++) for(int y=0;y<H;y++){
            uint32_t off=ex_be32(starts+((size_t)c*H+y)*4);
            const unsigned char* p=d+off; const unsigned char* end=d+n;
            unsigned char* dst=chan+((size_t)c*H+y)*W; int x=0;
            while(p<end && x<W){ unsigned char b=*p++; int cnt=b&0x7F; if(!cnt) break;
                if(b&0x80){ while(cnt-- && x<W && p<end) dst[x++]=*p++; }
                else { if(p>=end)break; unsigned char v=*p++; while(cnt-- && x<W) dst[x++]=v; } }
        }
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out){ free(chan); return NULL; }
    for(int y=0;y<H;y++){ int sy=H-1-y; // SGI va de abajo hacia arriba
        for(int x=0;x<W;x++){ unsigned char* o=out+((size_t)y*W+x)*4;
            unsigned char r,g,b,a=255;
            #define SGI_C(k) chan[((size_t)(k)*H+sy)*W+x]
            if(ch==1){ r=g=b=SGI_C(0); }
            else if(ch==2){ r=g=b=SGI_C(0); a=SGI_C(1); }
            else { r=SGI_C(0); g=SGI_C(1); b=SGI_C(2); if(ch==4) a=SGI_C(3); }
            #undef SGI_C
            o[0]=r;o[1]=g;o[2]=b;o[3]=a;
        }
    }
    free(chan); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- WBMP (1-bit)
static int ex_mbint(const unsigned char* d, size_t n, size_t* i){
    int v=0,c=0; while(*i<n && c<5){ unsigned char b=d[(*i)++]; v=(v<<7)|(b&0x7F); c++; if(!(b&0x80)) break; } return v;
}
static unsigned char* ex_wbmp(const unsigned char* d, size_t n, int* w, int* h){
    if(n<4 || d[0]!=0 || d[1]!=0) return NULL; // type 0, fixed header 0
    size_t i=2; int W=ex_mbint(d,n,&i), H=ex_mbint(d,n,&i);
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    size_t rb=((size_t)W+7)/8; if(i+rb*H>n) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    const unsigned char* p=d+i;
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){
        unsigned char bit=(p[(size_t)y*rb + x/8]>>(7-(x&7)))&1; unsigned char v=bit?255:0;
        unsigned char* o=out+((size_t)y*W+x)*4; o[0]=o[1]=o[2]=v; o[3]=255;
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- PAM (P7)
static unsigned char* ex_pam(const unsigned char* d, size_t n, int* w, int* h){
    if(n<3 || d[0]!='P' || d[1]!='7') return NULL;
    int W=0,H=0,depth=0,maxval=255; size_t i=2;
    char tok[64];
    while(i<n){
        while(i<n && (d[i]=='\n'||d[i]=='\r'||d[i]==' '||d[i]=='\t')) i++;
        int k=0; while(i<n && k<63 && d[i]!='\n'&&d[i]!='\r'&&d[i]!=' '&&d[i]!='\t') tok[k++]=(char)d[i++];
        tok[k]=0;
        if(!strcmp(tok,"ENDHDR")){ while(i<n && d[i]!='\n') i++; if(i<n) i++; break; }
        else if(!strcmp(tok,"WIDTH"))  { while(i<n&&(d[i]==' '||d[i]=='\t'))i++; W=atoi((const char*)d+i); while(i<n&&d[i]!='\n')i++; }
        else if(!strcmp(tok,"HEIGHT")) { while(i<n&&(d[i]==' '||d[i]=='\t'))i++; H=atoi((const char*)d+i); while(i<n&&d[i]!='\n')i++; }
        else if(!strcmp(tok,"DEPTH"))  { while(i<n&&(d[i]==' '||d[i]=='\t'))i++; depth=atoi((const char*)d+i); while(i<n&&d[i]!='\n')i++; }
        else if(!strcmp(tok,"MAXVAL")) { while(i<n&&(d[i]==' '||d[i]=='\t'))i++; maxval=atoi((const char*)d+i); while(i<n&&d[i]!='\n')i++; }
        else { while(i<n && d[i]!='\n') i++; }
    }
    if(!ex_ok((uint32_t)W,(uint32_t)H) || depth<1 || depth>4 || maxval<1) return NULL;
    int bytes=(maxval>255)?2:1; size_t need=(size_t)W*H*depth*bytes;
    if(i+need>n) return NULL;
    const unsigned char* p=d+i; unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(size_t k=0;k<(size_t)W*H;k++){ unsigned char s[4]={0,0,0,255};
        for(int c=0;c<depth;c++){ int val = bytes==2 ? ((p[(k*depth+c)*2]<<8)|p[(k*depth+c)*2+1]) : p[k*depth+c];
            s[c]=(unsigned char)(maxval==255?val:(val*255/maxval)); }
        unsigned char* o=out+k*4;
        if(depth==1){ o[0]=o[1]=o[2]=s[0]; o[3]=255; }
        else if(depth==2){ o[0]=o[1]=o[2]=s[0]; o[3]=s[1]; }
        else { o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=depth==4?s[3]:255; }
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- XBM (texto C)
// Busca `pat` dentro de los primeros n bytes (el buffer no esta NUL-terminado,
// asi que strstr/atoi crudos se irian de rango).
static const unsigned char* ex_find(const unsigned char* d, size_t n, const char* pat){
    size_t pl=strlen(pat);
    if(pl>n) return NULL;
    for(size_t i=0;i+pl<=n;i++) if(!memcmp(d+i,pat,pl)) return d+i;
    return NULL;
}
static int ex_num_at(const unsigned char* p, const unsigned char* end){
    while(p<end && (*p==' '||*p=='\t')) p++;
    long v=0; int got=0;
    while(p<end && *p>='0' && *p<='9'){ v=v*10+(*p++-'0'); got=1; if(v>1000000L) return 0; }
    return got?(int)v:0;
}
static unsigned char* ex_xbm(const unsigned char* d, size_t n, int* w, int* h){
    const unsigned char* end=d+n;
    const unsigned char* pw=ex_find(d,n,"_width");
    const unsigned char* ph=ex_find(d,n,"_height");
    if(!pw||!ph) return NULL;
    int W=ex_num_at(pw+6,end), H=ex_num_at(ph+7,end);
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    const unsigned char* br=ex_find(d,n,"{"); if(!br) return NULL;
    size_t rb=((size_t)W+7)/8; unsigned char* bits=(unsigned char*)calloc(rb*H,1); if(!bits) return NULL;
    size_t cnt=0; const unsigned char* p=br+1;
    while(p<end && cnt<rb*H){
        while(p<end && *p!='0' && *p!='}') p++;
        if(p>=end || *p=='}') break;
        if(p+1<end && (p[1]=='x'||p[1]=='X')){
            p+=2; unsigned v=0; int got=0;
            while(p<end){ int hx=ex_hex1(*p); if(hx<0) break; v=(v<<4)|(unsigned)hx; p++; got=1; }
            if(got) bits[cnt++]=(unsigned char)v;
        }
        else p++;
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out){ free(bits); return NULL; }
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){ // XBM: bit 1 = negro, LSB primero
        unsigned char bit=(bits[(size_t)y*rb + x/8]>>(x&7))&1; unsigned char v=bit?0:255;
        unsigned char* o=out+((size_t)y*W+x)*4; o[0]=o[1]=o[2]=v; o[3]=255;
    }
    free(bits); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- PNM ASCII (P1/P2/P3)
// stb_image solo lee el Netpbm binario (P5/P6); la variante en texto queda para aca.
// Salta comentarios '#' y acepta cualquier whitespace entre los valores.
static int ex_pnm_tok(const unsigned char* d, size_t n, size_t* i, long* out){
    for(;;){
        while(*i<n && (d[*i]==' '||d[*i]=='\t'||d[*i]=='\n'||d[*i]=='\r')) (*i)++;
        if(*i<n && d[*i]=='#'){ while(*i<n && d[*i]!='\n') (*i)++; continue; }
        break;
    }
    if(*i>=n || d[*i]<'0' || d[*i]>'9') return 0;
    long v=0; while(*i<n && d[*i]>='0' && d[*i]<='9'){ v=v*10+(d[(*i)++]-'0'); if(v>1000000000L) return 0; }
    *out=v; return 1;
}
static unsigned char* ex_pnm_ascii(const unsigned char* d, size_t n, int* w, int* h){
    if(n<8 || d[0]!='P' || d[1]<'1' || d[1]>'3') return NULL;
    int kind=d[1]-'0';                 // 1=bitmap, 2=gray, 3=rgb
    size_t i=2; long W=0,H=0,maxv=1;
    if(!ex_pnm_tok(d,n,&i,&W) || !ex_pnm_tok(d,n,&i,&H)) return NULL;
    if(kind!=1 && !ex_pnm_tok(d,n,&i,&maxv)) return NULL;
    if(!ex_ok((uint32_t)W,(uint32_t)H) || maxv<1 || maxv>65535) return NULL;
    int ch=(kind==3)?3:1;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(size_t k=0;k<(size_t)W*H;k++){
        unsigned char* o=out+k*4; unsigned char s[3]={0,0,0};
        for(int c=0;c<ch;c++){
            long v; if(!ex_pnm_tok(d,n,&i,&v)){ free(out); return NULL; }
            if(kind==1) s[c]=v?0:255;              // P1: 1 = negro
            else { if(v>maxv) v=maxv; s[c]=(unsigned char)(maxv==255?v:(v*255/maxv)); }
        }
        if(ch==1){ o[0]=o[1]=o[2]=s[0]; } else { o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; }
        o[3]=255;
    }
    *w=(int)W; *h=(int)H; return out;
}

// ---------------------------------------------------------------- XPM (X PixMap, texto C)
// Hermano a color del XBM. Se leen las cadenas entre comillas: la 1a es el header
// "W H ncolors cpp", siguen ncolors definiciones y despues H filas de pixeles.
// Devuelve 1 si parseo el color; deja RGBA en rgba. Acepta #RGB/#RRGGBB/#RRRRGGGGBBBB,
// "None" (transparente) y los nombres X11 mas usados.
static int ex_xpm_color(const char* s, unsigned char* rgba){
    while(*s==' '||*s=='\t') s++;
    if(*s=='#'){
        const char* p=s+1; int len=0; while(ex_hex1((unsigned char)p[len])>=0) len++;
        int per = (len==3)?1 : (len==6)?2 : (len==12)?4 : (len==9)?3 : 0;
        if(!per) return 0;
        for(int c=0;c<3;c++){
            int hi=ex_hex1((unsigned char)p[c*per]);
            int lo=(per==1)?hi:ex_hex1((unsigned char)p[c*per+1]);
            rgba[c]=(unsigned char)((hi<<4)|lo);
        }
        rgba[3]=255; return 1;
    }
    struct { const char* name; unsigned char r,g,b; } tbl[] = {
        {"none",0,0,0},{"black",0,0,0},{"white",255,255,255},{"red",255,0,0},
        {"green",0,128,0},{"lime",0,255,0},{"blue",0,0,255},{"yellow",255,255,0},
        {"cyan",0,255,255},{"aqua",0,255,255},{"magenta",255,0,255},{"fuchsia",255,0,255},
        {"gray",190,190,190},{"grey",190,190,190},{"orange",255,165,0},{"pink",255,192,203},
        {"brown",165,42,42},{"purple",128,0,128},{"navy",0,0,128},{"teal",0,128,128},
        {"olive",128,128,0},{"maroon",128,0,0},{"silver",192,192,192},{"gold",255,215,0},
    };
    char low[32]; int k=0;
    while(s[k] && k<31 && s[k]!=' ' && s[k]!='\t'){ char c=s[k]; low[k]=(char)((c>='A'&&c<='Z')?c+32:c); k++; }
    low[k]=0;
    for(size_t t=0;t<sizeof(tbl)/sizeof(tbl[0]);t++){
        if(!strcmp(low,tbl[t].name)){
            rgba[0]=tbl[t].r; rgba[1]=tbl[t].g; rgba[2]=tbl[t].b;
            rgba[3]=(unsigned char)(t==0?0:255);   // "none" = transparente
            return 1;
        }
    }
    return 0;
}
static unsigned char* ex_xpm(const unsigned char* d, size_t n, int* w, int* h){
    // recolectar las cadenas entre comillas (sin las que estan en comentarios)
    size_t cap=64, cnt=0;
    const unsigned char** str=(const unsigned char**)malloc(cap*sizeof(*str));
    size_t* slen=(size_t*)malloc(cap*sizeof(*slen));
    if(!str||!slen){ free(str); free(slen); return NULL; }
    for(size_t i=0;i<n;i++){
        if(d[i]=='/' && i+1<n && d[i+1]=='*'){ i+=2; while(i+1<n && !(d[i]=='*'&&d[i+1]=='/')) i++; i++; continue; }
        if(d[i]!='"') continue;
        size_t s=++i; while(i<n && d[i]!='"'){ if(d[i]=='\\' && i+1<n) i++; i++; }
        if(i>=n) break;
        if(cnt==cap){ cap*=2;
            const unsigned char** s2=(const unsigned char**)realloc(str,cap*sizeof(*str));
            size_t* l2=(size_t*)realloc(slen,cap*sizeof(*slen));
            if(!s2||!l2){ free(s2?s2:str); free(l2?l2:slen); return NULL; }
            str=s2; slen=l2;
        }
        str[cnt]=d+s; slen[cnt]=i-s; cnt++;
    }
    unsigned char* out=NULL; unsigned char* pal=NULL; char* keys=NULL;
    if(cnt<2) goto done;
    {
        // header: "W H ncolors cpp [x_hot y_hot [XPMEXT]]"
        int hv[4]={0,0,0,0}; size_t hi=0;
        for(int t=0;t<4;t++){
            while(hi<slen[0] && (str[0][hi]==' '||str[0][hi]=='\t')) hi++;
            if(hi>=slen[0] || str[0][hi]<'0' || str[0][hi]>'9') goto done;
            long v=0; while(hi<slen[0] && str[0][hi]>='0' && str[0][hi]<='9'){ v=v*10+(str[0][hi++]-'0'); if(v>1000000L) goto done; }
            hv[t]=(int)v;
        }
        int W=hv[0],H=hv[1],nc=hv[2],cpp=hv[3];
        if(!ex_ok((uint32_t)W,(uint32_t)H) || nc<1 || nc>65536 || cpp<1 || cpp>8) goto done;
        if(cnt < (size_t)1+nc+H) goto done;

        pal=(unsigned char*)malloc((size_t)nc*4);
        keys=(char*)malloc((size_t)nc*cpp);
        if(!pal||!keys) goto done;
        for(int c=0;c<nc;c++){
            const unsigned char* e=str[1+c]; size_t el=slen[1+c];
            if(el<(size_t)cpp) goto done;
            memcpy(keys+(size_t)c*cpp, e, cpp);
            // buscar la clave "c" (color); si no esta, probar "m"/"g" (mono/gris)
            char line[256]; size_t ll=(el-cpp)<255?(el-cpp):255;
            memcpy(line, e+cpp, ll); line[ll]=0;
            unsigned char rgba[4]={0,0,0,255}; int got=0;
            for(int pass=0; pass<2 && !got; pass++){
                const char want = pass==0 ? 'c' : 'm';
                for(size_t j=0; line[j]; j++){
                    int atStart = (j==0) || line[j-1]==' ' || line[j-1]=='\t';
                    if(!atStart || line[j]!=want) continue;
                    if(line[j+1] && line[j+1]!=' ' && line[j+1]!='\t') continue;
                    size_t k=j+1; while(line[k]==' '||line[k]=='\t') k++;
                    if(ex_xpm_color(line+k, rgba)){ got=1; break; }
                }
            }
            memcpy(pal+(size_t)c*4, rgba, 4);
        }
        out=(unsigned char*)malloc((size_t)W*H*4); if(!out) goto done;
        for(int y=0;y<H;y++){
            const unsigned char* row=str[1+nc+y]; size_t rl=slen[1+nc+y];
            for(int x=0;x<W;x++){
                unsigned char* o=out+((size_t)y*W+x)*4;
                o[0]=o[1]=o[2]=0; o[3]=0;
                if((size_t)(x+1)*cpp>rl) continue;
                for(int c=0;c<nc;c++)
                    if(!memcmp(keys+(size_t)c*cpp, row+(size_t)x*cpp, cpp)){ memcpy(o, pal+(size_t)c*4, 4); break; }
            }
        }
        *w=W; *h=H;
    }
done:
    free(str); free(slen); free(pal); free(keys);
    return out;
}

// ---------------------------------------------------------------- IFF ILBM / PBM (Amiga)
// Planar con compresion ByteRun1, mas los modos de color de Amiga: EHB (Extra
// Half-Brite, 64 colores usando la mitad del brillo) y HAM6/HAM8 (hold-and-modify:
// cada pixel cambia un solo canal del anterior).
static unsigned char* ex_ilbm(const unsigned char* d, size_t n, int* w, int* h){
    if(n<32 || memcmp(d,"FORM",4)!=0) return NULL;
    int chunky = !memcmp(d+8,"PBM ",4);            // DPaint IIe: 1 byte por pixel
    int acbm   = !memcmp(d+8,"ACBM",4);            // AmigaBasic: planos contiguos, sin RLE
    if(!chunky && !acbm && memcmp(d+8,"ILBM",4)!=0) return NULL;

    int W=0,H=0,planes=0,masking=0,compress=0,transp=-1;
    unsigned char cmap[256*3]; int ncol=0; uint32_t camg=0;
    const unsigned char *body=NULL; size_t bodyLen=0;

    for(size_t p=12; p+8<=n; ){
        const unsigned char* id=d+p; uint32_t sz=ex_be32(d+p+4);
        p+=8; if(sz>n-p) sz=(uint32_t)(n-p);
        if(!memcmp(id,"BMHD",4) && sz>=20){
            W=ex_be16(d+p); H=ex_be16(d+p+2);
            planes=d[p+8]; masking=d[p+9]; compress=d[p+10];
            transp=ex_be16(d+p+12);
        } else if(!memcmp(id,"CMAP",4)){
            ncol=(int)(sz/3); if(ncol>256) ncol=256;
            memcpy(cmap,d+p,(size_t)ncol*3);
        } else if(!memcmp(id,"CAMG",4) && sz>=4){
            camg=ex_be32(d+p);
        } else if(!memcmp(id,"BODY",4) || (acbm && !memcmp(id,"ABIT",4))){
            body=d+p; bodyLen=sz;
        }
        p+=sz+(sz&1);                              // los chunks IFF van a byte par
    }
    if(!body || !ex_ok((uint32_t)W,(uint32_t)H) || planes<1 || planes>8 || compress>1) return NULL;

    int ham = (camg & 0x800) && (planes==6 || planes==8) && ncol>0;
    int ehb = (camg & 0x80)  && planes==6 && !ham;
    // chunky (PBM ) trae un byte por pixel: una sola "capa", no un plano por bit
    int hasMask = (masking==1);
    int layers  = (chunky ? 1 : planes) + (hasMask ? 1 : 0);
    size_t rowb = chunky ? (((size_t)W+1)&~(size_t)1) : ((((size_t)W+15)/16)*2);
    size_t need = rowb*layers*(size_t)H;
    // ACBM guarda cada plano entero uno atras del otro; ILBM los intercala por fila
    size_t pstride = acbm ? rowb*(size_t)H : rowb;
    if(acbm) compress=0;

    unsigned char* raw=(unsigned char*)malloc(need); if(!raw) return NULL;
    if(compress==0){
        if(bodyLen<need){ free(raw); return NULL; }
        memcpy(raw,body,need);
    } else {                                       // ByteRun1 (PackBits)
        size_t o=0,i=0;
        while(o<need && i<bodyLen){
            signed char c=(signed char)body[i++];
            if(c>=0){ int cnt=c+1; while(cnt-- && o<need && i<bodyLen) raw[o++]=body[i++]; }
            else if(c!=-128){ int cnt=-c+1; if(i>=bodyLen) break; unsigned char v=body[i++]; while(cnt-- && o<need) raw[o++]=v; }
        }
        while(o<need) raw[o++]=0;
    }

    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4);
    if(!out){ free(raw); return NULL; }
    size_t maskOff = pstride*(size_t)(chunky?1:planes);   // el plano de mascara va ultimo
    for(int y=0;y<H;y++){
        const unsigned char* rp = acbm ? (raw+(size_t)y*rowb) : (raw+(size_t)y*rowb*layers);
        int hr=0,hg=0,hb=0;                        // color "sostenido" del HAM
        for(int x=0;x<W;x++){
            unsigned int idx;
            if(chunky) idx=rp[x];
            else { idx=0; for(int b=0;b<planes;b++) if((rp[(size_t)b*pstride + x/8]>>(7-(x&7)))&1) idx|=(1u<<b); }
            unsigned char* o=out+((size_t)y*W+x)*4; unsigned char a=255;
            if(masking==2 && (int)idx==transp) a=0;    // color-clave declarado en el BMHD
            if(ham){
                int ctlShift = (planes==8)?6:4;
                int ctl=(int)(idx>>ctlShift), data=(int)(idx & ((1u<<ctlShift)-1));
                int val = (planes==8) ? ((data<<2)|(data>>4)) : (data*17);
                if(ctl==0){ int c=data<ncol?data:0; hr=cmap[c*3]; hg=cmap[c*3+1]; hb=cmap[c*3+2]; }
                else if(ctl==1) hb=val;
                else if(ctl==2) hr=val;
                else            hg=val;
                o[0]=(unsigned char)hr; o[1]=(unsigned char)hg; o[2]=(unsigned char)hb;
            } else if(ehb && (int)idx>=32){
                int c=(int)idx-32; if(c>=ncol) c=ncol?ncol-1:0;
                o[0]=cmap[c*3]/2; o[1]=cmap[c*3+1]/2; o[2]=cmap[c*3+2]/2;
            } else if(ncol){
                int c=(int)idx<ncol?(int)idx:ncol-1;
                o[0]=cmap[c*3]; o[1]=cmap[c*3+1]; o[2]=cmap[c*3+2];
            } else {                               // sin CMAP: escala de grises
                unsigned char v=(unsigned char)(planes>=8?idx:(idx*255/((1u<<planes)-1)));
                o[0]=o[1]=o[2]=v;
            }
            if(hasMask && !((rp[maskOff + x/8]>>(7-(x&7)))&1)) a=0;
            o[3]=a;
        }
    }
    free(raw); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- MacPaint (.mac / .pntg)
// 576x720 monocromo, comprimido con PackBits. Puede venir con cabecera MacBinary.
static unsigned char* ex_macpaint(const unsigned char* d, size_t n, int* w, int* h){
    const int W=576, H=720; const size_t rowb=72;
    size_t off=512;
    if(n>=128+512 && d[0]==0 && d[1]>0 && d[1]<64 && !memcmp(d+65,"PNTG",4)) off=128+512; // MacBinary
    if(n<=off) return NULL;
    unsigned char* bits=(unsigned char*)calloc(rowb*H,1); if(!bits) return NULL;
    size_t o=0,i=off;
    while(o<rowb*H && i<n){                        // PackBits
        signed char c=(signed char)d[i++];
        if(c>=0){ int cnt=c+1; while(cnt-- && o<rowb*H && i<n) bits[o++]=d[i++]; }
        else if(c!=-128){ int cnt=-c+1; if(i>=n) break; unsigned char v=d[i++]; while(cnt-- && o<rowb*H) bits[o++]=v; }
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4);
    if(!out){ free(bits); return NULL; }
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){      // bit 1 = negro
        unsigned char v=((bits[(size_t)y*rowb + x/8]>>(7-(x&7)))&1)?0:255;
        unsigned char* p=out+((size_t)y*W+x)*4; p[0]=p[1]=p[2]=v; p[3]=255;
    }
    free(bits); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- XWD (X Window Dump v7)
// Volcado crudo de una ventana X11. Solo ZPixmap, que es lo que genera xwd por defecto.
static unsigned char* ex_xwd(const unsigned char* d, size_t n, int* w, int* h){
    if(n<100) return NULL;
    uint32_t hdrSize=ex_be32(d), ver=ex_be32(d+4);
    int be=1;
    if(ver!=7){                                    // header en little endian
        hdrSize=ex_le32(d); ver=ex_le32(d+4);
        if(ver!=7) return NULL;
        be=0;
    }
    #define XW(k) (be ? ex_be32(d+(k)*4) : ex_le32(d+(k)*4))
    uint32_t format=XW(2), W=XW(4), H=XW(5), byteOrder=XW(7), bpp=XW(11), bpl=XW(12);
    uint32_t rmask=XW(14), gmask=XW(15), bmask=XW(16), ncolors=XW(19);
    #undef XW
    if(format!=2 || !ex_ok(W,H) || hdrSize<100 || hdrSize>n) return NULL;
    if(bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32) return NULL;
    if(bpl < (uint64_t)W*bpp/8) return NULL;

    const unsigned char* cmap=d+hdrSize;
    if(ncolors>65536 || (uint64_t)hdrSize+(uint64_t)ncolors*12 > n) return NULL;
    const unsigned char* p=cmap+(size_t)ncolors*12;
    if((uint64_t)(p-d) + (uint64_t)bpl*H > n) return NULL;

    // shift/escala de cada mascara (para los modos directos)
    int rs=0,gs=0,bs=0; uint32_t rm=rmask?rmask:0xFF0000u, gm=gmask?gmask:0xFF00u, bm=bmask?bmask:0xFFu;
    while(rs<32 && !((rm>>rs)&1)) rs++;  while(gs<32 && !((gm>>gs)&1)) gs++;  while(bs<32 && !((bm>>bs)&1)) bs++;
    uint32_t rmax=rm>>rs, gmax=gm>>gs, bmax=bm>>bs;
    if(!rmax||!gmax||!bmax) return NULL;

    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(uint32_t y=0;y<H;y++){
        const unsigned char* row=p+(size_t)y*bpl;
        for(uint32_t x=0;x<W;x++){
            uint32_t v=0; const unsigned char* q=row+(size_t)x*(bpp/8);
            if(bpp==8) v=q[0];
            else if(bpp==16) v=byteOrder?ex_be16(q):ex_le16(q);
            else if(bpp==24) v=byteOrder?(((uint32_t)q[0]<<16)|((uint32_t)q[1]<<8)|q[2])
                                       :(((uint32_t)q[2]<<16)|((uint32_t)q[1]<<8)|q[0]);
            else v=byteOrder?ex_be32(q):ex_le32(q);
            unsigned char* o=out+((size_t)y*W+x)*4;
            if(bpp==8 && ncolors){                 // pseudocolor: indice en la paleta
                uint32_t i2 = v<ncolors ? v : 0;
                o[0]=cmap[i2*12+4]; o[1]=cmap[i2*12+6]; o[2]=cmap[i2*12+8];   // byte alto de cada u16
            } else if(bpp==8){ o[0]=o[1]=o[2]=(unsigned char)v; }
            else {
                o[0]=(unsigned char)(((v&rm)>>rs)*255/rmax);
                o[1]=(unsigned char)(((v&gm)>>gs)*255/gmax);
                o[2]=(unsigned char)(((v&bm)>>bs)*255/bmax);
            }
            o[3]=255;
        }
    }
    *w=(int)W; *h=(int)H; return out;
}

// ---------------------------------------------------------------- DPX / Cineon (cine digital)
// El caballito de batalla del DI: RGB de 10 bits empaquetado de a 3 en 32 bits.
// Tambien 8/12/16 bits sin comprimir. Los 10 bits son log Cineon -> se linealiza
// con la curva estandar (black 95, white 685) para que no se vea lavado.
// Curva estandar de densidad de impresion: 0.002 de densidad por codigo, gamma
// de negativo 0.6. El blanco de referencia es el codigo 685 y el negro el 95;
// se reescala entre esos dos para que el negro de la pelicula caiga en 0 y no en
// un gris lavado, y se sale a gamma de video.
static unsigned char ex_cineon_lut[1024];
static int ex_cineon_lut_ready = 0;
static void ex_build_cineon_lut(void){
    const double black = pow(10.0, (95.0-685.0)*0.002/0.6);   // ~0.0108 en lineal
    for(int i=0;i<1024;i++){
        double v=(pow(10.0,(i-685.0)*0.002/0.6)-black)/(1.0-black);
        if(v<0) v=0; if(v>1) v=1;
        v=pow(v,1.0/2.2);
        int q=(int)(v*255.0+0.5);
        ex_cineon_lut[i]=(unsigned char)(q<0?0:(q>255?255:q));
    }
    ex_cineon_lut_ready=1;
}
#define EX_DX32(o) (be?ex_be32(d+(o)):ex_le32(d+(o)))
#define EX_DX16(o) (be?ex_be16(d+(o)):ex_le16(d+(o)))
static unsigned char* ex_dpx(const unsigned char* d, size_t n, int* w, int* h){
    int be, isCineon;
    if(n>=1664 && !memcmp(d,"SDPX",4))              { be=1; isCineon=0; }
    else if(n>=1664 && !memcmp(d,"XPDS",4))         { be=0; isCineon=0; }
    else if(n>=2048 && ex_be32(d)==0x802A5FD7u)     { be=1; isCineon=1; }
    else if(n>=2048 && ex_le32(d)==0x802A5FD7u)     { be=0; isCineon=1; }
    else return NULL;

    size_t dataOff=EX_DX32(4);
    uint32_t W,H; int bits,packing,encoding,chans,logCurve;
    if(isCineon){
        chans=d[201]; if(chans<1||chans>4) chans=3;
        W=EX_DX32(208); H=EX_DX32(212); bits=d[206];
        packing=1; encoding=0; logCurve=1;         // Cineon: log, packed-filled, sin RLE
    } else {
        W=EX_DX32(772); H=EX_DX32(776);
        int desc=d[780+20], transfer=d[780+21];
        bits=d[780+23]; packing=EX_DX16(780+24); encoding=EX_DX16(780+26);
        uint32_t eo=EX_DX32(780+28); if(eo && eo<n) dataOff=eo;
        chans = (desc==51||desc==52) ? 4 : (desc==50||desc==100||desc==102) ? 3 : 1;
        logCurve = (transfer==1 || transfer==3);   // printing density / logaritmico
    }
    if(!ex_ok(W,H) || encoding!=0 || dataOff>=n) return NULL;
    if(bits!=8 && bits!=10 && bits!=12 && bits!=16) return NULL;
    if(chans!=1 && chans!=3 && chans!=4) return NULL;

    const unsigned char* p=d+dataOff;
    size_t avail=n-dataOff;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    if(!ex_cineon_lut_ready) ex_build_cineon_lut();

    if(bits==10 && packing!=0){
        // 3 muestras de 10 bits por palabra de 32 (2 bits sin usar); las filas
        // arrancan en palabra nueva.
        size_t wordsPerRow=((size_t)W*chans+2)/3;
        if(avail < wordsPerRow*4*(size_t)H){ free(out); return NULL; }
        for(uint32_t y=0;y<H;y++){
            const unsigned char* row=p+(size_t)y*wordsPerRow*4;
            for(uint32_t x=0;x<W;x++){
                unsigned char* o=out+((size_t)y*W+x)*4; o[3]=255;
                for(int c=0;c<chans && c<4;c++){
                    size_t s=(size_t)x*chans+c;
                    uint32_t word = be?ex_be32(row+(s/3)*4):ex_le32(row+(s/3)*4);
                    uint32_t v=(word>>(22-10*(int)(s%3)))&0x3FF;
                    // el alfa nunca lleva la curva: es cobertura, no densidad
                    if(c<3) o[c] = logCurve ? ex_cineon_lut[v] : (unsigned char)(v>>2);
                    else    o[3] = (unsigned char)(v>>2);
                }
                if(chans==1) o[1]=o[2]=o[0];
            }
        }
    } else {
        int bypp=(bits==8)?1:2;
        size_t rowb=(size_t)W*chans*bypp;
        if(avail < rowb*(size_t)H){ free(out); return NULL; }
        for(uint32_t y=0;y<H;y++){
            const unsigned char* row=p+(size_t)y*rowb;
            for(uint32_t x=0;x<W;x++){
                unsigned char* o=out+((size_t)y*W+x)*4; o[3]=255;
                for(int c=0;c<chans && c<4;c++){
                    const unsigned char* q=row+((size_t)x*chans+c)*bypp;
                    unsigned int v;
                    if(bits==8)       v=q[0];
                    // 12 bits vienen alineados a la izquierda en 16: los 8 altos ya sirven
                    else              v=(unsigned)(be?ex_be16(q):ex_le16(q))>>8;
                    if(c<3) o[c]=(unsigned char)v; else o[3]=(unsigned char)v;
                }
                if(chans==1) o[1]=o[2]=o[0];
            }
        }
    }
    *w=(int)W; *h=(int)H; return out;
}

// ---------------------------------------------------------------- ICNS (iconos de macOS)
// Contenedor de varias resoluciones. Las modernas traen un PNG adentro (se delega
// al decoder del host); las clasicas usan un RLE propio de 24 bits + mascara alfa.
// Descomprime un canal y devuelve cuantos bytes de la fuente consumio (los tres
// canales van pegados en un unico flujo, asi que el offset tiene que ser exacto).
static size_t ex_icns_rle(const unsigned char* s, size_t n, unsigned char* dst, size_t want, size_t* wrote){
    size_t i=0,o=0;
    while(o<want && i<n){
        unsigned char c=s[i++];
        if(c & 0x80){ int cnt=c-125; if(i>=n) break; unsigned char v=s[i++]; while(cnt-- && o<want) dst[o++]=v; }
        else { int cnt=c+1; while(cnt-- && o<want && i<n) dst[o++]=s[i++]; }
    }
    *wrote=o; return i;
}
static int ex_icns_dim(const unsigned char* t){    // lado del icono RLE segun su tipo
    if(!memcmp(t,"is32",4)) return 16;
    if(!memcmp(t,"il32",4)) return 32;
    if(!memcmp(t,"ih32",4)) return 48;
    if(!memcmp(t,"it32",4)) return 128;
    return 0;
}
static unsigned char* ex_icns(const unsigned char* d, size_t n, int* w, int* h){
    if(n<16 || memcmp(d,"icns",4)!=0) return NULL;
    uint32_t total=ex_be32(d+4); if(total>n || total<16) total=(uint32_t)n;

    const unsigned char *bestEmb=NULL; size_t bestEmbLen=0; long bestEmbArea=-1;
    const unsigned char *bestRle=NULL; size_t bestRleLen=0; int bestRleDim=0;

    for(size_t p=8; p+8<=total; ){
        const unsigned char* t=d+p; uint32_t sz=ex_be32(d+p+4);
        if(sz<8 || sz>total-p) break;
        const unsigned char* payload=d+p+8; size_t plen=sz-8;
        if(plen>=24 && !memcmp(payload,"\x89PNG\r\n\x1a\n",8) && !memcmp(payload+12,"IHDR",4)){
            long area=(long)ex_be32(payload+16)*(long)ex_be32(payload+20);
            if(area>bestEmbArea){ bestEmbArea=area; bestEmb=payload; bestEmbLen=plen; }
        } else {
            int dim=ex_icns_dim(t);
            if(dim && dim>bestRleDim){ bestRleDim=dim; bestRle=payload; bestRleLen=plen; }
        }
        p+=sz;
    }

    // el PNG mas grande gana si supera al mejor RLE (o si no hay decoder para el)
    if(bestEmb && ex_decode_embedded && (bestEmbArea >= (long)bestRleDim*bestRleDim)){
        unsigned char* r=ex_decode_embedded(bestEmb,bestEmbLen,w,h);
        if(r) return r;
    }
    if(!bestRle){
        if(bestEmb && ex_decode_embedded) return ex_decode_embedded(bestEmb,bestEmbLen,w,h);
        return NULL;
    }

    int dim=bestRleDim; size_t px=(size_t)dim*dim;
    const unsigned char* s=bestRle; size_t sl=bestRleLen;
    if(dim==128 && sl>4 && ex_be32(s)==0) { s+=4; sl-=4; }   // it32 arranca con 4 ceros
    unsigned char* chan=(unsigned char*)malloc(px*3); if(!chan) return NULL;
    // Ojo: el RLE puede salir MAS grande que el crudo si la imagen tiene mucho
    // detalle, asi que la variante sin comprimir se reconoce por tamano exacto.
    if(sl==px*3){
        memcpy(chan,s,px*3);
    } else {
        size_t got=0;
        for(int c=0;c<3;c++){
            size_t wrote=0;
            got += ex_icns_rle(s+got, sl>got?sl-got:0, chan+(size_t)c*px, px, &wrote);
            if(wrote<px){ free(chan); return NULL; }
        }
    }
    // mascara alfa del mismo tamano (s8mk/l8mk/h8mk/t8mk), si esta
    unsigned char* alpha=NULL;
    const char* mk = dim==16?"s8mk" : dim==32?"l8mk" : dim==48?"h8mk" : "t8mk";
    for(size_t p=8; p+8<=total; ){
        uint32_t sz=ex_be32(d+p+4); if(sz<8 || sz>total-p) break;
        if(!memcmp(d+p,mk,4) && sz-8>=px){ alpha=(unsigned char*)(d+p+8); break; }
        p+=sz;
    }
    unsigned char* out=(unsigned char*)malloc(px*4);
    if(!out){ free(chan); return NULL; }
    for(size_t i=0;i<px;i++){
        out[i*4+0]=chan[i]; out[i*4+1]=chan[px+i]; out[i*4+2]=chan[px*2+i];
        out[i*4+3]=alpha?alpha[i]:255;
    }
    free(chan); *w=dim; *h=dim; return out;
}

#undef EX_DX32
#undef EX_DX16

// ---------------------------------------------------------------- despacho
// Devuelve RGBA (malloc) o NULL. Elige por extension; cae a magic.
static unsigned char* exotic_load(const unsigned char* d, size_t n, const char* ext, int* w, int* h){
    if(!d || n<8) return NULL;
    // por extension (puntero ya en minúsculas)
    if(ext){
        if(!strcmp(ext,"ff")||!strcmp(ext,"farbfeld")) return ex_farbfeld(d,n,w,h);
        if(!strcmp(ext,"pcx")) return ex_pcx(d,n,w,h);
        if(!strcmp(ext,"pfm")) return ex_pfm(d,n,w,h);
        if(!strcmp(ext,"ras")||!strcmp(ext,"sun")||!strcmp(ext,"im1")||!strcmp(ext,"im8")||!strcmp(ext,"im24")||!strcmp(ext,"im32")) return ex_sun(d,n,w,h);
        if(!strcmp(ext,"sgi")||!strcmp(ext,"rgb")||!strcmp(ext,"rgba")||!strcmp(ext,"bw")||!strcmp(ext,"int")||!strcmp(ext,"inta")) return ex_sgi(d,n,w,h);
        if(!strcmp(ext,"wbmp")) return ex_wbmp(d,n,w,h);
        if(!strcmp(ext,"pam")) return ex_pam(d,n,w,h);
        if(!strcmp(ext,"xbm")) return ex_xbm(d,n,w,h);
        if(!strcmp(ext,"xpm")) return ex_xpm(d,n,w,h);
        if(!strcmp(ext,"iff")||!strcmp(ext,"ilbm")||!strcmp(ext,"lbm")||!strcmp(ext,"acbm")) return ex_ilbm(d,n,w,h);
        if(!strcmp(ext,"mac")||!strcmp(ext,"pntg")||!strcmp(ext,"macp")) return ex_macpaint(d,n,w,h);
        if(!strcmp(ext,"xwd")) return ex_xwd(d,n,w,h);
        if(!strcmp(ext,"dpx")||!strcmp(ext,"cin")) return ex_dpx(d,n,w,h);
        if(!strcmp(ext,"icns")) return ex_icns(d,n,w,h);
        // Netpbm: stb hace el binario (P5/P6), aca queda el ASCII (P1/P2/P3)
        if(!strcmp(ext,"ppm")||!strcmp(ext,"pgm")||!strcmp(ext,"pbm")||!strcmp(ext,"pnm"))
            return ex_pnm_ascii(d,n,w,h);
    }
    // fallback por magic (formatos con firma fuerte)
    unsigned char* r;
    if((r=ex_farbfeld(d,n,w,h))) return r;
    if(ex_be32(d)==0x59A66A95u && (r=ex_sun(d,n,w,h))) return r;
    if(ex_be16(d)==0x01DAu && (r=ex_sgi(d,n,w,h))) return r;
    if(!memcmp(d,"icns",4) && (r=ex_icns(d,n,w,h))) return r;
    if(!memcmp(d,"FORM",4) && (r=ex_ilbm(d,n,w,h))) return r;
    if((!memcmp(d,"SDPX",4)||!memcmp(d,"XPDS",4)||ex_be32(d)==0x802A5FD7u||ex_le32(d)==0x802A5FD7u)
       && (r=ex_dpx(d,n,w,h))) return r;
    if((d[0]=='P'&&d[1]=='7') && (r=ex_pam(d,n,w,h))) return r;
    if((d[0]=='P'&&(d[1]=='F'||d[1]=='f')) && (r=ex_pfm(d,n,w,h))) return r;
    if((d[0]=='P'&&d[1]>='1'&&d[1]<='3') && (r=ex_pnm_ascii(d,n,w,h))) return r;
    if((r=ex_xwd(d,n,w,h))) return r;
    return NULL;
}
