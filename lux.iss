; Lux — instalador (Inno Setup 6). Compilar: ISCC lux.iss  ->  dist\Lux-Setup-x.y.z.exe
#define AppName    "Lux"
#define AppVer     "1.1.0"
#define AppExe     "lux.exe"
#define AppPub     "Agustin Yarrus"
#define AppUrl     "https://github.com/agustinyarrus/lux"
; extensiones para registrar en "Abrir con"
#define Exts       ".jpg;.jpeg;.jpe;.jfif;.jif;.png;.apng;.gif;.bmp;.dib;.tif;.tiff;.ico;.cur;.ani;.mpo;.jps;.dds;.jxr;.wdp;.hdp;.webp;.heic;.heif;.heics;.heifs;.avif;.avifs;.avci;.jxl;.jp2;.j2k;.jpf;.jpx;.jpm;.jpc;.tga;.targa;.icb;.vda;.vst;.tpic;.hdr;.rgbe;.xyze;.pic;.ppm;.pgm;.pbm;.pnm;.pam;.psd;.pdd;.psb;.svg;.svgz;.qoi;.exr;.emf;.wmf;.emz;.wmz;.ora;.kra;.krz;.sketch;.procreate;.xcf;.pcx;.pfm;.ff;.farbfeld;.ras;.sun;.sgi;.rgb;.rgba;.bw;.wbmp;.xbm;.xpm;.xwd;.iff;.ilbm;.lbm;.acbm;.mac;.pntg;.macp;.dpx;.cin;.icns;.pi1;.pi2;.pi3;.pc1;.pc2;.pc3;.neo;.koa;.kla;.tim;.pix;.als;.dcx;.pcd;.fits;.fit;.fts;.dcm;.dicom;.vtf;.ktx;.3fr;.ari;.arw;.bay;.cap;.cr2;.cr3;.crw;.dcr;.dcs;.dng;.drf;.eip;.erf;.fff;.gpr;.iiq;.k25;.kdc;.mdc;.mef;.mos;.mrw;.nef;.nrw;.orf;.pef;.ptx;.pxn;.raf;.raw;.rw2;.rwl;.rwz;.sr2;.srf;.srw;.x3f"

[Setup]
AppId={{7A3C1E80-9F2B-4D6A-8E15-2C4B6D8F0A11}
AppName={#AppName}
AppVersion={#AppVer}
AppPublisher={#AppPub}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
LicenseFile=LICENSE
OutputDir=dist
OutputBaseFilename={#AppName}-Setup-{#AppVer}
SetupIconFile=lux.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0
AppReadmeFile={app}\README.md

[Languages]
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "openwith";    Description: "Registrar {#AppName} en el menu ""Abrir con"""; GroupDescription: "Integracion con Windows:"

[Files]
Source: "{#AppExe}";     DestDir: "{app}"; Flags: ignoreversion
Source: "lux.ico";       DestDir: "{app}"; Flags: ignoreversion
Source: "lux-file.ico";  DestDir: "{app}"; Flags: ignoreversion
Source: "README.md";     DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "LICENSE";       DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}";              Filename: "{app}\{#AppExe}"
Name: "{group}\Desinstalar {#AppName}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";        Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; La aplicacion en si: icono del exe (imagen.ico, indice 0) -> es como se ve Lux en "Abrir con"
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: openwith
; ProgID propio: portador del icono de los ARCHIVOS asociados (lux-file.ico) y handler al abrir.
; Los archivos toman este icono cuando el usuario elige Lux como visor predeterminado.
Root: HKA; Subkey: "Software\Classes\Lux.Image"; ValueType: string; ValueData: "Imagen"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Lux.Image\DefaultIcon"; ValueType: string; ValueData: "{app}\lux-file.ico"; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\Lux.Image\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: openwith

[Run]
Filename: "{app}\{#AppExe}"; Description: "Abrir {#AppName} ahora"; Flags: nowait postinstall skipifsilent

[Code]
procedure AddSupportedTypes(exeName, csv: String);
var rk: Integer; stKey, owpKey, ext: String; p: Integer;
begin
  if IsAdminInstallMode then rk := HKLM else rk := HKCU;
  stKey := 'Software\Classes\Applications\' + exeName + '\SupportedTypes';
  csv := csv + ';';
  repeat
    p := Pos(';', csv);
    ext := Trim(Copy(csv, 1, p-1));
    Delete(csv, 1, p);
    if ext <> '' then begin
      RegWriteStringValue(rk, stKey, ext, '');
      // ofrecer el ProgID Lux.Image como "Abrir con" para esta extension (no pisa el default);
      // al elegir Lux como predeterminado, el archivo muestra lux-file.ico
      owpKey := 'Software\Classes\' + ext + '\OpenWithProgids';
      RegWriteStringValue(rk, owpKey, 'Lux.Image', '');
    end;
  until Length(csv) = 0;
end;

procedure DelSupportedTypes(csv: String);
var rk: Integer; owpKey, ext: String; p: Integer;
begin
  if IsAdminInstallMode then rk := HKLM else rk := HKCU;
  csv := csv + ';';
  repeat
    p := Pos(';', csv);
    ext := Trim(Copy(csv, 1, p-1));
    Delete(csv, 1, p);
    if ext <> '' then begin
      owpKey := 'Software\Classes\' + ext + '\OpenWithProgids';
      RegDeleteValue(rk, owpKey, 'Lux.Image');
    end;
  until Length(csv) = 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('openwith') then
    AddSupportedTypes('{#AppExe}', '{#Exts}');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    DelSupportedTypes('{#Exts}');
end;
