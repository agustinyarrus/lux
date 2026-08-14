// texture.h — decoders header-only para texturas de motores 3D y juegos.
// Mismo contrato que exotic.h: RGBA8 con malloc (el caller libera) o NULL.
// Se incluye DESPUES de exotic.h (usa ex_le16/ex_le32/ex_ok).
//
// Formatos: los bloques comprimidos BC1..BC5 (DXT1/3/5, RGTC) y los tres
// contenedores que los usan: DDS (DirectX), VTF (Source de Valve) y KTX (OpenGL).
#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

enum { TX_BC1=1, TX_BC2, TX_BC3, TX_BC4, TX_BC5 };

static void tx_565(uint16_t c, unsigned char* o){
    int r=(c>>11)&31, g=(c>>5)&63, b=c&31;
    o[0]=(unsigned char)((r<<3)|(r>>2)); o[1]=(unsigned char)((g<<2)|(g>>4)); o[2]=(unsigned char)((b<<3)|(b>>2));
}
// Bloque de color de 4x4 comun a BC1/2/3: dos extremos en 565 e indices de 2 bits.
static void tx_color_block(const unsigned char* s, unsigned char px[16][4], int bc1){
    uint16_t c0=ex_le16(s), c1=ex_le16(s+2);
    unsigned char c[4][4];
    tx_565(c0,c[0]); c[0][3]=255;
    tx_565(c1,c[1]); c[1][3]=255;
    if(c0>c1 || !bc1){
        for(int k=0;k<3;k++){ c[2][k]=(unsigned char)((2*c[0][k]+c[1][k])/3); c[3][k]=(unsigned char)((c[0][k]+2*c[1][k])/3); }
        c[2][3]=c[3][3]=255;
    } else {
        for(int k=0;k<3;k++){ c[2][k]=(unsigned char)((c[0][k]+c[1][k])/2); c[3][k]=0; }
        c[2][3]=255; c[3][3]=0;                       // el 4o color es transparente
    }
    uint32_t bits=ex_le32(s+4);
    for(int i=0;i<16;i++){ int idx=(bits>>(i*2))&3; memcpy(px[i],c[idx],4); }
}
// Bloque de un canal (el alfa de BC3 y los canales de BC4/BC5): dos extremos de
// 8 bits e indices de 3 bits, con dos modos de interpolacion.
static void tx_alpha_block(const unsigned char* s, unsigned char out[16]){
    int a0=s[0], a1=s[1], a[8];
    a[0]=a0; a[1]=a1;
    if(a0>a1) for(int i=1;i<7;i++) a[i+1]=((7-i)*a0+i*a1)/7;
    else { for(int i=1;i<5;i++) a[i+1]=((5-i)*a0+i*a1)/5; a[6]=0; a[7]=255; }
    uint64_t bits=0; for(int i=0;i<6;i++) bits|=(uint64_t)s[2+i]<<(8*i);
    for(int i=0;i<16;i++) out[i]=(unsigned char)a[(bits>>(i*3))&7];
}
static int tx_block_bytes(int fmt){ return (fmt==TX_BC1||fmt==TX_BC4)?8:16; }
// Descomprime una imagen entera de bloques 4x4 a RGBA8.
static unsigned char* tx_bc(const unsigned char* s, size_t n, int W, int H, int fmt){
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    int bw=(W+3)/4, bh=(H+3)/4, bb=tx_block_bytes(fmt);
    if((uint64_t)bw*bh*bb > n) return NULL;
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(int by=0;by<bh;by++) for(int bx=0;bx<bw;bx++){
        const unsigned char* blk=s+((size_t)by*bw+bx)*bb;
        unsigned char px[16][4]; unsigned char a[16], a2[16];
        switch(fmt){
        case TX_BC1: tx_color_block(blk,px,1); break;
        case TX_BC2:
            tx_color_block(blk+8,px,0);
            for(int i=0;i<16;i++){ int v=(blk[i/2]>>((i&1)*4))&0xF; px[i][3]=(unsigned char)(v*17); }
            break;
        case TX_BC3:
            tx_color_block(blk+8,px,0); tx_alpha_block(blk,a);
            for(int i=0;i<16;i++) px[i][3]=a[i];
            break;
        case TX_BC4:
            tx_alpha_block(blk,a);
            for(int i=0;i<16;i++){ px[i][0]=px[i][1]=px[i][2]=a[i]; px[i][3]=255; }
            break;
        default: {                                   // BC5: dos canales -> normal map
            tx_alpha_block(blk,a); tx_alpha_block(blk+8,a2);
            for(int i=0;i<16;i++){
                double x=a[i]/127.5-1.0, y=a2[i]/127.5-1.0, z2=1.0-x*x-y*y;
                double z=z2>0?sqrt(z2):0.0;
                px[i][0]=a[i]; px[i][1]=a2[i]; px[i][2]=(unsigned char)(z*127.5+127.5); px[i][3]=255;
            }
        } }
        for(int y=0;y<4;y++) for(int x=0;x<4;x++){
            int ix=bx*4+x, iy=by*4+y;
            if(ix>=W || iy>=H) continue;
            memcpy(out+((size_t)iy*W+ix)*4, px[y*4+x], 4);
        }
    }
    return out;
}
static size_t tx_bc_size(int W, int H, int fmt){
    return (size_t)((W+3)/4)*((H+3)/4)*tx_block_bytes(fmt);
}

// Formato sin comprimir descrito por mascaras de bits (lo que usa el DDS clasico).
static unsigned char* tx_masked(const unsigned char* s, size_t n, int W, int H, int bpp,
                                uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am){
    if(bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32) return NULL;
    int bypp=bpp/8;
    if((uint64_t)W*H*bypp > n) return NULL;
    int rs=0,gs=0,bs=0,as=0; uint32_t rmax=1,gmax=1,bmax=1,amax=1;
    if(rm){ while(!((rm>>rs)&1)) rs++; rmax=rm>>rs; }
    if(gm){ while(!((gm>>gs)&1)) gs++; gmax=gm>>gs; }
    if(bm){ while(!((bm>>bs)&1)) bs++; bmax=bm>>bs; }
    if(am){ while(!((am>>as)&1)) as++; amax=am>>as; }
    unsigned char* out=(unsigned char*)malloc((size_t)W*H*4); if(!out) return NULL;
    for(size_t k=0;k<(size_t)W*H;k++){
        const unsigned char* q=s+k*bypp;
        uint32_t v=0;
        for(int i=0;i<bypp;i++) v|=(uint32_t)q[i]<<(8*i);
        unsigned char* o=out+k*4;
        o[0]= rm ? (unsigned char)(((v&rm)>>rs)*255/rmax) : 0;
        o[1]= gm ? (unsigned char)(((v&gm)>>gs)*255/gmax) : 0;
        o[2]= bm ? (unsigned char)(((v&bm)>>bs)*255/bmax) : 0;
        o[3]= am ? (unsigned char)(((v&am)>>as)*255/amax) : 255;
        if(!rm && !gm && !bm){ o[0]=o[1]=o[2]=(unsigned char)(v&0xFF); }   // luminancia
    }
    return out;
}

// ---------------------------------------------------------------- DDS
static unsigned char* tx_dds(const unsigned char* d, size_t n, int* w, int* h){
    if(n<128 || memcmp(d,"DDS ",4) || ex_le32(d+4)!=124) return NULL;
    int H=(int)ex_le32(d+12), W=(int)ex_le32(d+16);
    uint32_t pfFlags=ex_le32(d+80), fourCC=ex_le32(d+84), bpp=ex_le32(d+88);
    uint32_t rm=ex_le32(d+92), gm=ex_le32(d+96), bm=ex_le32(d+100), am=ex_le32(d+104);
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    size_t off=128;
    int fmt=0;
    if(pfFlags & 4){                                  // DDPF_FOURCC
        if(fourCC==0x31545844u) fmt=TX_BC1;           // 'DXT1'
        else if(fourCC==0x32545844u||fourCC==0x33545844u) fmt=TX_BC2;   // DXT2/3
        else if(fourCC==0x34545844u||fourCC==0x35545844u) fmt=TX_BC3;   // DXT4/5
        else if(fourCC==0x31495441u||fourCC==0x55344342u) fmt=TX_BC4;   // ATI1 / BC4U
        else if(fourCC==0x32495441u||fourCC==0x55354342u) fmt=TX_BC5;   // ATI2 / BC5U
        else if(fourCC==0x30315844u){                 // 'DX10': el formato viene aparte
            if(n<148) return NULL;
            uint32_t dxgi=ex_le32(d+128); off=148;
            if(dxgi>=70 && dxgi<=72) fmt=TX_BC1;
            else if(dxgi>=73 && dxgi<=75) fmt=TX_BC2;
            else if(dxgi>=76 && dxgi<=78) fmt=TX_BC3;
            else if(dxgi>=79 && dxgi<=81) fmt=TX_BC4;
            else if(dxgi>=82 && dxgi<=84) fmt=TX_BC5;
            else if(dxgi==87||dxgi==88){ rm=0xFF0000;gm=0xFF00;bm=0xFF;am=0xFF000000u; bpp=32; }
            else if(dxgi==28||dxgi==29){ rm=0xFF;gm=0xFF00;bm=0xFF0000;am=0xFF000000u; bpp=32; }
            else return NULL;                          // BC6H/BC7 y demas: que lo haga WIC
        }
        else return NULL;
    }
    if(off>=n) return NULL;
    unsigned char* out = fmt ? tx_bc(d+off,n-off,W,H,fmt)
                             : tx_masked(d+off,n-off,W,H,(int)bpp,rm,gm,bm,am);
    if(!out) return NULL;
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- VTF (Valve)
// Los mipmaps van del mas chico al mas grande, asi que el nivel 0 es lo ultimo
// del archivo: se lo ubica desde el final y nos ahorramos recorrer la cadena.
static unsigned char* tx_vtf(const unsigned char* d, size_t n, int* w, int* h){
    if(n<64 || memcmp(d,"VTF\0",4)) return NULL;
    uint32_t vmaj=ex_le32(d+4), vmin=ex_le32(d+8), hdr=ex_le32(d+12);
    int W=ex_le16(d+16), H=ex_le16(d+18);
    uint32_t flags=ex_le32(d+20); int frames=ex_le16(d+24);
    uint32_t hiFmt=ex_le32(d+52);
    if(vmaj!=7 || hdr<64 || hdr>n || !ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    if(frames<1) frames=1;
    int faces = (flags & 0x4000) ? ((vmin<5) ? 7 : 6) : 1;   // cubemap (+ esfera hasta 7.4)

    int fmt=0, bpp=0; uint32_t rm=0,gm=0,bm=0,am=0;
    switch(hiFmt){
    case 13: case 20: fmt=TX_BC1; break;
    case 14: fmt=TX_BC2; break;
    case 15: fmt=TX_BC3; break;
    case 0:  bpp=32; rm=0x000000FF; gm=0x0000FF00; bm=0x00FF0000; am=0xFF000000u; break; // RGBA8888
    case 11: bpp=32; rm=0x0000FF00; gm=0x00FF0000; bm=0xFF000000u; am=0x000000FF; break; // ARGB8888
    case 12: bpp=32; rm=0x00FF0000; gm=0x0000FF00; bm=0x000000FF; am=0xFF000000u; break; // BGRA8888
    case 16: bpp=32; rm=0x00FF0000; gm=0x0000FF00; bm=0x000000FF; am=0; break;           // BGRX8888
    case 2:  bpp=24; rm=0x0000FF; gm=0x00FF00; bm=0xFF0000; am=0; break;                 // RGB888
    case 3:  bpp=24; rm=0xFF0000; gm=0x00FF00; bm=0x0000FF; am=0; break;                 // BGR888
    case 4:  bpp=16; rm=0xF800; gm=0x07E0; bm=0x001F; am=0; break;                       // RGB565
    case 17: bpp=16; rm=0x001F; gm=0x07E0; bm=0xF800; am=0; break;                       // BGR565
    case 5:  bpp=8;  break;                                                              // I8
    case 8:  bpp=8;  break;                                                              // A8
    default: return NULL;
    }
    size_t lvl0 = fmt ? tx_bc_size(W,H,fmt) : (size_t)W*H*(bpp/8);
    size_t total = lvl0*(size_t)frames*(size_t)faces;
    if(total>n || total==0) return NULL;
    const unsigned char* s=d+n-total;                 // primer frame/cara del nivel 0
    unsigned char* out = fmt ? tx_bc(s,lvl0,W,H,fmt) : tx_masked(s,lvl0,W,H,bpp,rm,gm,bm,am);
    if(!out) return NULL;
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- KTX (OpenGL)
static unsigned char* tx_ktx(const unsigned char* d, size_t n, int* w, int* h){
    static const unsigned char sig[12]={0xAB,0x4B,0x54,0x58,0x20,0x31,0x31,0xBB,0x0D,0x0A,0x1A,0x0A};
    if(n<64 || memcmp(d,sig,12)) return NULL;
    int be = ex_le32(d+12)!=0x04030201u;
    #define KX(o) (be?ex_be32(d+(o)):ex_le32(d+(o)))
    uint32_t glType=KX(16), glFormat=KX(24), glIntFmt=KX(28);
    int W=(int)KX(36), H=(int)KX(40);
    uint32_t kvLen=KX(60);
    if(!ex_ok((uint32_t)W,(uint32_t)H)) return NULL;
    size_t off=64+(size_t)kvLen;
    if(off+4>n) return NULL;
    uint32_t imgSize=KX(off); off+=4;                 // el nivel 0 es el primero
    #undef KX
    if(off+imgSize>n) return NULL;
    int fmt=0, bpp=0; uint32_t rm=0,gm=0,bm=0,am=0;
    switch(glIntFmt){
    case 0x83F0: case 0x83F1: fmt=TX_BC1; break;
    case 0x83F2: fmt=TX_BC2; break;
    case 0x83F3: fmt=TX_BC3; break;
    case 0x8DBB: case 0x8DBC: fmt=TX_BC4; break;
    case 0x8DBD: case 0x8DBE: fmt=TX_BC5; break;
    default:
        if(glType!=0x1401) return NULL;               // solo GL_UNSIGNED_BYTE
        if(glFormat==0x1908||glIntFmt==0x8058){ bpp=32; rm=0xFF;gm=0xFF00;bm=0xFF0000;am=0xFF000000u; }
        else if(glFormat==0x1907||glIntFmt==0x8051){ bpp=24; rm=0xFF;gm=0xFF00;bm=0xFF0000; }
        else if(glFormat==0x80E1){ bpp=32; rm=0xFF0000;gm=0xFF00;bm=0xFF;am=0xFF000000u; }
        else if(glFormat==0x1903||glFormat==0x1909){ bpp=8; }
        else return NULL;
    }
    unsigned char* out = fmt ? tx_bc(d+off,imgSize,W,H,fmt)
                             : tx_masked(d+off,imgSize,W,H,bpp,rm,gm,bm,am);
    if(!out) return NULL;
    // KTX guarda la imagen de abajo hacia arriba (convencion de OpenGL)
    size_t rb=(size_t)W*4;
    unsigned char* tmp=(unsigned char*)malloc(rb);
    if(tmp){
        for(int y=0;y<H/2;y++){
            unsigned char* a=out+(size_t)y*rb, *b=out+(size_t)(H-1-y)*rb;
            memcpy(tmp,a,rb); memcpy(a,b,rb); memcpy(b,tmp,rb);
        }
        free(tmp);
    }
    *w=W; *h=H; return out;
}

// ---------------------------------------------------------------- despacho
static unsigned char* texture_load(const unsigned char* d, size_t n, const char* ext, int* w, int* h){
    if(!d || n<32) return NULL;
    if(ext){
        if(!strcmp(ext,"dds")) return tx_dds(d,n,w,h);
        if(!strcmp(ext,"vtf")) return tx_vtf(d,n,w,h);
        if(!strcmp(ext,"ktx")) return tx_ktx(d,n,w,h);
    }
    unsigned char* r;
    if(!memcmp(d,"DDS ",4) && (r=tx_dds(d,n,w,h))) return r;
    if(!memcmp(d,"VTF\0",4) && (r=tx_vtf(d,n,w,h))) return r;
    if((r=tx_ktx(d,n,w,h))) return r;
    return NULL;
}
