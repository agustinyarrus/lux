<div align="center">

# Lux

**Un visor de imágenes nativo, dark y minimalista para Windows.**

Win32 puro + Direct2D + WIC. Sin frameworks, sin runtime, sin dependencias. Un solo `.exe` portable de ~360 KB que abre al instante.

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)
![Windows](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D6?logo=windows&logoColor=white)
![Direct2D](https://img.shields.io/badge/render-Direct2D%20%2B%20WIC-7AA2F7)
![Size](https://img.shields.io/badge/exe-~360%20KB-9ECE6A)
![License](https://img.shields.io/badge/License-MIT-9ECE6A)

[![Descargar](https://img.shields.io/badge/Descargar-Setup_%2B_Portable-7AA2F7?style=for-the-badge&logo=github&logoColor=white)](https://github.com/agustinyarrus/lux/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/agustinyarrus/lux/total?style=for-the-badge&color=9ECE6A&label=descargas)](https://github.com/agustinyarrus/lux/releases)

<img src="docs/screenshot.png" alt="Lux" width="820">

</div>

---

## ✨ Qué es

**Lux** es el hermano de bajo nivel de [Lumen](https://github.com/agustinyarrus/lumen): la misma estética
oscura y sin distracciones, pero escrito en **C++ nativo** contra la API de Windows. La imagen se decodifica
con **WIC** (el motor del sistema) y se dibuja con **Direct2D/DirectWrite**, acelerado por GPU. La ventana es
**frameless** —la barra de título la dibuja la propia app—, con zoom al cursor suave y un fondo _ambient_
desenfocado. **Funciona 100% offline** y no carga ningún runtime: es Win32 a secas.

## 🖼️ Formatos

Lux abre prácticamente todo. **Vectorial** rasterizado con nanosvg (nítido a cualquier zoom):

> `svg`

**Por WIC**, con los códecs del sistema / Microsoft Store:

> `jpg` · `jpeg` · `png` · `gif` · `bmp` · `tiff` · `ico` · `dds` · `jxr` / `hdp` · `webp`* · `heic`* / `heif`* · `avif`* · `jxl`* · `raw`* (`cr2` `cr3` `nef` `arw` `dng` `orf` `rw2` `raf` `srw` `pef` …)

**Por decoders propios integrados** (header-only, sin dependencias):

> `qoi` · `exr` (OpenEXR HDR) · `pcx` · `pfm` · `ff` (farbfeld) · `ras` / `sun` · `sgi` / `rgb` / `bw` · `wbmp` · `pam` · `xbm` · `tga` · `hdr` · `ppm` / `pgm` / `pbm` / `pnm` · `pic` · `psd`

<sub>Si el formato de imagen existe, es muy probable que Lux lo abra.</sub>

<sub>* webp/heic/avif/jxl usan las extensiones de códec de Windows (gratis en Microsoft Store; en Windows 11 suelen venir preinstaladas). Si falta alguna, Lux te avisa cuál instalar.</sub>

## 🎛️ Características

- **Ventana pegada a la imagen** o **tamaño libre**: un toggle en la barra de título (o la tecla `W`) elige si cada imagen ajusta la ventana a su tamaño, o si conservás el tuyo. La preferencia y el tamaño/posición se **recuerdan entre sesiones**.
- **Render Direct2D** con interpolación bicúbica de alta calidad al reducir y _nearest_ al hacer pixel-peeping (≥300 %).
- **Zoom al cursor** con la rueda, paneo al arrastrar, doble clic para alternar ajuste ↔ 100 %.
- **Fondo _ambient_**: una versión desenfocada de la imagen llena el escenario (efecto Gaussian de D2D).
- **Navegación por carpeta** en orden natural (`foto2` antes que `foto10`), con índice _n / total_.
- **Cromo que se autooculta**: barra y HUD se desvanecen tras unos segundos de inactividad y el cursor desaparece.
- **Frameless real**: barra de título propia, botones dibujados a mano, esquinas redondeadas y borde oscuro de Windows 11.
- **Arranque sin _flash_**: la ventana nace oscura desde el primer pixel.
- Arrastrar y soltar, pantalla completa, y un `.exe` portable que no deja nada instalado.

## ⌨️ Atajos

| Tecla                       | Acción                         |
|-----------------------------|--------------------------------|
| `←` `↑` · `→` `↓` `Espacio`  | Imagen anterior / siguiente    |
| `rueda`                     | Zoom al cursor                 |
| `doble clic`                | Ajuste ↔ detalle (100 %)       |
| `+` / `-` / `0` / `1`       | Zoom in / out / ajustar / 100 %|
| `Inicio` / `Fin`            | Primera / última imagen        |
| `F` / `F11`                 | Pantalla completa              |
| `Esc`                       | Salir de pantalla completa / cerrar |
| `Ctrl O`                    | Abrir                          |
| `W`                         | Ventana pegada a la imagen (on/off) |

## ⚙️ Configuración

Lux guarda sus preferencias en **`%APPDATA%\Lux\config.json`** (se crea solo). Lo podés editar a mano o usar el toggle de la barra de título (el ícono de marco, se pone azul cuando está activo):

```json
{
  "fitWindowToImage": false,
  "maximized": false,
  "winX": 100, "winY": 100,
  "winW": 1100, "winH": 720
}
```

- **`fitWindowToImage: true`** — al abrir o navegar, la ventana toma el tamaño de la imagen (limitada al monitor) y la muestra a 100 %. Es la opción "ventana pegada".
- **`fitWindowToImage: false`** — la ventana conserva el tamaño y la posición que vos le des; se recuerdan entre sesiones (`winX/Y/W/H`, `maximized`).

## 📦 Compilar

> Requiere **Visual Studio 2022** (o Build Tools) con el _Desktop development with C++_ y el Windows 10/11 SDK.

```powershell
.\build.ps1        # compila lux.exe (release, /O2 /MT, sin consola, con icono y manifest)
.\build.ps1 -Run   # compila y abre una imagen de prueba
.\build.ps1 -Dbg   # build con símbolos + consola para diagnóstico
```

Los iconos son assets curados del repo: **`lux.ico`** (icono de la app, embebido en el `.exe`) y
**`lux-file.ico`** (icono de los archivos asociados, vía el ProgID `Lux.Image`). El script opcional
`.\gen-icon.ps1` regenera el sparkle azul-noche original y **sobreescribe `lux.ico`**.

El resultado es un único **`lux.exe`** portable. Luego:

```powershell
lux.exe foto.jpg
```

o asocialo en _Abrir con…_ y usalo como visor por defecto.

## 🏗️ Arquitectura

| Pieza                  | Rol                                                                            |
|------------------------|--------------------------------------------------------------------------------|
| `lux.cpp`              | Todo: ventana frameless, decodificación (WIC + stb + SVG + QOI), render D2D, input, navegación |
| `third_party/`         | Decoders _header-only_: `stb_image`, `nanosvg` (SVG), `qoi`, `tinyexr` (EXR, reusa el zlib de stb), `exotic.h` (pcx/farbfeld/pfm/sun/sgi/wbmp/pam/xbm) |
| `lux.manifest`         | DPI _per-monitor v2_, common controls, code page UTF-8                          |
| `lux.rc`               | Icono + versión + manifest embebidos                                           |
| `lux.ico` / `lux-file.ico` | Iconos curados (16→256, 32-bit): la app y los archivos asociados (ProgID `Lux.Image`) |
| `gen-icon.ps1`         | Regenera el `lux.ico` original (squircle) con System.Drawing — opcional           |
| `build.ps1`            | Localiza MSVC (vcvars) y compila con `cl` + `rc`                                |

Toda la decodificación pasa por una cadena con _fallback_: los formatos típicos van por **WIC**
(acelerado, con orientación y códecs del sistema); los raros (`ppm`, `tga`, `hdr`, `psd`…) por **stb_image**;
si una vía falla, se intenta la otra.

## 📄 Licencia

MIT © Agustín Yarrus — ver [LICENSE](LICENSE).
