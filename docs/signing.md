# 코드 서명·공증 설정

`release.yml` 은 저장소 비밀 값이 **전부** 있으면 서명(macOS 는 공증까지)을 켜고, **하나도 없으면** 서명 없이(macOS 는 ad-hoc) 배포하며, **일부만 있으면 실패**한다. 이 문서는 인증서가 생겼을 때 무엇을 준비해 어떤 비밀 값을 넣고 어떻게 검증하는지 적는다. 2026-11 기준이며, 인증서 발급 규칙은 바뀌므로 아래 "출처" 의 원문을 다시 확인한다.

## 지금 워크플로우가 하는 일

| 플랫폼 | 비밀 값 | 서명 단계 |
|---|---|---|
| macOS | `MACOS_CERT_P12`(base64 .p12), `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | 임시 키체인에 .p12 가져오기 → `codesign --force --deep --options runtime --timestamp -s "Developer ID Application: …"` → `--self-test` → DMG → `xcrun notarytool submit --wait` → `xcrun stapler staple` |
| Windows 배포용 | `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_SUBSCRIPTION_ID`, `SIGNING_ENDPOINT`, `SIGNING_ACCOUNT_NAME`, `SIGNING_CERT_PROFILE` | **Microsoft Artifact Signing**(#116): `azure/login`(GitHub OIDC, 비밀 키 없음) → `azure/artifact-signing-action@v2` 가 `onpdf_app.exe` 와 설치기(`…-setup.exe`)를 각각 서명(SHA256, 타임스탬프 `timestamp.acs.microsoft.com`) → `signtool verify /pa` 로 공개 신뢰 체인 확인. 설치기는 `/DSigned=1` 로 "서명 없음" 안내를 빼고 만든다 |
| Windows 점검용 | `WINDOWS_CERT_PFX`(base64 .pfx), `WINDOWS_CERT_PASSWORD` | `signtool sign /f cert.pfx …` — 자체 서명으로 파이프라인만 확인(아래). 배포용과 **동시에 설정하면 실패** |

비밀 값이 없을 때: macOS 는 `codesign -s -`(ad-hoc) 라 첫 실행에 Control-클릭 → 열기가 필요하고, Windows 는 SmartScreen 경고가 뜬다(README 안내).

## macOS — Developer ID 서명 + 공증

### 준비
1. **Apple Developer Program** 가입(개인 또는 조직, 연 99 USD).
2. **Developer ID Application 인증서**: 개발 Mac 의 키체인 접근 → 인증서 지원 → 인증 기관에서 인증서 요청(CSR 저장) → developer.apple.com → Certificates → `+` → *Developer ID Application* → CSR 업로드 → 내려받아 더블클릭(키체인에 설치). 유효 기간 5년.
3. **.p12 내보내기**: 키체인에서 그 인증서(개인 키 포함)를 선택 → 내보내기 → .p12 + 암호. 이 암호가 `MACOS_CERT_PASSWORD`.
4. **공증 자격**: Apple ID 는 2단계 인증이 켜져 있어야 하고, appleid.apple.com → 로그인 및 보안 → 앱 암호 → 새 암호(예: "OnPdf CI")를 만든다. 이것이 `APPLE_APP_PASSWORD`. Team ID 는 developer.apple.com → Membership details.
   - 대안: App Store Connect API 키(`notarytool --key …`). 워크플로우는 지금 Apple ID + 앱 암호 방식이다.
5. 로컬에서 먼저 확인(선택): `xcrun notarytool store-credentials "OnPdf" --apple-id <id> --team-id <team> --password <앱 암호>` 가 성공하면 자격이 맞다.

### 비밀 값 등록
```bash
gh secret set MACOS_CERT_P12 < <(base64 -i DeveloperID.p12)
gh secret set MACOS_CERT_PASSWORD   # .p12 암호
gh secret set APPLE_ID              # Apple ID 이메일
gh secret set APPLE_TEAM_ID         # 10자 Team ID
gh secret set APPLE_APP_PASSWORD    # 앱 암호 (xxxx-xxxx-xxxx-xxxx)
```
다섯 개를 **한 번에** 넣는다(일부만 있으면 리허설이 실패한다 — 의도된 안전장치).

### 검증
1. Actions → Release → Run workflow(ref `develop`, publish 끔) → `macos-arm64`·`macos-x64` 잡 로그에 `NOTARIZE=1`, `notarytool … status: Accepted`, `stapler … The staple and validate action worked!`.
2. 아티팩트 DMG 를 받아 Mac 에서:
   ```bash
   hdiutil attach OnPdf-*-macos-arm64.dmg
   codesign -dv --verbose=4 "/Volumes/OnPdf */OnPdf.app"      # Authority=Developer ID Application: …, Timestamp=…, flags=…(runtime)
   spctl --assess --type execute -vv "/Volumes/OnPdf */OnPdf.app"   # accepted, source=Notarized Developer ID
   xcrun stapler validate OnPdf-*-macos-arm64.dmg
   ```
3. 공증이 거절되면 `xcrun notarytool log <submission-id> --apple-id … --team-id … --password …` 로 사유(JSON)를 본다. 흔한 사유: 하드닝 런타임 없는 바이너리(워크플로우는 `--options runtime` 을 쓴다), 서명 안 된 dylib(`finish_macos_bundle.py` 가 번들 밖 의존을 막는다), 타임스탬프 없음.

### 운영
- 인증서 만료(5년)·앱 암호 폐기·Apple ID 비밀번호 변경 때 비밀 값을 갱신한다. 리허설(publish 끔)로 먼저 확인한다.
- 워크플로우는 러너마다 임시 키체인을 만들고 끝나면 러너가 버려지므로 개인 키가 남지 않는다. .p12 는 저장소에 넣지 않는다.
- Developer ID Installer 인증서는 필요 없다(pkg 를 만들지 않는다).

## Windows — 코드 서명

### 왜 클라우드 서명인가
CA/Browser Forum 규칙(2023-06-01 발효)으로 **공개 신뢰 코드 서명 인증서(OV·EV)의 개인 키는 FIPS 140-2 Level 2 이상 하드웨어(토큰·HSM)에 있어야** 한다. 새로 발급받는 인증서는 **.pfx 로 내보낼 수 없다**. 그래서 워크플로우는 두 제공자를 둔다(#116): **배포용 Microsoft Artifact Signing**(구 Trusted Signing, 2026-01 GA — Azure 가 키를 보관하고 GitHub Action 이 서명)과 **점검용 PFX**(자체 서명으로 파이프라인만 확인). 대안인 상용 CA(Sectigo·DigiCert 등) OV/EV + 클라우드 HSM(DigiCert KeyLocker·SSL.com eSigner·Cloud KMS + signtool CNG 공급자)은 연 수백 USD + HSM 비용이 들고 설정이 복잡해 쓰지 않는다. USB 토큰은 CI 자동화가 안 된다.

### 준비 (Artifact Signing)
1. **Azure 구독** 생성(종량제). 리소스 그룹 하나.
2. **Artifact Signing 계정**: Azure Portal → "Artifact Signing"(Trusted Signing) → 계정 만들기. 지역을 고르면 엔드포인트가 정해진다(예: East US → `https://eus.codesigning.azure.net/`, West Europe → `https://weu.codesigning.azure.net/`). 계정 이름이 `SIGNING_ACCOUNT_NAME`, 엔드포인트가 `SIGNING_ENDPOINT`.
3. **신원 검증**(Identity validation): 개인(Individual) 또는 조직(Organization). 개인은 정부 신분증·주소 확인, 조직은 등록 서류 — 수일. **Public Trust** 유형.
4. **인증서 프로필**(Certificate profile): 유형 *Public Trust*, 위 신원 검증에 연결. 이름이 `SIGNING_CERT_PROFILE`. 서명자 이름(Subject)은 검증된 신원 그대로 들어간다.
5. **GitHub OIDC 용 앱 등록**(비밀 키 없음):
   - Microsoft Entra ID → 앱 등록 → 새 등록(예: `onpdf-github-release`). 애플리케이션(클라이언트) ID → `AZURE_CLIENT_ID`, 디렉터리(테넌트) ID → `AZURE_TENANT_ID`, 구독 ID → `AZURE_SUBSCRIPTION_ID`.
   - 그 앱 → 인증서 및 비밀 → **페더레이션 자격 증명** → GitHub Actions: 조직 `Edgar-J-Shin`, 저장소 `OnPdf`, 엔터티 **Environment**, 환경 이름 `release`. subject 는 `repo:Edgar-J-Shin/OnPdf:environment:release`, audience `api://AzureADTokenExchange`. (워크플로우의 `windows-x64` 잡이 `environment: release` 를 쓰므로 태그·브랜치·PR 어느 실행이든 subject 가 같다 — 페더레이션 자격 증명은 와일드카드를 받지 않는다.)
   - Artifact Signing 계정(또는 인증서 프로필) → 액세스 제어(IAM) → 역할 할당 → **Artifact Signing Certificate Profile Signer** 를 그 앱에.
   - 저장소의 `release` 환경은 Settings → Environments 에 이미 있다(보호 규칙 없음). 없으면 만든다.
6. (선택) 로컬 확인: `az login` 뒤 `signtool`(Windows SDK) + Trusted Signing dlib 로 임의 exe 서명 — 필수는 아니다. 리허설(아래)이 같은 것을 CI 에서 한다.

### 비밀 값 등록
```bash
gh secret set AZURE_TENANT_ID
gh secret set AZURE_CLIENT_ID
gh secret set AZURE_SUBSCRIPTION_ID
gh secret set SIGNING_ENDPOINT        # https://<region>.codesigning.azure.net/
gh secret set SIGNING_ACCOUNT_NAME
gh secret set SIGNING_CERT_PROFILE
```
여섯 개를 **한 번에** 넣는다(일부만 있으면 리허설이 실패한다). `WINDOWS_CERT_PFX`·`WINDOWS_CERT_PASSWORD` 가 남아 있으면 "동시에 설정" 으로 실패하므로 지운다. 엔드포인트·계정·프로필은 비밀이 아니지만 "전부 또는 없음" 규칙을 한 곳에서 보려고 secrets 로 둔다.

### 검증
1. Actions → Release → Run workflow(ref `develop`, publish 끔) → `windows-x64` 잡 로그: `signing provider: azure`, `Azure login (OIDC)` 성공, `Sign with Artifact Signing` 와 `Sign the installer with Artifact Signing` 성공, `Verify the signature`·`Verify the installer signature` 에 `Status: Valid` 와 `signtool verify /pa` 의 "Successfully verified". `Build the installer` 로그에 `/DSigned=1`(설치기 첫 화면의 "서명 없음" 안내가 빠진다).
2. 아티팩트 zip 의 `onpdf_app.exe` 와 `…-setup.exe` 를 Windows 에서: 속성 → 디지털 서명 탭에 서명자(검증된 신원 이름)·타임스탬프, 또는 `signtool verify /pa /v onpdf_app.exe`.
3. 흔한 실패: `AADSTS70021`/`No matching federated identity record` → 페더레이션 자격 증명의 subject·audience 확인(환경 이름 `release`). `403` → 역할 할당(Certificate Profile Signer)이 계정 범위에 없음. 엔드포인트 지역 불일치 → 계정을 만든 지역의 엔드포인트.

### 운영
- Artifact Signing 인증서는 **짧은 수명(며칠)으로 자동 갱신**되므로 만료 관리가 없다. 타임스탬프가 있어 서명은 인증서 만료 뒤에도 유효하다.
- 신원 검증은 주기적으로 재검증을 요구할 수 있다(포털 알림). 구독 결제가 끊기면 서명 단계가 403 으로 실패하고 릴리스가 나가지 않는다(의도된 안전장치).
- 앱 등록에는 비밀 키가 없다(OIDC). 저장소 secrets 가 새도 서명 권한은 이 저장소의 `release` 환경에서 돌아가는 워크플로우에만 있다.

### 파이프라인 점검용 자체 서명(지금 할 수 있는 것)
자체 서명 인증서는 사용자 PC 에서 신뢰되지 않지만(SmartScreen 경고 그대로) **워크플로우의 서명 단계가 동작하는지**는 확인할 수 있다.
```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=OnPdf CI test" -CertStoreLocation Cert:\CurrentUser\My
$pw = ConvertTo-SecureString "test-password" -AsPlainText -Force
Export-PfxCertificate -Cert $cert -FilePath onpdf-test.pfx -Password $pw
[Convert]::ToBase64String([IO.File]::ReadAllBytes("onpdf-test.pfx")) | Set-Content onpdf-test.pfx.b64
```
```bash
gh secret set WINDOWS_CERT_PFX < onpdf-test.pfx.b64
gh secret set WINDOWS_CERT_PASSWORD   # test-password
```
리허설(publish 끔) 로그에 `signing provider: pfx`(경고 "점검용"), `Sign with PFX` 성공 두 번(exe·설치기), 검증 단계에 서명자 `CN=OnPdf CI test`(상태는 `NotSigned` 가 아니면 됨 — 자체 서명이라 `UnknownError`), 설치기 로그에 "서명 없음" 안내가 빠졌는지 확인한 뒤, **반드시 비밀 값을 지운다**(`gh secret delete …`). 자체 서명으로 공개 릴리스를 만들지 않는다.

### SmartScreen
- OV·Artifact Signing 은 다운로드 수가 쌓여야 경고가 사라진다(수주), EV 는 즉시. `packaging/README-windows.txt` 의 "추가 정보 → 실행" 안내는 평판이 쌓인 뒤 뺀다.

## 공통 체크리스트
- [ ] 비밀 값은 플랫폼별로 **전부** 넣는다(일부만 있으면 리허설 실패).
- [ ] 먼저 `workflow_dispatch`(publish 끔)로 리허설 → 로그·아티팩트 검증 → 그다음 태그.
- [ ] 비밀 값 교체: 새 값을 넣고 리허설 → 통과 뒤 옛 인증서 폐기.
- [ ] 인증서·앱 암호·토큰은 저장소에 커밋하지 않는다(CLAUDE.md "커밋 금지").
- [ ] 서명이 켜진 첫 릴리스 뒤 README 의 서명 없음 안내(Windows SmartScreen, macOS Control-클릭)를 갱신한다.

## 비용·기간 (대략, 확인 필요)
| 항목 | 비용 | 기간·주기 |
|---|---|---|
| Apple Developer Program | 99 USD/년 | 가입 승인 1~2일, 인증서 5년 |
| Microsoft Artifact Signing | 월 구독(확인 필요) | Azure 신원 검증 수일 |
| 상용 OV 코드 서명 + 클라우드 HSM | 연 200~500 USD + HSM | 신원 검증 1~5일, 인증서 최대 ~460일(2026 규칙) |

## 출처
- Apple, [Customizing the notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow) — notarytool·앱 암호·2단계 인증
- GitHub Actions 에서 macOS 서명·공증 예(커뮤니티): [dev.to](https://dev.to/federicoterzi/automatic-code-signing-and-notarization-for-macos-apps-using-github-actions-1ne8), [Simon Willison TIL](https://til.simonwillison.net/electron/sign-notarize-electron-macos)
- 코드 서명 키 하드웨어 요건(CA/B Forum 2023-06)·유효 기간 변경: [sslcertshop 정리](https://dev.to/sslcertshopcom/code-signing-certificates-in-2026-hardware-tokens-hsms-and-modern-developer-workflows-154), [IdenTrust](https://identrust.com/blog-entry/overcoming-code-signing-roadblocks-handling-non-exportable-private-keys)
- Microsoft Artifact Signing GA(2026-01): [Microsoft Tech Community](https://techcommunity.microsoft.com/blog/microsoft-security-blog/-/4482789); 클라우드 KMS 로 signtool 쓰는 예: [Percy CLI PR](https://github.com/percy/cli/pull/2168), [dev.to](https://dev.to/katz/building-a-cost-effective-windows-code-signing-pipeline-sectigo-google-cloud-kms-on-github-2ghf)
- Tauri 의 Windows 서명 안내(Azure 서명 포함): [tauri.app](https://v2.tauri.app/distribute/sign/windows/)
