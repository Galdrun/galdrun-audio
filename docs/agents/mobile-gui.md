# Mobile app (Avalonia) implementation map

Paths below are relative to `mobile-gui/` unless stated otherwise. Overview and
build: [README](../../mobile-gui/README.md); desktop build scripts:
[development.md](development.md). The app talks to the server's REST API
like any client ([server.md](server.md)).

## Entry points

- `src/Multiroom.App/MainView.axaml` and `ShellViewModel.cs`: the shell, its
  pages (shown by `IsVisible`, never removed) and navigation.
- `src/Multiroom.App/QueueView.axaml.cs`: the queue (drag, swipe, tap) on
  `RecyclingRowPanel.cs`, one `QueueRowView` per visible row.
- `src/Multiroom.App/ArtworkView.cs` and `src/Multiroom.App.Api/ArtworkCache.cs`:
  pictures, fetched at the size they are drawn (`?size=`), kept in memory and
  on disk.
- `src/Multiroom.App.Android/MainActivity.cs`: Android entry point and the
  app builder options; enables EGL partial presentation
  (`PartialPresentation.cs`).

## Buttons

The app's own button is the **Galdrun button**: class `galdrun` (style in
`src/Multiroom.App/App.axaml`). Flat and square (no corner radius, no
border), background `#2C372F`, text `#CBEC97` SemiBold 16, height 48; no
hover or press change (on touch it would stick); disabled: `#1B2123` with
`#5A6462` text. Use it for a page's actions (Zone Setup's "Update zone", the
Manual page's Connect and Back, the connecting page's Manual); stretch side
by side ones in equal columns. Icon buttons (transport, header, marks) are
transparent with a coloured icon instead.

## Android build and device test

Not built by the scripts. Needs the `android` workload of the SDK in
`global.json` and an Android SDK/JDK (`-t:InstallAndroidDependencies` once).
From `mobile-gui/`:

```powershell
dotnet publish src/Multiroom.App.Android -f net10.0-android -c Release -p:AndroidPackageFormat=apk "-p:AndroidSdkDirectory=$env:LOCALAPPDATA\Android\Sdk" "-p:JavaSdkDirectory=$env:LOCALAPPDATA\Android\jdk"
adb install -r src/Multiroom.App.Android/bin/Release/net10.0-android/publish/audio.galdrun.controller-Signed.apk
```

Delete `src/Multiroom.App.Android/obj` and `bin` after changing Android build
properties (`AndroidEnableProfiler`, the manifest): an incremental build keeps
stale native glue and the app dies at start with `UnsatisfiedLinkError`.
Measure in Release on a device; the desktop and headless tests hide every cost
below.

## Performance rules (Android)

A frame has a fixed cost however little changed. Verified against Avalonia
12.1.3 and a native profile (simpleperf) on a MediaTek/Mali phone: Android
renders with EGL (`AndroidPlatformOptions.RenderingMode` default); the EGL target
never reports a retained frame, so `ServerCompositionTarget` draws the dirty
area into an offscreen layer and blits the whole layer each frame (GPU work).
The render thread's CPU per frame goes mostly to Android's buffer hand-off
(`eglSwapBuffers` -> `queueBuffer`/`dequeueBuffer`, SurfaceFlinger binder,
vendor frame hooks) and Skia's GPU flush, then Mono GC and Avalonia's walk of
the visual tree (a full page costs about twice an empty one). Vulkan costs the
same, software rendering 2.5 times more, and region dirty-rect clipping or full
AOT change nothing measurable. EGL partial presentation (AvaloniaUI/Avalonia#22501,
Avalonia 12.2: buffer age, damage regions) removes the layer copy: for the playing bars
55% less GPU time and about 6% less render thread CPU; Avalonia has no Android option for it, so the app
pins a 12.2 nightly and turns it on by reflection
(`src/Multiroom.App.Android/PartialPresentation.cs`). Evidence, measurements and the
Avalonia update procedure: [android-rendering.md](../../mobile-gui/docs/android-rendering.md).
On the UI thread, each re-arm of Avalonia's
Android timer (`DispatcherTimer`) makes several calls into Java. So the number
of frames and timer ticks decides the cost. Hence:

- **Nothing hidden runs.** An endless animation must stop while its control is
  not shown, its page and the viewport included. Avalonia keeps its frame
  timer going for a hidden or paused animation (measured: a hidden progress
  bar kept the UI thread at ~100% of a core). Mark such a control with
  `VisibilityWatch.TrackShown` in its style and run the animation only on the
  class `shown` (see `Arc.busy` in `App.axaml`); in code, use
  `VisibilityWatch.Attach`. An indeterminate `ProgressBar` is indeterminate
  only while its work runs. Rows of `RecyclingRowPanel` report
  `IsInView`; a row's decoration animates only in view.
- **No `DispatcherTimer` for a repeating tick.** Each tick re-arms an Android
  `Handler` through Java (~30 ms of UI thread per tick, profiled). Use
  `UiTimer` (thread-pool timer that posts the tick). Never a
  `DispatcherPriority.Background` timer: on Android it waits for an idle
  looper, its overdue timer re-posts at once, and the UI thread spins.
- **Few frames for decoration.** A compositor animation runs at the display's
  120 Hz (the queue's four playing bars: ~47% of the render thread). Drive
  decorative motion with `UiTimer` at a low rate and set the compositor visual
  (`ElementComposition.GetElementVisual(...).Scale`), which needs no layout
  (`QueueRowView`: 10 frames a second). Update text and progress at the rate
  they visibly change (`SeekSlider`: once a second).
- **Long lists keep their rows.** Avalonia's `VirtualizingStackPanel` removes
  and re-adds a recycled row and restyles it each time; `ListBox` also clears
  its content and rebuilds the item template. Use `RecyclingRowPanel` for long
  lists of one row height (the queue; Home's "See all" grid, as rows of covers
  whose height it re-measures when the width changes), and give rows what they need from their owner once
  (`QueueRowView`'s constructor) instead of `$parent[...]` bindings.
- **Pictures at the size drawn.** `ArtworkView` asks for the server's
  thumbnail size (`ArtworkCache.SizeFor`) and decodes at device pixels;
  never decode an original for a list row.
- **App builder:** `InputStarvationTimeout` is 50 ms (`MainActivity.cs`): with
  Avalonia's 1 s default a fling's glide starved behind rendering and stopped
  for a second (glide steps run at `Input` priority; cause and
  AvaloniaUI/Avalonia#22749 in
  [android-rendering.md](../../mobile-gui/docs/android-rendering.md#measurements)).
  Avalonia 12.1 already leaves `UseRegionDirtyRectClipping`
  off (AvaloniaUI/Avalonia#21538); do not turn it on.

### Measuring

- In the app: Settings → Show performance shows, once a second over every
  page, the whole app's CPU in % of all cores and the UI and render threads
  in % of one core with their core's clock
  (`src/Multiroom.App/PerformanceView.cs`, from `/proc/self/task` and
  `/sys`). A thread's % is time at that clock, not work. For numbers to
  compare between builds, use the tools below.

- CPU cycles per thread (independent of clock speed, which drops between
  sparse frames and makes "% of a core" misleading):
  `simpleperf stat --app audio.galdrun.controller -e cpu-cycles --per-thread --duration 5`
  (needs the `profileable` manifest entry below). Runs vary by tens of percent
  as threads move between big and little cores: compare repeated runs.
- CPU time per thread: read `/proc/<pid>/task/*/stat` (utime+stime) twice, e.g.
  over 5 s; `top -H` output must not be cut with `tail`, or the main thread
  goes missing. Idle with nothing moving should be about 0%.
- Frames: `adb shell dumpsys SurfaceFlinger --latency-clear`, flick with
  `input swipe`, then `dumpsys SurfaceFlinger --latency '<BLAST layer>'`;
  gaps above 8 ms (120 Hz) are dropped frames.
- Managed profile: build with `-p:AndroidEnableProfiler=true`, set
  `debug.mono.profile` to `127.0.0.1:9000,nosuspend,connect`, run
  `dotnet-dsrouter android` and `dotnet-trace collect -p <dsrouter pid>`.
  Clear the property afterwards, or the next start waits for a tracer.
- GPU active time per app, without root: Perfetto with the ftrace event
  `power/gpu_work_period` (config and query in
  [android-rendering.md](../../mobile-gui/docs/android-rendering.md#measurements)).
- Native profile (all threads, waits included): add
  `<profileable android:shell="true" />` to the manifest temporarily and run
  `simpleperf record --app audio.galdrun.controller --call-graph fp --trace-offcpu`.

## Invariants

- No endless animation, repeating timer or polling runs while its control is
  not shown. Reason: on Android every frame and every timer tick costs the UI
  and render threads far more than the work itself (measurements above).
- Every Avalonia package uses `AvaloniaVersion` from `Directory.Build.props`.
  Reason: the partial presentation enabler sets Avalonia internals; after a
  version change, `PartialPresentationTests` and the device log line
  `partial presentation: surface enabled` show whether it still works
  ([procedure](../../mobile-gui/docs/android-rendering.md#updating-avalonia)).
- Every "Play now" (item sheet, Play in room, Browse, collections) goes
  through `src/Multiroom.App/PlayNow.cs`. Reason: the Settings switch
  `PlayNowKeepsQueue` decides whether it replaces the zone's queue or adds after
  the playing track and plays that; separate code paths would disagree.
