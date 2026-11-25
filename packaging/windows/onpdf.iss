; OnPdf Windows 설치기 (Inno Setup 6, #99). release.yml 이 zip 과 같은 stage 폴더를 입력으로 만든다:
;   ISCC /DAppVersion=0.5.1 /DStageDir=<stage 절대 경로> /DRepoDir=<저장소> /DOutputDir=<출력 폴더> packaging\windows\onpdf.iss
; 사용자별 설치가 기본(관리자 권한 없음, %LocalAppData%\Programs\OnPdf), 설치 유형에서 "모든 사용자" 를 고를 수 있다.
; 레지스트리는 HKA(관리자면 HKLM, 아니면 HKCU)로 설치 모드를 따른다. .pdf 는 "연결 프로그램" 에 항상 등록하고,
; 기본 앱 지정은 꺼진 작업(pdfdefault)으로 둔다 — Windows 10/11 은 사용자 선택(UserChoice)을 우선하므로 강제하지 않는다.

#ifndef AppVersion
  #error AppVersion is required: ISCC /DAppVersion=x.y.z
#endif
#ifndef StageDir
  #error StageDir is required: ISCC /DStageDir=<staged app folder>
#endif
#ifndef RepoDir
  #define RepoDir "..\.."
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#define AppName "OnPdf"
; 고정 GUID — 바꾸면 이전 설치를 다른 앱으로 보아 업그레이드가 안 된다. AppId 와 제거 키({GUID}_is1) 가 같이 쓴다
#define AppGuid "6F5FEE03-64B4-45E4-A23D-1AC76FAB33AC"
#define AppExe "onpdf_app.exe"
#define ProgId "OnPdf.Document"

[Setup]
AppId={{{#AppGuid}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=OnPdf project
AppPublisherURL=https://github.com/Edgar-J-Shin/OnPdf
AppSupportURL=https://github.com/Edgar-J-Shin/OnPdf/issues
AppUpdatesURL=https://github.com/Edgar-J-Shin/OnPdf/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#RepoDir}\LICENSE
SetupIconFile={#RepoDir}\app\onpdf.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
ChangesAssociations=yes
OutputDir={#OutputDir}
OutputBaseFilename=OnPdf-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
#ifexist "compiler:Languages\Korean.isl"
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
#else
  #pragma message "Korean.isl not found in this Inno Setup - installer UI will be English only"
#endif

[CustomMessages]
en.PdfDefault=Open PDF files with OnPdf (you may be asked to confirm the default app)
en.Unsigned=OnPdf is not code-signed yet. If Windows SmartScreen warns, choose "More info" and "Run anyway". Verify the download with SHA256SUMS.txt from the release.
#ifexist "compiler:Languages\Korean.isl"
ko.PdfDefault=PDF 파일을 OnPdf 로 열기 (Windows 가 기본 앱 확인을 요청할 수 있습니다)
ko.Unsigned=OnPdf 는 아직 코드 서명이 없습니다. Windows SmartScreen 경고가 뜨면 "추가 정보" → "실행" 을 누르세요. 내려받은 파일은 릴리스의 SHA256SUMS.txt 로 확인할 수 있습니다.
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "pdfdefault"; Description: "{cm:PdfDefault}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; ProgId: 아이콘·열기 명령. 제거 때 키째 지운다
Root: HKA; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueData: "PDF Document (OnPdf)"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""
; "연결 프로그램" 목록 (항상). 제거 때 값만 지운다 (.pdf 키는 다른 앱 것일 수 있다)
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: string; ValueName: "{#ProgId}"; ValueData: ""; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\SupportedTypes"; ValueType: string; ValueName: ".pdf"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\Applications\{#AppExe}\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""
; 기본 앱 (작업을 켰을 때만). 제거 때는 [Code] 가 우리 값일 때만 지운다
Root: HKA; Subkey: "Software\Classes\.pdf"; ValueType: string; ValueData: "{#ProgId}"; Tasks: pdfdefault

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[Code]
// 업그레이드: 같은 AppId 의 이전 설치가 있으면 그 제거기를 조용히 먼저 돌린다 — 새 버전에서 빠진 파일이
// {app} 에 남지 않게 (#101). 사용자 문서는 {app} 에 없다. 제거기는 자기 삭제 때문에 비동기로 끝나므로 잠시
// 기다린다
function PreviousUninstaller(var Path: String): Boolean;
var
  Key: String;
begin
  // SetupSetting("AppId") 는 이스케이프된 '{{GUID}' 를 돌려줘 키 이름과 다르다 → GUID 로 직접 만든다
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#AppGuid}}_is1';
  Result := RegQueryStringValue(HKEY_AUTO, Key, 'UninstallString', Path);
  if not Result then
    Result := RegQueryStringValue(HKEY_CURRENT_USER, Key, 'UninstallString', Path);
  if not Result then
    Result := RegQueryStringValue(HKEY_LOCAL_MACHINE, Key, 'UninstallString', Path);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Uninstaller: String;
  ResultCode: Integer;
  I: Integer;
begin
  Result := '';
  if PreviousUninstaller(Uninstaller) then
  begin
    Uninstaller := RemoveQuotes(Uninstaller);
    if FileExists(Uninstaller) then
    begin
      Log('Uninstalling the previous version first: ' + Uninstaller);
      if Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      begin
        for I := 0 to 30 do
        begin
          if not FileExists(Uninstaller) then Break;
          Sleep(1000);
        end;
      end
      else
        Log('Previous uninstaller could not be started; continuing with the upgrade');
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Value: String;
begin
  if CurUninstallStep = usPostUninstall then
    if RegQueryStringValue(HKEY_AUTO, 'Software\Classes\.pdf', '', Value) and (Value = '{#ProgId}') then
      RegDeleteValue(HKEY_AUTO, 'Software\Classes\.pdf', '');
end;

#ifndef Signed
procedure InitializeWizard();
begin
  // 서명 인증서가 생기기 전까지: 첫 화면에 SmartScreen 안내. 워크플로우가 서명하면 /DSigned=1 로 뺀다 (#101)
  WizardForm.WelcomeLabel2.Caption := WizardForm.WelcomeLabel2.Caption + #13#10#13#10 + CustomMessage('Unsigned');
end;
#endif
