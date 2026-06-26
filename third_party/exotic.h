// exotic.h — decoders header-only propios para formatos de imagen raros.
// Cada uno devuelve RGBA8 (malloc, el caller libera con free) o NULL.
// Formatos: Farbfeld, PCX, PFM, Sun Raster, SGI/RGB, WBMP, PAM, XBM.
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static uint32_t ex_be32(const unsigned char* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t ex_be16(const unsigned char* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static uint16_t ex_le16(const unsigned char* p){ return (uint16_t)(((uint16_t)p[1]<<8)|p[0]); }
static int      ex_ok(uint32_t w,uint32_t h){ return w && h && w<=30000u && h<=30000u && (uint64_t)w*h<=200000000ull; }

// tone-map HDR float -> 8 bit (normaliza por el max si supera 1, luego gamma 2.2)
static unsigned char ex_tm(float v, float scale){
    v *= scale; if(v<0) v=0;
    v = powf(v, 1.0f/2.2f);
    int i = (int)(v*255.0f + 0.5f);
    return (unsigned char)(i<0?0:(i>255?255:i));
}

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

// ---------------------------------------------------------------- PCX (8-bit, 1/3/4 planos)
static unsigned char* ex_pcx(const unsigned char* d, size_t n, int* w, int* h){
    if(n<128 || d[0]!=0x0A) return NULL;
    int bpp=d[3];
    int xmin=ex_le16(d+4), ymin=ex_le16(d+6), xmax=ex_le16(d+8), ymax=ex_le16(d+10);
    int W=xmax-xmin+1, H=ymax-ymin+1, planes=d[65], bpl=ex_le16(d+66);
    if(W<=0||H<=0||W>20000||H>20000||bpp!=8||(planes!=1&&planes!=3&&planes!=4)||bpl<=0) return NULL;
    int total=bpl*planes;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    unsigned char* line=(unsigned char*)malloc(total); if(!line){free(out);return NULL;}
    const unsigned char* p=d+128, *end=d+n;
    const unsigned char* pal=(planes==1 && n>=769 && d[n-769]==0x0C) ? d+n-768 : NULL;
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
            if(planes==1){ unsigned char v=line[x];
                if(pal){ r=pal[v*3];g=pal[v*3+1];b2=pal[v*3+2]; } else r=g=b2=v;
            } else { r=line[x]; g=line[bpl+x]; b2=line[2*bpl+x]; if(planes==4) a=line[3*bpl+x]; }
            unsigned char* o=out+((size_t)y*W+x)*4; o[0]=r;o[1]=g;o[2]=b2;o[3]=a;
        }
    }
    free(line); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- PFM (float, HDR)
static unsigned char* ex_pfm(const unsigned char* d, size_t n, int* w, int* h){
    if(n<3 || d[0]!='P' || (d[1]!='F'&&d[1]!='f')) return NULL;
    int color=(d[1]=='F'); size_t i=2;
    long vals[3]={0,0,0}; double sc=0; int got=0; // W, H, scale
    char num[64];
    for(int t=0;t<3;t++){
        while(i<n && (d[i]==' '||d[i]=='\n'||d[i]=='\r'||d[i]=='\t')) i++;
        int k=0; while(i<n && k<63 && !(d[i]==' '||d[i]=='\n'||d[i]=='\r'||d[i]=='\t')) num[k++]=(char)d[i++];
        num[k]=0; if(!k) return NULL;
        if(t<2) vals[t]=atol(num); else sc=atof(num);
        got++;
    }
    i++; // un whitespace tras el scale
    int W=(int)vals[0], H=(int)vals[1];
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    int ch=color?3:1; int little=(sc<0); size_t need=(size_t)W*H*ch*4;
    if(i+need>n) return NULL;
    const unsigned char* p=d+i;
    // 1ª pasada: max para normalizar
    float mx=0; size_t cnt=(size_t)W*H*ch;
    for(size_t j=0;j<cnt;j++){ const unsigned char* q=p+j*4; unsigned char b[4];
        if(little){b[0]=q[0];b[1]=q[1];b[2]=q[2];b[3]=q[3];} else {b[0]=q[3];b[1]=q[2];b[2]=q[1];b[3]=q[0];}
        float f; memcpy(&f,b,4); if(f>mx && f<1e30f) mx=f; }
    float scale=(mx>1.0f)?1.0f/mx:1.0f;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(int y=0;y<H;y++){ int sy=H-1-y; // PFM va de abajo hacia arriba
        for(int x=0;x<W;x++){ const unsigned char* q=p+((size_t)sy*W+x)*ch*4; float c[3];
            for(int z=0;z<ch;z++){ unsigned char b[4]; const unsigned char* qq=q+z*4;
                if(little){b[0]=qq[0];b[1]=qq[1];b[2]=qq[2];b[3]=qq[3];} else {b[0]=qq[3];b[1]=qq[2];b[2]=qq[1];b[3]=qq[0];}
                memcpy(&c[z],b,4); }
            unsigned char* o=out+((size_t)y*W+x)*4;
            if(color){ o[0]=ex_tm(c[0],scale); o[1]=ex_tm(c[1],scale); o[2]=ex_tm(c[2],scale); }
            else { unsigned char g=ex_tm(c[0],scale); o[0]=o[1]=o[2]=g; }
            o[3]=255;
        }
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- Sun Raster (.ras), no comprimido
static unsigned char* ex_sun(const unsigned char* d, size_t n, int* w, int* h){
    if(n<32 || ex_be32(d)!=0x59A66A95u) return NULL;
    uint32_t W=ex_be32(d+4),H=ex_be32(d+8),depth=ex_be32(d+12),type=ex_be32(d+20),maplen=ex_be32(d+28);
    if(!ex_ok(W,H) || type>1) return NULL; // type 0/1 = sin comprimir
    if(depth!=8 && depth!=24 && depth!=32) return NULL;
    const unsigned char* cmap=d+32; const unsigned char* p=d+32+maplen;
    int bypp=depth/8; size_t rowbytes=((size_t)W*bypp+1)&~(size_t)1; // padded a 16-bit
    if(p+rowbytes*H > d+n) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(uint32_t y=0;y<H;y++){ const unsigned char* row=p+(size_t)y*rowbytes;
        for(uint32_t x=0;x<W;x++){ unsigned char r,g,b,a=255;
            if(depth==8){ unsigned char v=row[x];
                if(maplen>=768){ r=cmap[v]; g=cmap[256+v]; b=cmap[512+v]; } else r=g=b=v; }
            else if(depth==24){ b=row[x*3+0]; g=row[x*3+1]; r=row[x*3+2]; } // BGR
            else { a=row[x*4+0]; b=row[x*4+1]; g=row[x*4+2]; r=row[x*4+3]; } // xBGR
            unsigned char* o=out+((size_t)y*W+x)*4; o[0]=r;o[1]=g;o[2]=b;o[3]=a;
        }
    }
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
static unsigned char* ex_xbm(const unsigned char* d, size_t n, int* w, int* h){
    const char* s=(const char*)d; // buscar "_width" y "_height"
    const char* pw=strstr(s,"_width"); const char* ph=strstr(s,"_height");
    if(!pw||!ph) return NULL;
    int W=atoi(pw+6), H=atoi(ph+7);
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    const char* br=strchr(s,'{'); if(!br) return NULL;
    size_t rb=((size_t)W+7)/8; unsigned char* bits=(unsigned char*)malloc(rb*H); if(!bits) return NULL;
    size_t cnt=0; const char* p=br+1;
    while(*p && cnt<rb*H){
        while(*p && *p!='0' && *p!='}') p++;
        if(*p=='}'||!*p) break;
        if(p[0]=='0'&&(p[1]=='x'||p[1]=='X')){ bits[cnt++]=(unsigned char)strtol(p,(char**)&p,16); }
        else p++;
    }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out){ free(bits); return NULL; }
    for(int y=0;y<H;y++) for(int x=0;x<W;x++){ // XBM: bit 1 = negro, LSB primero
        unsigned char bit=(bits[(size_t)y*rb + x/8]>>(x&7))&1; unsigned char v=bit?0:255;
        unsigned char* o=out+((size_t)y*W+x)*4; o[0]=o[1]=o[2]=v; o[3]=255;
    }
    free(bits); *w=W; *h=H; return out;
}

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
    }
    // fallback por magic (formatos con firma fuerte)
    unsigned char* r;
    if((r=ex_farbfeld(d,n,w,h))) return r;
    if(ex_be32(d)==0x59A66A95u && (r=ex_sun(d,n,w,h))) return r;
    if(ex_be16(d)==0x01DAu && (r=ex_sgi(d,n,w,h))) return r;
    if((d[0]=='P'&&d[1]=='7') && (r=ex_pam(d,n,w,h))) return r;
    if((d[0]=='P'&&(d[1]=='F'||d[1]=='f')) && (r=ex_pfm(d,n,w,h))) return r;
    return NULL;
}
