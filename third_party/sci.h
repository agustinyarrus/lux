// sci.h — decoders header-only para imagenes cientificas y medicas.
// Mismo contrato que exotic.h: RGBA8 con malloc (el caller libera) o NULL.
// Se incluye DESPUES de exotic.h (usa ex_be16/ex_le16/ex_ok y el gancho
// ex_decode_embedded para los DICOM que traen un JPEG adentro).
//
// Formatos: FITS (astronomia) y DICOM (imagen medica).
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// ---------------------------------------------------------------------------
//  Normalizacion comun: de un plano de doubles a 8 bits recortando las colas.
//  Las imagenes cientificas casi nunca usan todo el rango (una estrella
//  saturada aplasta el resto), asi que se recorta el 0.25% de cada extremo.
// ---------------------------------------------------------------------------
static void sci_stretch(const double* v, size_t n, double* lo, double* hi){
    double mn=1e308, mx=-1e308;
    for(size_t i=0;i<n;i++){ double x=v[i]; if(x!=x) continue; if(x<mn)mn=x; if(x>mx)mx=x; }
    if(!(mx>mn)){ *lo=mn; *hi=mn+1.0; return; }
    enum { NB=2048 };
    size_t* hist=(size_t*)calloc(NB,sizeof(size_t));
    if(!hist){ *lo=mn; *hi=mx; return; }
    double k=(NB-1)/(mx-mn);
    size_t valid=0;
    for(size_t i=0;i<n;i++){ double x=v[i]; if(x!=x) continue;
        int b=(int)((x-mn)*k); if(b<0)b=0; if(b>=NB)b=NB-1; hist[b]++; valid++; }
    size_t cut=(size_t)(valid*0.0025), acc=0; int b0=0, b1=NB-1;
    for(int b=0;b<NB;b++){ acc+=hist[b]; if(acc>cut){ b0=b; break; } }
    acc=0;
    for(int b=NB-1;b>=0;b--){ acc+=hist[b]; if(acc>cut){ b1=b; break; } }
    free(hist);
    if(b1<=b0){ *lo=mn; *hi=mx; return; }
    *lo=mn+b0/k; *hi=mn+b1/k;
}
static unsigned char sci_q(double x, double lo, double hi){
    if(x!=x) return 0;
    double v=(x-lo)/(hi-lo);
    if(v<0)v=0; if(v>1)v=1;
    int q=(int)(v*255.0+0.5);
    return (unsigned char)(q<0?0:(q>255?255:q));
}

// ---------------------------------------------------------------- FITS
// Cabecera de tarjetas de 80 caracteres en bloques de 2880 bytes; los datos van
// en big endian y de abajo hacia arriba. NAXIS3==3 se interpreta como RGB.
static int sci_fits_card(const unsigned char* d, size_t n, const char* key, double* out){
    size_t kl=strlen(key);
    for(size_t c=0; (c+1)*80<=n; c++){
        const unsigned char* card=d+c*80;
        if(!memcmp(card,"END     ",8)) return 0;
        if(memcmp(card,key,kl)) continue;
        int ok=1; for(size_t j=kl;j<8;j++) if(card[j]!=' ') ok=0;   // el nombre va rellenado a 8
        if(!ok || card[8]!='=') continue;
        char buf[72]; memcpy(buf,card+10,70); buf[70]=0;
        for(int j=0;j<70;j++) if(buf[j]=='/'){ buf[j]=0; break; }
        *out=atof(buf); return 1;
    }
    return 0;
}
static unsigned char* sci_fits(const unsigned char* d, size_t n, int* w, int* h){
    if(n<2880 || memcmp(d,"SIMPLE  =",9)) return NULL;
    // fin de la cabecera: primera tarjeta END, redondeada al bloque de 2880
    size_t endCard=0; int found=0;
    for(size_t c=0; (c+1)*80<=n; c++) if(!memcmp(d+c*80,"END     ",8)){ endCard=c; found=1; break; }
    if(!found) return NULL;
    size_t hdrBytes=((endCard*80+80)+2879)/2880*2880;
    if(hdrBytes>=n) return NULL;

    double bitpix=0, naxis=0, n1=0, n2=0, n3=1, bzero=0, bscale=1;
    if(!sci_fits_card(d,hdrBytes,"BITPIX",&bitpix)) return NULL;
    if(!sci_fits_card(d,hdrBytes,"NAXIS",&naxis) || naxis<2) return NULL;
    if(!sci_fits_card(d,hdrBytes,"NAXIS1",&n1)) return NULL;
    if(!sci_fits_card(d,hdrBytes,"NAXIS2",&n2)) return NULL;
    sci_fits_card(d,hdrBytes,"NAXIS3",&n3);
    sci_fits_card(d,hdrBytes,"BZERO",&bzero);
    sci_fits_card(d,hdrBytes,"BSCALE",&bscale);
    int W=(int)n1, H=(int)n2, planes=(int)n3;
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    if(planes<1) planes=1;
    if(planes!=3) planes=1;                       // 2D, o el 1er plano de un cubo
    int bp=(int)bitpix, bytes=(bp<0?-bp:bp)/8;
    if(bytes!=1 && bytes!=2 && bytes!=4 && bytes!=8) return NULL;
    if(bscale==0) bscale=1;

    size_t px=(size_t)W*H, need=px*(size_t)planes*(size_t)bytes;
    if((uint64_t)px*planes > 64000000ull) return NULL;   // el buffer de doubles seria absurdo
    if(hdrBytes+need>n) return NULL;
    double* val=(double*)malloc(px*(size_t)planes*sizeof(double)); if(!val) return NULL;
    const unsigned char* p=d+hdrBytes;
    for(size_t i=0;i<px*(size_t)planes;i++){
        const unsigned char* q=p+i*bytes;
        double x=0;
        if(bp==8)        x=q[0];
        else if(bp==16){ int16_t v=(int16_t)ex_be16(q); x=v; }
        else if(bp==32){ int32_t v=(int32_t)ex_be32(q); x=v; }
        else if(bp==64){ int64_t v=((int64_t)ex_be32(q)<<32)|ex_be32(q+4); x=(double)v; }
        else if(bp==-32){ uint32_t u=ex_be32(q); float f; memcpy(&f,&u,4); x=f; }
        else if(bp==-64){ uint64_t u=((uint64_t)ex_be32(q)<<32)|ex_be32(q+4); double f; memcpy(&f,&u,8); x=f; }
        else { free(val); return NULL; }
        val[i]=bzero+bscale*x;
    }
    double lo,hi; sci_stretch(val,px*(size_t)planes,&lo,&hi);
    unsigned char* out=(unsigned char*)malloc(px*4);
    if(!out){ free(val); return NULL; }
    for(int y=0;y<H;y++){
        int sy=H-1-y;                              // FITS arranca en la fila de abajo
        for(int x=0;x<W;x++){
            unsigned char* o=out+((size_t)y*W+x)*4;
            size_t k=(size_t)sy*W+x;
            if(planes==3){ o[0]=sci_q(val[k],lo,hi); o[1]=sci_q(val[px+k],lo,hi); o[2]=sci_q(val[2*px+k],lo,hi); }
            else { unsigned char g=sci_q(val[k],lo,hi); o[0]=o[1]=o[2]=g; }
            o[3]=255;
        }
    }
    free(val); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- DICOM
// Lector de la parte del dataset que necesitamos: el grupo 0028 (geometria y
// fotometria) y el 7FE0,0010 (pixeles). Soporta VR explicita e implicita, little
// y big endian, y datos crudos o encapsulados en JPEG (se delega en el host).
typedef struct {
    const unsigned char* d; size_t n, i;
    int expl, be;
} dcm_rd;
static uint16_t dcm_u16(dcm_rd* r, size_t at){ return r->be?ex_be16(r->d+at):ex_le16(r->d+at); }
static uint32_t dcm_u32(dcm_rd* r, size_t at){ return r->be?ex_be32(r->d+at):ex_le32(r->d+at); }

typedef struct {
    int rows, cols, frames, samples, alloc, stored, high, signd, planar;
    int mono1, ybr, rgb;
    double slope, inter, wc, ww;
    int haveWin;
    const unsigned char* px; size_t pxLen; int encap;
} dcm_info;

static void dcm_str(const unsigned char* p, size_t len, char* out, size_t cap){
    size_t k=0; for(size_t i=0;i<len && k+1<cap;i++){ unsigned char c=p[i]; if(c=='\0') break; out[k++]=(char)c; }
    while(k && (out[k-1]==' ')) k--;
    out[k]=0;
}
static double dcm_num(const unsigned char* p, size_t len){
    char b[64]; dcm_str(p,len<63?len:63,b,64);
    for(int i=0;b[i];i++) if(b[i]=='\\'){ b[i]=0; break; }   // multivaluado: 1er valor
    return atof(b);
}
static long dcm_int(dcm_rd* r, const unsigned char* p, size_t len){
    if(len==2) return (long)(r->be?ex_be16(p):ex_le16(p));
    if(len==4) return (long)(r->be?ex_be32(p):ex_le32(p));
    return (long)dcm_num(p,len);
}
static void dcm_elements(dcm_rd* r, size_t end, dcm_info* inf, int depth);
static void dcm_items(dcm_rd* r, size_t end, dcm_info* inf, int depth){
    while(r->i+8<=end){
        uint16_t g=dcm_u16(r,r->i), e=dcm_u16(r,r->i+2);
        uint32_t len=dcm_u32(r,r->i+4);
        r->i+=8;
        if(g==0xFFFE && e==0xE0DD) return;                 // fin de la secuencia
        if(g!=0xFFFE || e!=0xE000) return;                 // desincronizado: cortar
        if(len==0xFFFFFFFFu) dcm_elements(r,end,inf,depth+1);
        else { size_t sub=r->i+len; if(sub>end) sub=end; dcm_elements(r,sub,inf,depth+1); r->i=sub; }
    }
}
static void dcm_elements(dcm_rd* r, size_t end, dcm_info* inf, int depth){
    if(depth>8) { r->i=end; return; }
    while(r->i+8<=end){
        uint16_t g=dcm_u16(r,r->i), e=dcm_u16(r,r->i+2);
        size_t at=r->i+4; uint32_t len; int isSQ=0;
        if(g==0xFFFE){                                      // delimitadores: nunca llevan VR
            len=dcm_u32(r,at); r->i=at+4;
            if(e==0xE00D || e==0xE0DD) return;
            continue;
        }
        if(r->expl){
            if(at+2>end) return;
            char vr[3]={ (char)r->d[at], (char)r->d[at+1], 0 };
            if(!memcmp(vr,"OB",2)||!memcmp(vr,"OW",2)||!memcmp(vr,"OF",2)||!memcmp(vr,"OD",2)||
               !memcmp(vr,"OL",2)||!memcmp(vr,"SQ",2)||!memcmp(vr,"UT",2)||!memcmp(vr,"UC",2)||
               !memcmp(vr,"UR",2)||!memcmp(vr,"UN",2)){
                if(at+8>end) return;
                len=dcm_u32(r,at+4); r->i=at+8;
            } else { if(at+4>end) return; len=dcm_u16(r,at+2); r->i=at+4; }
            isSQ=!memcmp(vr,"SQ",2);
        } else {
            if(at+4>end) return;
            len=dcm_u32(r,at); r->i=at+4;
        }
        const unsigned char* v=r->d+r->i;
        if(len==0xFFFFFFFFu){                               // longitud indefinida
            if(g==0x7FE0 && e==0x0010){                     // pixeles encapsulados (comprimidos)
                inf->encap=1;
                // items: el 1o es la tabla de offsets (suele ir vacia); tomamos el
                // 1er fragmento con datos, que es el cuadro completo en el 99% de los casos
                size_t p=r->i;
                while(p+8<=end){
                    uint16_t ig=dcm_u16(r,p), ie=dcm_u16(r,p+2); uint32_t il=dcm_u32(r,p+4);
                    p+=8;
                    if(ig==0xFFFE && ie==0xE0DD) break;
                    if(ig!=0xFFFE || ie!=0xE000) break;
                    if(il==0xFFFFFFFFu || p+il>end) break;
                    if(il>0 && !inf->px){ inf->px=r->d+p; inf->pxLen=il; }
                    p+=il;
                }
                r->i=p; continue;
            }
            dcm_items(r,end,inf,depth+1);
            continue;
        }
        if(r->i+len>end){ r->i=end; return; }
        if(isSQ){ r->i+=len; continue; }

        if(g==0x0028){
            switch(e){
            case 0x0002: inf->samples=(int)dcm_int(r,v,len); break;
            case 0x0004: { char b[32]; dcm_str(v,len,b,32);
                           inf->mono1=!strncmp(b,"MONOCHROME1",11);
                           inf->ybr=!strncmp(b,"YBR",3);
                           inf->rgb=!strncmp(b,"RGB",3); } break;
            case 0x0006: inf->planar=(int)dcm_int(r,v,len); break;
            case 0x0008: inf->frames=(int)dcm_num(v,len); break;
            case 0x0010: inf->rows=(int)dcm_int(r,v,len); break;
            case 0x0011: inf->cols=(int)dcm_int(r,v,len); break;
            case 0x0100: inf->alloc=(int)dcm_int(r,v,len); break;
            case 0x0101: inf->stored=(int)dcm_int(r,v,len); break;
            case 0x0102: inf->high=(int)dcm_int(r,v,len); break;
            case 0x0103: inf->signd=(int)dcm_int(r,v,len); break;
            case 0x1050: inf->wc=dcm_num(v,len); inf->haveWin|=1; break;
            case 0x1051: inf->ww=dcm_num(v,len); inf->haveWin|=2; break;
            case 0x1052: inf->inter=dcm_num(v,len); break;
            case 0x1053: inf->slope=dcm_num(v,len); break;
            }
        } else if(g==0x7FE0 && e==0x0010){
            inf->px=v; inf->pxLen=len;
        }
        r->i+=len;
    }
}
static unsigned char* sci_dicom(const unsigned char* d, size_t n, int* w, int* h){
    size_t start=0; int expl=1, be=0;
    if(n>132 && !memcmp(d+128,"DICM",4)) start=132;
    else {
        // sin preambulo: solo lo aceptamos si arranca con un elemento del grupo 0008
        if(n<8 || ex_le16(d)!=0x0008) return NULL;
        expl = (d[4]>='A'&&d[4]<='Z'&&d[5]>='A'&&d[5]<='Z');
    }
    dcm_info inf; memset(&inf,0,sizeof(inf));
    inf.samples=1; inf.alloc=16; inf.stored=16; inf.slope=1; inf.inter=0; inf.frames=1;

    dcm_rd r; r.d=d; r.n=n; r.i=start; r.expl=1; r.be=0;
    if(start){
        // grupo 0002 (meta) siempre en VR explicita little endian; de ahi sale el
        // transfer syntax que rige para el resto del archivo
        char ts[80]="";
        size_t p=start;
        while(p+8<=n){
            uint16_t g=ex_le16(d+p);
            if(g!=0x0002) break;
            uint16_t e=ex_le16(d+p+2);
            char vr[3]={(char)d[p+4],(char)d[p+5],0}; uint32_t len; size_t vp;
            if(!memcmp(vr,"OB",2)||!memcmp(vr,"OW",2)||!memcmp(vr,"SQ",2)||!memcmp(vr,"UT",2)||!memcmp(vr,"UN",2)){
                if(p+12>n) break; len=ex_le32(d+p+8); vp=p+12;
            } else { if(p+8>n) break; len=ex_le16(d+p+6); vp=p+8; }
            if(vp+len>n) break;
            if(e==0x0010) dcm_str(d+vp,len,ts,80);
            p=vp+len;
        }
        r.i=p;
        if(!strcmp(ts,"1.2.840.10008.1.2"))        { expl=0; be=0; }
        else if(!strcmp(ts,"1.2.840.10008.1.2.2")) { expl=1; be=1; }
        else                                        { expl=1; be=0; }
        r.expl=expl; r.be=be;
    } else { r.expl=expl; r.be=0; }

    dcm_elements(&r,n,&inf,0);
    if(!inf.px || !inf.pxLen || !ex_ok((uint32_t)inf.cols,(uint32_t)inf.rows)) return NULL;

    // encapsulado: adentro hay un JPEG/JPEG-LS/JPEG2000 completo -> al host
    if(inf.encap){
        if(!ex_decode_embedded) return NULL;
        unsigned char* r2=ex_decode_embedded(inf.px,inf.pxLen,w,h);
        return r2;
    }

    int W=inf.cols, H=inf.rows, sp=inf.samples<1?1:inf.samples;
    int bytes=(inf.alloc<=8)?1:2;
    size_t px=(size_t)W*H;
    if(inf.pxLen < px*(size_t)sp*(size_t)bytes) return NULL;
    unsigned char* out=(unsigned char*)malloc(px*4); if(!out) return NULL;

    if(sp>=3){                                     // color: RGB o YBR, plano o intercalado
        for(size_t k=0;k<px;k++){
            unsigned char c[3];
            for(int ch=0;ch<3;ch++){
                size_t off = inf.planar ? ((size_t)ch*px+k) : (k*(size_t)sp+ch);
                c[ch]= bytes==1 ? inf.px[off] : inf.px[off*2+1];
            }
            unsigned char* o=out+k*4;
            if(inf.ybr){
                double Y=c[0], Cb=c[1]-128.0, Cr=c[2]-128.0;
                double rr=Y+1.402*Cr, gg=Y-0.344136*Cb-0.714136*Cr, bb=Y+1.772*Cb;
                o[0]=(unsigned char)(rr<0?0:(rr>255?255:rr));
                o[1]=(unsigned char)(gg<0?0:(gg>255?255:gg));
                o[2]=(unsigned char)(bb<0?0:(bb>255?255:bb));
            } else { o[0]=c[0]; o[1]=c[1]; o[2]=c[2]; }
            o[3]=255;
        }
        *w=W; *h=H; return out;
    }

    // gris: sign-extend, rescale y ventana (window/level) o estiramiento automatico
    double* val=(double*)malloc(px*sizeof(double));
    if(!val){ free(out); return NULL; }
    int stored=(inf.stored>0 && inf.stored<=16)?inf.stored:(bytes*8);
    for(size_t k=0;k<px;k++){
        long raw;
        if(bytes==1) raw=inf.px[k];
        else raw=(long)(be?ex_be16(inf.px+k*2):ex_le16(inf.px+k*2));
        if(inf.signd && stored<32){
            long sign=1L<<(stored-1);
            raw &= (1L<<stored)-1;
            if(raw & sign) raw -= (1L<<stored);
        }
        val[k]=inf.inter + (inf.slope?inf.slope:1.0)*(double)raw;
    }
    double lo,hi;
    if(inf.haveWin==3 && inf.ww>1){ lo=inf.wc-0.5-(inf.ww-1)/2.0; hi=inf.wc-0.5+(inf.ww-1)/2.0; }
    else sci_stretch(val,px,&lo,&hi);
    if(!(hi>lo)) hi=lo+1;
    for(size_t k=0;k<px;k++){
        unsigned char g=sci_q(val[k],lo,hi);
        if(inf.mono1) g=(unsigned char)(255-g);
        unsigned char* o=out+k*4; o[0]=o[1]=o[2]=g; o[3]=255;
    }
    free(val); *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- despacho
static unsigned char* sci_load(const unsigned char* d, size_t n, const char* ext, int* w, int* h){
    if(!d || n<64) return NULL;
    if(ext){
        if(!strcmp(ext,"fits")||!strcmp(ext,"fit")||!strcmp(ext,"fts")) return sci_fits(d,n,w,h);
        if(!strcmp(ext,"dcm")||!strcmp(ext,"dicom")||!strcmp(ext,"dic")) return sci_dicom(d,n,w,h);
    }
    unsigned char* r;
    if(!memcmp(d,"SIMPLE  =",9) && (r=sci_fits(d,n,w,h))) return r;
    if(n>132 && !memcmp(d+128,"DICM",4) && (r=sci_dicom(d,n,w,h))) return r;
    return NULL;
}
