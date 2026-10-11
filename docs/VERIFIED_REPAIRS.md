# Verified PinePhone Pro repairs

## Validation scope

These changes were compiled and exercised on a PinePhone Pro using the pinned
LineageOS20 / Android13 sources. The final camera acceptance on2026-10-11 used
the rebuilt vendor partition and RX1 kernel#4 after a cold boot. It used the
installed modules, without camera-file bind mounts, a routing diagnostic module,
or a camera-provider restart during the acceptance tests.

The test installation uses a preserving extlinux multiboot layout on SD.
Installing this revision to eMMC is a separate task and is not claimed here.

## Camera — passed

- Rear IMX258/front OV8858 orientation and location metadata, driver probing,
  Android stream negotiation and JPEG BLOB mapping are integrated.
- ISP-native buffers are copied into Mapper4-locked Android output buffers using
  their actual plane offsets and strides. This removes the reproduced wrapping
  and wide green preview border without replacing global graphics libraries.
- Internal NV12 geometry is chroma-aligned while public photo dimensions remain
  unchanged. YUV/JPEG crop to those public dimensions and honor the actual pitch.
- RKISP1 imports the negotiated native-buffer count instead of hardcoding four
  slots. Private buffering supports concurrent preview and a still capture;
  public Android `max_buffers` remains bounded.
- Contrast AF uses real ISP sharpness, the adjustable rear actuator, eight
  settling frames and a reference measured after final settling. Front remains
  fixed-focus. Android exposes explicitly uncalibrated normalized lens positions,
  rather than inventing physical diopters.
- RX1 source selection is restored to ISP1 at every receive-PHY startup. A
  controlled single-bit experiment confirmed this routing fault caused the
  reproduced cold-boot rear no-frame condition.

Acceptance passed concurrent preview plus rear4208×3120 JPEG → front3264×2448
JPEG → reopened rear4208×3120 JPEG. Both rear sessions passed AF START → active
scan → CANCEL → inactive → START → focused locked. Original images decoded
successfully with no previously reproduced green artifact. Actual Camera2 CAF,
shutter, front/rear switching and reopen produced three fresh photos; the
stationary textured focal plane was optically checked. Provider, SurfaceFlinger
and system_server stayed stable.

Dynamic near/far continuous focus tracking has not been tested. The earlier
PDAF/table and chroma-allocation changes alone did not fix the green artifact;
the accepted image includes that tested sensor setup, but the actual artifact
repair is the internal HAL geometry/copy path. Dividing sharpness by luminance
squared was rejected after it produced false focus peaks in binned mode.

## Other compiled and exercised repairs

| Component | Verified result | Remaining scope |
|---|---|---|
| Wi-Fi | Bounded one-shot recovery helper packaged; rebuilt kernel/vendor boots, adapter scanning and a validated network connection tested. | No claim about every AP/security mode. |
| Binder/hwbinder | Handle the newer kernel's pending-frozen response; rebuilt framework remains stable during subsequent APK/camera/Bluetooth diagnostics. | Not a general Binder certification. |
| Bluetooth Linux HAL | Callback transport status and resource lifecycle fixes built; independent classic and BLE scans discovered peers. | Mixed classic+BLE command-disallowed failure and known-peer pairing are not resolved/certified. |
| Panfrost | Exported DMA-BUF is writable where camera/gralloc CPU access requires it; rebuilt graphics integration tested. | No whole-device graphics conformance claim. |
| GNSS | Real EG25 NMEA reached the registered Android GPS provider, including after cold boot; no synthetic coordinates/providers. | Satellite fix/status has not been obtained or certified. |
| Build inputs | Corrected WebView manifest entry point/pin, ATF remote and Etar dialog type; dependency patches applied and build completed. | This does not constitute a new clean full build during the final source-promotion step. |

Controlled sensor motion tests already demonstrated accelerometer/gyro response;
no speculative sensor driver changes were made for earlier stationary readings.

## Source packaging

`patches-aosp/` contains mail-format patches consumable by the existing unfold
scripts. Kernel changes are ordered in the board's existing driver and DT patch
directories. Mapper4 linkage is a build dependency of the new HAL, not a manual
runtime replacement. The final IMX258 tuning is installed through the board's
existing IPA tuning-file copy rule.

Patch application is checked against the pinned source trees before submission.
The source repository contains no generated images, APKs, signing keys, raw
device logs, private scene photos or diagnostic route-write module.

Original multiboot entries and other installed systems were preserved during
device acceptance. The repository's upstream whole-disk flashing scripts should
not be used to overwrite an existing multiboot installation.
