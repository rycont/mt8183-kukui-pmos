# Plasma Mobile Input Mode

Plasma Mobile에서 터치 입력과 키보드 커버 입력을 자동 전환하는 Alpine
Linux 패키지다.

| KWin 태블릿 상태 | 입력 방식 | Plasma Mobile UI |
|---|---|---|
| `tabletMode=true` | Plasma Keyboard OSK | 모바일 모드 |
| `tabletMode=false` | IBus + Hangul | 독 모드 |

빠른 설정의 **Input Mode** 타일로 두 모드를 수동 전환할 수도 있다. 자동
서비스는 다음 실제 태블릿 모드 변경 전까지 수동 선택을 유지한다.

## 검증 환경

- Lenovo IdeaPad Duet Chromebook (`google-krane`, MT8183 Kukui)
- postmarketOS v26.06, systemd
- Plasma Mobile 6.6.6 / KWin 6.6.6
- IBus 1.5.34 / ibus-hangul

## 감지 방식

자동 서비스는 일반 입력 장치 목록을 세지 않는다. KWin의
`org.kde.KWin.TabletModeManager` D-Bus 인터페이스가 제공하는 하드웨어
태블릿 모드만 감시한다. 따라서 다음 장치는 커버 부착 상태를 바꾸지 않는다.

- 연결된 키보드가 없는 Logi Unifying 수신기
- 일반 USB 키보드
- Bluetooth 키보드

KWin이 `tabletModeAvailable=false`를 보고하는 기기에서는 자동 전환을
유휴 상태로 두며 빠른 설정 타일만 수동으로 사용할 수 있다.

현재 상태는 다음처럼 확인할 수 있다.

```sh
busctl --user get-property \
  org.kde.KWin /org/kde/KWin \
  org.kde.KWin.TabletModeManager tabletModeAvailable

busctl --user get-property \
  org.kde.KWin /org/kde/KWin \
  org.kde.KWin.TabletModeManager tabletMode
```

## 빌드

Alpine의 `alpine-sdk`와 사용자 abuild 키가 필요하다.

```sh
sudo apk add alpine-sdk
sudo addgroup "$USER" abuild
abuild-keygen -a -i
```

그룹 변경을 적용하려면 다시 로그인한 뒤 이 디렉터리에서 빌드한다.

```sh
abuild checksum
abuild -r
```

빌드는 셸 문법, `metadata.json`, QML 바이트코드 컴파일을 검사한다. 결과 APK는
기본 설정에서 `~/packages/<repo>/<arch>/` 아래에 생성된다.

## 설치

로컬 abuild 공개키를 시스템 키 저장소에 설치했다면:

```sh
sudo apk add /path/to/plasma-mobile-input-mode-systemd-0.3.0-r0.apk
```

공개키를 등록하지 않은 다른 기기에서 시험할 때만:

```sh
sudo apk add --allow-untrusted \
  /path/to/plasma-mobile-input-mode-systemd-0.3.0-r0.apk
```

설치 후 한 번 로그아웃하고 다시 로그인한다. 패키지가 제공하는 user systemd
서비스는 `plasma-workspace.target`에서 기본 활성화되며 Plasma Shell 시작 전에
빠른 설정 타일을 등록한다. 별도 setup 명령은 없다.

## 수동 조작

```sh
plasma-input-mode hardware  # IBus + Hangul
plasma-input-mode touch     # Plasma Keyboard OSK
```

Hardware 모드는 `ibus start --type kde-wayland`를 사용한 뒤 전역 엔진을
`hangul`로 지정한다. Touch 모드는 KWin의 `InputMethod`를 Plasma Keyboard로
되돌리고 실행 중인 입력기 프로세스를 다시 만든다.

## 진단

```sh
ibus engine
pgrep -a -f 'ibus|plasma-keyboard'
systemctl --user status plasma-input-mode-auto.service
journalctl --user -u plasma-input-mode-auto.service -b
```

Hardware 모드에서는 `ibus engine`이 `hangul`을 출력하고
`ibus-engine-hangul` 프로세스가 보여야 한다. Touch 모드에서는
`plasma-keyboard` 프로세스가 보여야 한다.

## 제거

입력기를 먼저 Plasma Keyboard로 복구한 뒤 패키지를 제거한다.

```sh
plasma-input-mode touch
sudo apk del plasma-mobile-input-mode-systemd
```

서비스는 기존 기본 가상 키보드 타일을 삭제하지 않으므로 패키지를 제거해도
기본 타일 설정은 보존된다. 존재하지 않는 사용자 정의 타일 ID가 설정에 남아도
Plasma Mobile이 무시한다.

## 파일 구성

- `plasma-input-mode`: 실제 입력기 전환
- `plasma-input-mode-auto`: KWin 태블릿 모드 감시와 독 모드 연동
- `plasma-input-mode-auto.service`: Plasma 세션 user service
- `inputmode-main.qml`, `metadata.json`: Plasma Mobile 빠른 설정 플러그인
- `APKBUILD`: Alpine 패키지 정의와 정적 검사

## 라이선스

이 디렉터리의 코드는 [MIT](LICENSE) 라이선스로 배포한다.
