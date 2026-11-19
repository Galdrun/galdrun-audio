# Android rendering: findings, measurements and the partial presentation patch

Findings from profiling the Avalonia app on a phone (November 2026). The rules derived from them are in
[docs/agents/mobile-gui.md](../../docs/agents/mobile-gui.md#performance-rules-android); this file keeps the
evidence, the patch the app carries and how to keep it working across Avalonia updates.

Device for all numbers: Motorola edge 60 neo (MediaTek MT6878, Mali GPU), 1200x2670 at 120 Hz, Android 16.
Release builds, playback running unless stated.

## Partial presentation patch (what the build carries)

| Part | Where |
|---|---|
| Avalonia nightly `12.2.999-cibuild0101468-alpha` (contains AvaloniaUI/Avalonia#22501), one version for all packages | `AvaloniaVersion` in [Directory.Build.props](../Directory.Build.props) |
| Nightly feed, only for `Avalonia*` packages | [NuGet.config](../NuGet.config) |
| Enabler: sets two Avalonia internals by reflection | [PartialPresentation.cs](../src/Multiroom.App.Android/PartialPresentation.cs), called from `MainActivity.OnCreate` |
| Guard: fails when Avalonia renames those internals | [PartialPresentationTests.cs](../tests/Multiroom.App.Tests/PartialPresentationTests.cs) |

No Avalonia file is patched; the app sets the flags at run time. An Avalonia update therefore cannot overwrite
the patch, but it can rename what the patch sets. Then the patch does nothing (the app draws as before, it does
not crash) and the test or the log line below says so.

What the enabler sets, before the window's render target exists:

1. `EglGlPlatformSurface._usePartialPresentation = true`. Avalonia's opt-in flag, which only X11, Wayland and DRM
   expose as an option; Android's `TopLevelImpl` always passes `false`.
2. `EglDisplay.SupportsBufferAge = true` (its backing field), only when the driver reports
   `EGL_KHR_partial_update`. Avalonia checks only `EGL_EXT_buffer_age`, which this phone (like many Android
   drivers) does not advertise, although `EGL_KHR_partial_update` defines the same query with the same token
   (`EGL_BUFFER_AGE_KHR` = `EGL_BUFFER_AGE_EXT` = 0x313D,
   [spec](https://registry.khronos.org/EGL/extensions/KHR/EGL_KHR_partial_update.txt)). Avalonia's gate is
   `SupportsPartialPresentation = UsePartialPresentation && Display.SupportsBufferAge`.

Avalonia still checks the extensions and falls back to the full-frame path on drivers without them.

### Updating Avalonia

1. Change `AvaloniaVersion` in `Directory.Build.props` only (every project uses it).
2. Run the app tests: `dotnet test tests/Multiroom.App.Tests`. `PartialPresentationTests` fails if
   `_usePartialPresentation`, `EglDisplay`'s `SupportsBufferAge` backing field, `SupportsPartialUpdate` or
   `AvaloniaLocator.Current` changed.
3. Install on a phone and check the log, which covers the Android-only members (the activity's `_view`, its
   `TopLevelImpl`, `Surfaces`):
   `adb logcat -d -s Galdrun` must show
   `partial presentation: surface enabled, buffer age from EGL_KHR_partial_update` (or `buffer age supported`
   on a driver with `EGL_EXT_buffer_age`). `surface not found` or `left off, ...` means the patch is inactive.
4. Optional proof that it is active: with `<profileable android:shell="true" />` in the manifest temporarily,
   `simpleperf record --app audio.galdrun.controller --call-graph fp -e cpu-clock --duration 4` on the queue
   while playing; the render thread calls `eglSwapBuffersWithDamageKHR`, `eglSetDamageRegionKHR` and
   `eglQuerySurface`, not plain `eglSwapBuffers`.

Back to a release: once a stable Avalonia (12.2 or later) contains #22501, set `AvaloniaVersion` to it. If that
release also has an Android option (proposal below), use it in `MainActivity.cs`, then delete
`PartialPresentation.cs`, its test and the nightly feed in `NuGet.config`. Nightly packages are not kept
forever on the feed: if a restore fails on a clean machine, move to a newer nightly (and run the steps above) or
to the release.

## Where a frame's cost goes

Verified against the Avalonia 12.1.3 source and native profiles (simpleperf):

- Android renders with EGL (`AndroidPlatformOptions.RenderingMode` default). The EGL target never reports a
  retained frame, so without partial presentation `ServerCompositionTarget` draws the dirty area into an
  offscreen layer and copies the whole layer to the window every frame. That copy is mostly GPU work.
- Render thread CPU per frame (isolated bars page, before partial presentation): Android buffer hand-off
  (`eglSwapBuffers` -> `queueBuffer`/`dequeueBuffer`, SurfaceFlinger binder, MediaTek frame hooks) ~21%, Skia
  GPU flush ~11%, thread wake-ups ~6.5%, Mono GC ~5%, precompiled .NET code without symbols (including
  Avalonia's tree walk) ~14%.
- A full queue page frame costs about twice an isolated bars frame (6.8 vs 3.2 M cycles): dirty rectangles limit
  drawing, but Avalonia walks the whole visual tree.
- "13 ms per frame" at 15 fps was largely the CPU clocking down between sparse frames. Count cycles
  (`simpleperf stat -e cpu-cycles --per-thread`), not "% of a core".
- UI thread: each re-arm of Avalonia's Android `DispatcherTimer` goes through several Java calls, ~30 ms of UI
  thread per tick. A `DispatcherPriority.Background` timer livelocks the UI thread on Android. A hidden or
  paused Avalonia animation keeps the frame timer running (a hidden indeterminate progress bar held the UI thread
  at ~97% of a core).

## Measurements

The bars ran at 15 fps for all measurements below; the app now steps them at 10 fps (`BarFrames` in
`QueueRowView.axaml.cs`): a third fewer frames while they move (not measured separately).

UI and render thread, before and after the app changes (`UiTimer`, 15 fps compositor bars, nothing hidden
running, once-a-second seek bar):

| Situation | UI thread | Render thread |
|---|---|---|
| Home, nothing animating | 97% (hidden progress bar) -> 12.2% -> **2.6%** | 7.8% -> **2.6%** |
| Queue, playing row on screen | 46% (15 fps `DispatcherTimer`) -> **6%** | **~20%** (bars drawn at 15 fps) |
| Flicking the queue | ~5 stalls of 41-74 ms per flick -> no frame over 25 ms | |

Scroll freezes (8 fast flicks on the queue, SurfaceFlinger frame times):

| Build | Longest gap during a glide |
|---|---|
| Avalonia's default `InputStarvationTimeout` (1 s) | ~1050-1100 ms freeze in nearly every flick |
| `InputStarvationTimeout` 50 ms | at most 65-82 ms, no freezes |

Why (Avalonia 12.1.5 source): `MediaContext.ScheduleRender` posts each frame at `Render` priority, above
`Input`, plus a marker at `Input` priority. Only when that marker has waited longer than
`DispatcherOptions.InputStarvationTimeout` is the next frame posted at `Input` priority. A fling's glide posts
each step from `ScrollGestureRecognizer.OnAnimationRequested` at `DispatcherPriority.Input`, so while frames keep
the UI thread busy, the glide waits up to the whole timeout. The value is a limit, not a timer: it adds no work,
it only lets waiting input run sooner. Reported upstream as
[AvaloniaUI/Avalonia#22749](https://github.com/AvaloniaUI/Avalonia/issues/22749) (our comment there has the
cause and the workaround).

Backends and settings, bars at 15 fps (render thread):

| Variant | Render thread | Result |
|---|---|---|
| EGL, full page | 110-125 M cycles/s | baseline |
| Vulkan | same as EGL | no gain |
| Software rendering | 2.5x EGL | worse |
| Compositor animation (instead of 15 fps steps) | 183-268 M cycles/s at 61 fps | 4x the frames |
| `UseRegionDirtyRectClipping` on (AvaloniaUI/Avalonia#21538) | 109-114 M cycles/s | no difference |
| Full AOT | 110-125 M cycles/s | no gain |

Partial presentation (nightly with #22501, 3 runs each):

| Scenario | Off | On |
|---|---|---|
| Queue, bars at 15 fps, render thread | 102-115 M cycles/s | 102 M cycles/s (about -6%) |
| Home, seek bar once a second | ~10 M cycles/s | ~10 M cycles/s |
| UI thread, GPU driver threads, flick frame times | unchanged | unchanged |
| Render thread profile: present/swap, Skia flush, `dequeueBuffer` | 16.4%, 11.4%, 4.7% | 11.4%, 6.9%, 3.7% |
| Queue, bars at 15 fps, GPU active time of the app (3 x 10 s, GPU at 265 MHz throughout) | 51.4 ms/s | **23.3 ms/s (-55%)** |

Enabled at startup in the production build: render thread 114-126 M cycles/s on the full queue page (noisy
across runs; the comparison above used the same page for both). No drawing errors while scrolling, animating,
or going to the background and back. Rotation untested (the app is portrait only). The main saving is on the GPU
(the full-screen layer copy is gone); power was not measured (the phone charges over USB). Partial presentation
does not remove Android's buffer hand-off, Mono GC on the render thread or Avalonia's tree walk.

GPU time without root: the kernel's `power/gpu_work_period` ftrace event reports GPU active periods per app UID,
and Perfetto may record it. Config (push to `/data/misc/perfetto-configs/`, run
`perfetto --txt -c <config> -o /data/misc/perfetto-traces/gpu.pftrace`, then sum the slice durations of the
`android_gpu_work_period` track whose `uid` is the app's, from `dumpsys package audio.galdrun.controller`, with
Perfetto's trace processor, e.g. the `perfetto` Python package):

```
buffers { size_kb: 32768 fill_policy: RING_BUFFER }
data_sources { config { name: "linux.ftrace" ftrace_config {
  ftrace_events: "power/gpu_work_period"
  ftrace_events: "power/gpu_frequency"
} } }
duration_ms: 10000
```

Compare runs only at the same GPU frequency (the `gpu_frequency` counter).

## Upstream proposal

Posted as [AvaloniaUI/Avalonia#22896](https://github.com/AvaloniaUI/Avalonia/issues/22896).

Four changes would make the patch unnecessary:

```diff
--- a/src/Avalonia.OpenGL/Egl/EglDisplay.cs
-            SupportsBufferAge = extensions.Contains("EGL_EXT_buffer_age");
+            // EGL_KHR_partial_update defines the same query (EGL_BUFFER_AGE_KHR == EGL_BUFFER_AGE_EXT).
+            SupportsBufferAge = extensions.Contains("EGL_EXT_buffer_age") ||
+                                extensions.Contains("EGL_KHR_partial_update");

--- a/src/Avalonia.OpenGL/Avalonia.OpenGL.csproj
     <InternalsVisibleTo Include="Avalonia.X11, PublicKey=$(AvaloniaPublicKey)" />
+    <InternalsVisibleTo Include="Avalonia.Android, PublicKey=$(AvaloniaPublicKey)" />

--- a/src/Android/Avalonia.Android/AndroidPlatform.cs   (AndroidPlatformOptions)
+        /// <summary>
+        /// Enables partial presentation when supported by the rendering backend (currently EGL).
+        /// When <c>null</c>, uses the platform default, which is currently disabled.
+        /// </summary>
+        public bool? UsePartialPresentation { get; set; }

--- a/src/Android/Avalonia.Android/Platform/SkiaPlatform/TopLevelImpl.cs
-            var gl = new EglGlPlatformSurface(this);
+            var gl = new EglGlPlatformSurface(this,
+                AvaloniaLocator.Current.GetService<AndroidPlatformOptions>()?.UsePartialPresentation ?? false);
```

Rationale for the issue text: #22501 added opt-in EGL partial presentation for X11, Wayland and DRM. On Android
there is no opt-in, and drivers that advertise only `EGL_KHR_partial_update` and
`EGL_KHR_swap_buffers_with_damage` (here Mali on MediaTek) never pass the buffer age gate. The partial
presentation measurements above can be attached.
