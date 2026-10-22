# Galdrun web style

`theme.css` is the canonical stylesheet for client pages and future server
webpages. Match the supplied Device Settings reference while keeping the Galdrun
name. Change shared tokens here instead of copying the theme into each project.

## Appearance

- Warm ivory background (`--bg: #f5f1e9`) and near-white cards (`--card: #fdfbf7`).
- Dark blue text (`--fg: #182333`), muted slate labels (`--muted: #626d7c`).
- Teal actions and icons (`--accent: #1d706d`), pale sage icon circles (`--soft`).
- Subtle warm borders, restrained shadows, 16px card corners, 8px control corners.
- Native system fonts; 28px page titles, 20px card titles, 16px inputs, 14px labels.
- A centered 1040px page, an 82px brand header, and a quiet footer.
- Desktop cards have an icon/title/description column occupying 32% of the card,
  a vertical divider, and a content column with aligned labels and inputs.
- At 850px the introduction becomes compact. At 600px cards and fields stack;
  the divider becomes horizontal. Keep controls usable at 320px viewport width.
- The theme stays light, matching the reference regardless of the OS theme.

Use simple inline SVG line icons with `currentColor` and `aria-hidden="true"`.
Device photography is optional decoration; do not require remote image or font
requests, especially in a captive portal. Show real device/connection state;
do not add buttons unless the underlying operation exists.

## Webpage integration

Serve this file with your webpage's static assets and load it once:

```html
<link rel="stylesheet" href="/assets/theme.css">
```

That URL is an example: configure the server to serve the CSS at that path.
Load page-specific styles afterward. Shared selectors style native form elements,
so apply this theme to a whole page, not as an isolated widget in another theme.

Card structure:

```html
<section class="settings-card" aria-labelledby="client-heading">
  <div class="card-intro">
    <div class="card-icon"><!-- decorative inline SVG --></div>
    <div>
      <h2 id="client-heading">Client</h2>
      <p>Set how this device appears in your audio system.</p>
    </div>
  </div>
  <div class="card-content fields">
    <label for="room">Room name</label>
    <input id="room" aria-describedby="room-hint">
    <p class="hint" id="room-hint">How the server lists this room.</p>
  </div>
</section>
```

Use `.card-content` without `.fields` for a status `<dl>`. `.row` groups an input
and an adjacent action. `.field-group` lets conditional fields participate in the
parent grid; toggle `.hidden` on the group. `.table-wrap` contains a native table.
`.unit-input` wraps an input and a unit `<span>`. `.form-actions` holds save buttons
and a `.message` with `role="status"`; add `.bad` for errors. `.primary` marks the
main action. `.connection.online` is reserved for a confirmed connection.

Keep unique IDs, visible input labels, table headers, keyboard focus outlines,
error feedback, and reduced-motion support. Use `textContent` for device names
and server-provided values. Do not replace working settings/API behavior for a
visual change. Add `aria-describedby` when a hint explains a control.

## Embedded C++ pages

Both client build systems include `EmbedStyle.cmake`. It generates
`MultiroomWebStyle.inc` in the current binary directory from the canonical CSS and
tracks CSS changes so an ordinary build regenerates it. Add that directory to the
target's include paths, and concatenate the fragment between the page literals:

```cpp
const char kPage[] = R"html(<style>)html"
#include "MultiroomWebStyle.inc"
R"html(</style>)html";
```

The served page contains the CSS inline, so desktop and ESP32 clients need no
runtime stylesheet file or additional HTTP route. Do not edit the generated file.
Keep `)css"` out of CSS source because it closes the C++ raw string literal.

### The client settings page

There is one settings page for every client: `client-settings.html` (markup)
and `client-settings.js` (script). `EmbedStyle.cmake` turns them into
`MultiroomClientSettings.inc` and `MultiroomClientSettingsScript.inc`
(`multiroom_embed_text`: adjacent raw literals cut at line ends below MSVC's
16 KiB literal limit). `ControlHttp.cpp` defines `kClientSettingsPage` from it
and serves it at `/` on the control port; the ESP32's `WebUi.cpp` serves the
same bytes on port 80. Two markers in the HTML are replaced:
`@MULTIROOM_BRAND_NAME@` by the C macro, and `@INCLUDE <file>@` by an `#include`
of another generated fragment (the style and the shared scripts). Keep both
files ASCII (entities in HTML, `\u` escapes in JS) and keep `)page"` and
`)script"` out of them.

The script tells the two servers apart by `GET /api/settings`: the ESP32 answers
it, the control port answers 404. The control backend drives the `/api` line
commands and shows the CPU, LED, power, DAC, drift, Ethernet and Linux network
cards as the client reports them supported; the board backend uses
`/api/settings`, `/api/networks`, `/api/status` and `/api/inputs`, shows the I2S
clock, buffer and external-DAC format cards, and saves with a restart. Cards
start `hidden`; a backend reveals what it provides, and `navigation.js` hides
the menu links of the rest.

Verify client changes with `cmake --build client/build --config Release` and
`ctest --test-dir client/build -C Release --output-on-failure`. Build firmware with
`build-client.ps1 -Target <board>` (Linux: `build-client.sh <board>`). Preview desktop and phone widths and check scans,
conditional IP/clock fields, save feedback, and keyboard navigation.

## Hardware input information

`inputs.js` renders button and encoder details using the existing
`inputs` control response. The Linux page calls its `/api` command; the ESP32
page uses `/api/inputs`, which forwards the running client's response and adds
the board's setup-button GPIO. In setup mode it shows saved input definitions
and states that the hardware backend is not running. Windows hides this section.

Keep input IDs, GPIO pins or Linux device/key/axis names, every gesture's assigned
action, volume steps/values/scopes, and backend errors visible. An encoder's push
button is a separate button input. The list reports configuration and discovered
inputs; it cannot prove that a physically wired control works.

Pass an optional async `saveBindings(id, bindings)` callback to enable editing.
It returns the canonical `bindings` saved by `setInputBindings`. Linux uses the
existing `/api` command; ESP32 posts to `/api/inputs`, including in setup mode.
Actions persist in the client configuration and apply without restarting.
Volume actions expose percentage and room/zone scope beside a fixed-height action selector on desktop and beneath it on phones.
Use the page's single floating Save button for all changes;
refreshes preserve focused, unsaved or saving editors. GPIO/device information
stays beside the input heading and retains its muted style. On phones, editable
gesture rows use one column so action names remain readable. Do not execute
playback actions just to test a binding.

The build embeds the shared script as `MultiroomWebInputs.inc`. Open
`tests/inputs.html` in a browser for the standalone rendering check, including
GPIO 0, encoders, evdev controls, empty/error/setup states, and escaped names.

## Sticky section menu

Place `.section-menu` inside `.topbar`, immediately after `.topbar-inner`. Its
`.section-menu-links` contains ordinary anchors pointing to each card's heading
ID. Give those headings `tabindex="-1"` so keyboard navigation can focus the
destination. The brand header and menu stick together at the top. A full-width
ivory menu row with matching line icons and teal active items sits directly below the brand.
All links remain visible: the menu wraps as needed, and phones use up to three
columns with 44px touch targets. Do not hide links behind horizontal scrolling.
Keep menu and card order aligned: Status, Wi-Fi, Address, Client, Audio, then
Buttons & encoders. Omit sections that a platform does not provide.

`navigation.js` adds each card's existing SVG icon, hides links to hidden cards,
and marks the current group with `aria-current="location"`. Native anchors and
CSS handle scrolling; a ResizeObserver measures the combined header height so
heading scroll margins keep destinations below both rows at any phone width.
Reduced-motion users get immediate scrolling without animation. The embedded
clients include `MultiroomWebNavigation.inc`; static server webpages can load this
script at the end of the body. Open `tests/navigation.html` for the standalone
navigation check.


## Floating Save button

Give the body `has-save-bar` and place `.form-actions.save-bar` outside the main
form, with a submit button associated through `form="form"`. Keep it disabled
and gray until settings or hardware bindings differ from their saved values.
`container.inputEdits` exposes `dirty`, `validate()` and async `save()`; listen
for the bubbling `bindingschange` event. Validate every draft before saving and
preserve unsaved drafts after errors. Use `formnovalidate` on the button and
validate the settings form only when settings changed, since hardware edits
can be saved independently. ESP device settings require a reboot: label the
button `Save and restart` only for those changes. Binding-only saves apply live.
The bar respects phone safe areas and the page reserves space below the footer.


Hardware editors use one `.input-info` card per input, with a square hardware
icon, title, short hardware description and muted GPIO/device pill in the header.
These cards are subgroups inside the single `#hardwareBox.settings-card` parent
group. Keep the parent card's background, border, icon and group heading visible;
use the standard two-column parent layout and vertical divider on desktop.
On phones use the same stacked layout and horizontal divider as other groups.
The hardware content is a CSS size container: nested gesture controls stack
according to the available column width, rather than just the viewport width.
Gesture rows show a circular icon, gesture name and explanatory subtitle.
Each row has labeled Action, conditional Amount (percent), conditional Scope,
and an action explanation below. Use native selects with decorative SVGs so
keyboard and phone selection remain accessible. Scope and amount share the
control row on desktop; phones put Action above the remaining controls.
Hardware descriptions stay generic unless the backend supplies actual metadata.

Each nested input uses native `details`/`summary`, collapsed initially. The
summary lists assigned gestures, actions, volume percentages and room/zone
scopes; unassigned gestures are omitted, with an explicit empty state when none
are assigned. Summaries follow local edits and expanded cards stay expanded
after refreshes. Invalid values and save errors open their card for correction.

## Live hardware states

Load `input-events.js` after `inputs.js` for standalone pages (embedded clients
include both through `MultiroomWebInputs.inc`). Call
`connectHardwareInputEvents(container, url)` once the running client is known.
It opens an EventSource to `GET /api/input-events` on the existing control port.
Linux uses the same origin; ESP port-80 settings provide `inputEventsPort` and
the browser connects directly to that port on the same hostname. The control
listener must be reachable from the browser. Setup mode has no running input
backend and does not open a feed.

The SSE stream sends `input-snapshot` with an array of `{id, pressed}` states
at connection/reconnection, then `input-state` with one `{id, pressed}` change.
States come from debounced physical button edges, including unassigned buttons;
an encoder push switch is a separate button. Simulated playback gestures do not
pretend to be physical presses. The badge becomes green while held and resets
on release or lost connection, even when its card is collapsed. Feed updates
never replace configuration editors or change their dirty state.

One connection is used per visible page; hidden pages release it. Automatic
reconnection restores held states and page refreshes restore badge states from
the latest snapshot. The existing control-server thread handles streaming;
there are no extra threads, larger stacks or browser polling requests. A 20ms
select timeout is used only while viewers are present; idle SSE keepalives are
sent every 10 seconds. The event queue is bounded to 128 edges, overflow sends
a current snapshot, and unread output is bounded to 16KiB per viewer. ESP
permits two simultaneous viewers, other clients four, reserving control slots.
Cross-port CORS is limited to the same device hostname; unrelated origins and
DNS-rebinding hostnames are rejected.

## Initial page loading

Client pages start with `page-loading` and `aria-busy="true"` on the body, and
a `page-loader` overlay with `pageLoader`, `pageLoaderMessage` and
`pageLoaderRetry` elements. Load `loading.js` and call `finishPageLoading` with
the initial settings/status/input promise. It reveals the page atomically on
success; failure or a 15-second timeout keeps the incomplete page hidden and
shows an error with a Retry button. Background refreshes do not show the loader.
The input feed's `inputUsageReady` promise waits for a usable CPU reading or two
samples when counters are unsupported. A live-feed error releases that optional
wait so connection limits cannot prevent access to otherwise loaded settings.

## CPU and RAM charts

The Status group places two equal columns below Format, including on phones.
Load `usage-charts.js` and call `createUsageCharts(container)` on a container with
class `usage-charts`. The shared event feed handles `system-usage` events with
`{cpuPercent, ramPercent, ramUsedBytes, ramTotalBytes, wifiRssiDbm}`. CPU is total device CPU across all cores; RAM is
`100 * (total - available) / total` (ESP includes its available heap/PSRAM).
Values update every 500ms through the existing SSE connection. CPU counters,
memory and the Wi-Fi link are sampled on the existing socket thread while
viewers are present; full process diagnostics retain their existing schedule.
Each SVG keeps 60 samples on a fixed 0–100% scale. Missing values and disconnected
feeds show an em dash and leave a gap in history, rather than reporting zero.
RAM also shows used and total sizes in binary units (KiB/MiB/GiB), with exact
byte counts in its tooltip. The Wi-Fi group uses the same chart style and
30-second history with a fixed -100 to -30 dBm scale; stronger signals are
higher. Pass its `wifi-chart` container as the optional second argument to
`createUsageCharts`. The current dBm reading stays exact even outside the plot
range, and missing/disconnected links show an em dash and a gap.
`createUsageCharts` returns its update function (also `window.updateUsageCharts`), so a
page can hold several sets, one per machine, as the ServerGui's System page does.
Its optional third argument is the sample interval in ms (default 500), which only
sets the history label (60 samples at 1000ms read "60s ago").
CPU shows `cpuModel` and `cpuMhz` below its percentage. The cached platform CPU
model and MHz share one line. The model shortens with an ellipsis and its full
text is in the tooltip; MHz stays visible on phones. The platform clock is the current
first-core frequency on Linux, rated frequency on Windows, and configured
frequency on ESP. Unknown clocks are labelled unavailable.
