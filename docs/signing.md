# 코드 서명·공증 설정

`release.yml` 은 저장소 비밀 값이 **전부** 있으면 서명(macOS 는 공증까지)을 켜고, **하나도 없으면** 서명 없이(macOS 는 ad-hoc) 배포하며, **일부만 있으면 실패**한다. 이 문서는 인증서가 생겼을 때 무엇을 준비해 어떤 비밀 값을 넣고 어떻게 검증하는지 적는다. 2026-11 기준이며, 인증서 발급 규칙은 바뀌므로 아래 "출처" 의 원문을 다시 확인한다.

## 지금 워크플로우가 하는 일

| 플랫폼 | 비밀 값 | 서명 단계 |
|---|---|---|
| macOS | `MACOS_CERT_P12`(base64 .p12), `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD` | 임시 키체인에 .p12 가져오기 → `codesign --force --deep --options runtime --timestamp -s "Developer ID Application: …"` → `--self-test` → DMG → `xcrun notarytool submit --wait` → `xcrun stapler staple` |
| Windows | `WINDOWS_CERT_PFX`(base64 .pfx), `WINDOWS_CERT_PASSWORD` | `signtool sign /f cert.pfx /p … /fd SHA256 /tr http://timestamp.digicert.com /td SHA256` 을 `onpdf_app.exe` 와 설치기(`…-setup.exe`)에 각각. 설치기는 `/DSigned=1` 로 "서명 없음" 안내를 빼고 만든다 |

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

### 현재 규칙과 우리 워크플로우의 차이
CA/Browser Forum 규칙(2023-06-01 발효)으로 **공개 신뢰 코드 서명 인증서(OV·EV)의 개인 키는 FIPS 140-2 Level 2 이상 하드웨어(토큰·HSM)에 있어야** 한다. 새로 발급받는 인증서는 **.pfx 로 내보낼 수 없다**. 따라서 워크플로우의 `WINDOWS_CERT_PFX` 경로는 **자체 서명 인증서로 파이프라인을 점검할 때**(아래)와 2023 년 이전에 발급된 소프트웨어 키에만 쓸 수 있다. 실제 배포 서명은 아래 두 방식 가운데 하나로 워크플로우를 바꿔야 한다(후속 이슈).

### 선택지
| 방식 | 내용 | 장단점 |
|---|---|---|
| **A. Microsoft Artifact Signing**(구 Trusted Signing, 2026-01 GA) | Azure 구독 + 신원 검증(개인·조직) → Azure 가 키를 보관, GitHub Action(`azure/trusted-signing-action`, OIDC 로그인)이 signtool 대행 | 하드웨어 토큰 없음, 월 구독(저렴하다는 보고가 있으나 확인 필요), Microsoft 발급이라 SmartScreen 평판에 유리. Azure 계정·신원 검증 절차가 필요 |
| **B. 상용 CA(Sectigo·DigiCert 등) OV/EV + 클라우드 HSM** | CA 가 제공하는 클라우드 서명(DigiCert KeyLocker, SSL.com eSigner) 또는 Google Cloud KMS·AWS KMS 에 키를 두고 signtool 의 CNG 공급자로 서명 | 연 수백 USD + HSM 비용, EV 는 SmartScreen 평판 즉시. 설정이 복잡 |
| C. USB 토큰 | 개인 PC 에서 수동 서명 | CI 자동화 불가 — 제외 |

권고: **A**. 1인 개발·CI 자동화·비용에서 유리하다. 전환 작업: `release.yml` 의 두 서명 단계를 "서명 제공자" 분기(PFX = 점검용 / Artifact Signing = 배포용)로 바꾸고, 비밀 값을 `AZURE_TENANT_ID`·`AZURE_CLIENT_ID`·`AZURE_SUBSCRIPTION_ID`(OIDC) + 엔드포인트·계정·프로필 이름으로 바꾼다. 설치기 `/DSigned=1` 규칙은 그대로.

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
리허설(publish 끔) 로그에 `signtool` 성공 두 번(exe·설치기), 설치기 로그에 "서명 없음" 안내가 빠졌는지 확인한 뒤, **반드시 비밀 값을 지운다**(`gh secret delete …`). 자체 서명으로 공개 릴리스를 만들지 않는다.

### 검증(실제 인증서 뒤)
- 아티팩트 zip 의 `onpdf_app.exe` 와 `…-setup.exe`: 속성 → 디지털 서명 탭에 서명자·타임스탬프, 또는 `signtool verify /pa /v onpdf_app.exe`.
- SmartScreen: OV·Artifact Signing 은 다운로드 수가 쌓여야 경고가 사라진다(수주), EV 는 즉시. README 의 "추가 정보 → 실행" 안내는 평판이 쌓인 뒤 뺀다.

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
