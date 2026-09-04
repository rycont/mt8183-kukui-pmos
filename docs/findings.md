# 알아낸 것들

작업하며 문서에 없거나, 검색으로 안 나오거나, 잘못 알고 있던 것들.

## 1. MTISP 10비트는 CSI-2 패킹이 아니라 연속 비트스트림

`V4L2_PIX_FMT_MTISP_S*10` (`MBBA`/`MBGA`/`MBgA`/`MBRA`) 은 표준 MIPI
RAW10(상위 바이트 4개 + 하위비트 1바이트)이 **아니다**. 픽셀 n 이 바이트
스트림의 비트 `[10n, 10n+10)` 에 리틀엔디언으로 들어가는 연속 비트스트림이다.

판별법 — 인접 픽셀 차이의 중앙값을 비교하면 바로 갈린다:

| 해석 | 인접 픽셀차 중앙값 |
|---|---|
| 표준 MIPI RAW10 | 261 |
| **연속 비트스트림 (LE)** | **25** |

틀린 해석은 픽셀을 뒤섞으므로 고주파가 난수가 되고 저주파(지역 평균)만
살아남는다. 그래서 **"여러 장 평균 내면 형체가 보이는데 원본은 순수
노이즈"** 라는 증상이 나온다. 노이즈로 오진하기 쉽다.

행 시작은 바이트 정렬이고 4픽셀 = 40비트 = 5바이트가 정확히 맞아떨어지므로,
5바이트씩 읽어 4픽셀을 뽑으면 비트 단위 접근보다 훨씬 빠르다
(`tools/bridge.c` 참고, 처리 시간 252ms → 92ms).

## 2. MTISP 8비트는 그냥 비패킹 베이어

`MBB8`/`MBG8`/`MBg8`/`MBR8` 은 이름에 "Packed 8-bit" 라고 붙어 있지만
실제로는 1바이트/픽셀이다 (`Bytes per Line: 1600` @ 1600px, 인접 픽셀차
중앙값 1.0).

즉 `V4L2_PIX_FMT_SBGGR8` 등 표준 fourcc 와 메모리 배치가 동일하다.
이 사실 덕분에 libcamera 에 MTISP 전용 디베이어를 구현하지 않고,
드라이버가 표준 fourcc 를 함께 노출하는 것만으로 소프트웨어 ISP 를
그대로 쓸 수 있었다.

## 3. camtg_sel 은 clk_set_rate 로 못 바꾼다

센서 마스터 클럭 mux (`CLK_TOP_MUX_CAMTG`) 에는 `CLK_SET_RATE_PARENT`
플래그가 있다. 그래서 `clk_set_rate()` 가 요청을 부모(고정 분주기)로
넘겨버리고, mux 재선택이 일어나지 않는다.

```
clk_set_rate(xvclk, 19200000) = 0   (성공 반환)
clk_get_rate(xvclk)           = 13000000   (그대로)
```

반환값이 0이라 실패를 알아채기 어렵다. `clk_set_parent()` 를 써야 한다.
ChromeOS DT 가 센서에 `freq_mux` 라는 두 번째 클럭을 넘기는 이유가 이것이다.

참고로 mt8183 의 camtg mux 부모 중 19.2MHz 를 낼 수 있는 것은 없다
(26 / 13 / 24 / 48 / 12 / 6 / 52MHz). 실제로는 24MHz 로 동작한다.

## 4. DT 에 있어도 핀mux 를 빠뜨리면 센서가 조용히 죽는다

`camera_pins_cam0` 에 리셋 핀만 넣고 `PINMUX_GPIO99__FUNC_CMMCLK0`
(센서로 나가는 마스터 클럭 출력) 을 빠뜨리면, 전원·리셋·i2c 가 모두
정상인데 센서만 i2c 응답을 안 한다.

진단의 열쇠는 **같은 모듈의 다른 칩과 비교**하는 것이었다.
전원이 켜진 상태에서 버스를 훑으면:

```
0x0c  dw9768 AF VCM   → 응답    (클럭 불필요)
0x58  EEPROM          → 응답    (클럭 불필요)
0x10  OV8856 센서     → 무응답  (클럭 필요)
```

## 5. pmOS 커널은 Clang + kCFI 빌드다

GCC 로 만든 모듈을 넣으면 첫 간접 호출에서 커널이 죽는다:

```
CFI failure at do_one_initcall (target: seninf_pdrv_init; expected type: 0x6fbb3035)
Internal error: Oops - CFI
```

`CONFIG_CC_IS_CLANG=y`, `CONFIG_CFI=y` 이므로 `make LLVM=1` 로 빌드해야 한다.
모듈을 만들기 전에 `/boot/config` 의 `CC_VERSION_TEXT` 를 확인할 것.

## 6. libcamera 의 uvcvideo 위장은 불가능하다

`simple` 이 아닌 `uvcvideo` 파이프라인 핸들러로 붙이려 해도,
`UVCCameraData::generateId()` 가 sysfs 에서 firmware node 를 찾아 올라가고
부모 디렉터리의 `idVendor`/`idProduct` 를 읽는다. platform 디바이스에는
둘 다 없으므로 미디어 디바이스 이름만 바꿔서는 안 된다.

## 7. libcamera 는 미디어 디바이스의 driver_name 으로 매칭한다

`struct media_device` 의 `driver_name` 필드를 채우면
`MEDIA_IOC_DEVICE_INFO` 가 그 값을 반환한다. 커널 문서상 USB 드라이버를
위한 것이지만, 이 값이 곧 libcamera 의 `DeviceMatch` 대상이다.

## 8. sink pad 포맷을 source pad 로 전파하지 않으면 해상도가 고정된다

libcamera 의 `simple` 파이프라인 핸들러는 체인의 각 엔티티에 대해
**sink pad 에 포맷을 `set` 하고 source pad 에서 `get` 한다.**
그 사이의 전파는 드라이버 책임이다 (V4L2 서브디바이스 규약).

seninf 와 mtk-cam-p1 은 원래 pad 별로 포맷을 따로 저장만 했다. 그래서
센서를 3280x2464 로 올려도 source pad 는 `init_state` 기본값 1600x1200 을
계속 돌려줬고, 후면 카메라(8MP)가 1596x1200 으로 고정됐다.

```
ov8856[0] -> seninf[0]:  3280x2464   ← 센서 pad 는 정상
seninf[4] -> mtk-cam-p1: 1600x1200   ← 전파 없음
```

하드웨어는 멀쩡했다. `media-ctl` 로 체인을 수동으로 3280x2464 에 맞추면
프레임당 8,081,920 바이트(= 3280x2464 8비트 베이어)가 정확히 나온다.
증상은 libcamera 쪽에 보이지만 원인은 드라이버 두 곳이다.

## 9. libcamera 의 카메라 인덱스는 재부팅마다 바뀐다

`cam -c1` 이 어제는 후면(ov8856)이었는데 재부팅 후 전면(ov02a10)이 됐다.
async 서브디바이스 바인딩 순서에 따라 열거 순서가 달라진다. 스크립트에서는
인덱스 대신 `cam --list` 가 출력하는 고정 ID 를 써야 한다.

```sh
cam -c "/base/soc/i2c@11009000/camera@10"   # 후면
cam -c "/base/soc/i2c@11008000/camera@3d"   # 전면
```

## 10. MTISP 벤더 fourcc 는 enum_fmt 마다 커널 WARN 을 찍는다

`Unknown pixelformat 0x3852424d` (= `"MBR8"`) 백트레이스가 카메라 열거
때마다 20 여 회 쌓인다. `v4l2-ioctl.c` 의 `v4l_enum_fmt` 이 코어의 설명
테이블에서 벤더 fourcc 를 못 찾아 내는 경고다. 동작에는 영향이 없다.

## 11. 소프트웨어 ISP 에는 자동초점이 없다

libcamera 0.7.1 의 `src/ipa/simple/algorithms/` 에는 `agc`, `awb`, `blc`,
`ccm`, `adjust` 만 있다. AF 알고리즘이 없고, `simple` 파이프라인 핸들러
소스에는 `focus`/`lens`/`vcm` 이라는 단어조차 나오지 않는다. 렌즈
서브디바이스를 바인딩하지 않으므로 VCM 을 DT 에 연결해도 libcamera 는
건드리지 않는다.

DT 연결의 효과는 커널 쪽에 국한된다 — dw9768 이 `V4L2_CID_FOCUS_ABSOLUTE`
를 노출하므로 `v4l2-ctl` 로 수동 초점을 맞출 수 있다. 자동으로 하려면
초점을 훑으면서 선명도 피크를 찾는 대비검출 AF 를 유저스페이스에서 직접
짜야 한다.

## 12. 소프트 ISP 의 AGC 는 센서 헬퍼 없이는 게인을 못 올린다

`CameraSensorHelper` 가 없으면 IPA 가 게인 레지스터 값과 실제 배율 사이를
변환하지 못해 AGC 가 노출만 조절하고 게인은 최소값에 방치한다. 증상은
`IPASoft: Failed to create camera sensor helper for <sensor>` 경고 한 줄과,
`cam --list-controls` 에 `AnalogueGain`/`ExposureTime` 이 안 보이는 것이다.

krane 에서는 60프레임을 돌려도 ov8856 이 이랬다:

```
exposure       max=2482   value=2482   ← 끝까지 올림
analogue_gain  min=128    value=128    ← 손도 안 댐 (16배 미사용)
```

libcamera 0.7.1 에 등록된 OV 센서는 ov13858, ov2685, ov2740, ov4689,
ov5640, ov5647, ov5670, ov5675, ov5693, ov64a40, ov8858, ov8865 뿐이라
ov8856 과 ov02a10 은 직접 추가해야 한다. 둘 다 선형 매핑이고 배율 기준값은
커널 드라이버에 있다 — ov8856 은 128 이 1x(최대 2047, 16x), ov02a10 은
0x10 이 1x(최대 0xf8, 15.5x).
