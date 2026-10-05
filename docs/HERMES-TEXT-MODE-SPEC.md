# Hermes text reply mode

Status: approved for implementation on 2026-10-05, retaining landscape and current-reply scrolling. Baseline reviewed against repository commit `cecec6f0a04f7826e1abe197575f0633d4c65a5f`. The design below distinguishes baseline SDK behavior from the local changes. Physical speaker cutoff timing remains a hardware validation item.

## Goal and assumptions

Keep using the microphone to ask Hermes questions, with a saved **Voice replies: On / Off** control. With voice replies off, read the answer on the T-Embed display and turn the knob to scroll without hearing the response.

Scope: the Hermes app on the standard T-Embed, using its existing push-to-talk gesture and 320 × 170 landscape display. Scrolling moves through the **current reply**. The inspected core does not contain a conversation-history browser. Quiet speech still depends on the existing microphone and gateway transcription; this change makes no promise about whisper recognition or noise rejection.

Recommended first version: device-enforced silence, persistent preference, and stable manual reading. The gateway may still generate and transmit speech. Avoiding TTS work is outside this first version.

## Verified baseline

These are existing capabilities and limitations, not proposed APIs:

| Finding | Source |
| --- | --- |
| The imported Gadget core is pinned to `edecb81c22f1c9308338735b0613be3d341e94ac`. | [Core provenance](../platform/hermes_core/PROVENANCE.md) |
| The setup guide pins the host plugin separately to `c3df35f82746d1f711ee1ee885d46552210dac2a`; T-Embed pins ESP-IDF to `25fe69f946311abdaf9ad56591f25fedbc20ac98`. | [Gateway setup](../tools/README-t-embed-hermes.md#L7), [build profile](../firmware-targets.json#L45) |
| Input and output audio have separate HAL interfaces. `AudioOut::abort()` requires dropping buffered output. | [SDK HAL contract](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/firmware/core/include/hg/hal.hpp#L43-L64) |
| The protocol carries microphone audio, transcription, reply text, and output audio separately. `reply.delta.text` is cumulative, not a fragment to append. | [SDK conversation protocol](https://github.com/Adolanium/hermes-gadget-sdk/blob/c3df35f82746d1f711ee1ee885d46552210dac2a/docs/protocol.md#conversation) |
| The core has a volume setting but no dedicated voice-reply switch. It stores one reply; starting another recording clears it. Each streaming preview resets scrolling, final text starts at the top, automatic paging resumes after a manual pause, and Ready eventually hides the reply. | [Pinned core settings and reply lifecycle](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/firmware/core/src/app.cpp#L16-L39), [reply handlers](../platform/hermes_core/src/app.cpp#L462), [recording](../platform/hermes_core/src/app.cpp#L775), [paging](../platform/hermes_core/src/app.cpp#L902), [Ready view](../platform/hermes_core/src/app.cpp#L1406) |
| The board already advertises scroll controls and maps knob detents to SDK Up/Down events. It has an action menu and saved volume control. | [Device profile](../main/targets/esp32s3/t_embed_hermes.cpp#L451), [menu and scroll routing](../main/targets/esp32s3/t_embed_hermes.cpp#L750) |
| The shared menu renderer caps menus at six rows. The online Hermes menu already uses six. | [Menu renderer](../main/targets/esp32s3/t_embed_ui.c#L231), [Hermes menu](../main/targets/esp32s3/t_embed_hermes.cpp#L875) |
| Omitting `caps.speaker` in the handshake is the documented way to prevent server audio. The plugin also honors its chat-level auto-TTS disable state (`/voice off`). Neither is an existing device-local mute switch. | [SDK capability negotiation](https://github.com/Adolanium/hermes-gadget-sdk/blob/c3df35f82746d1f711ee1ee885d46552210dac2a/docs/protocol.md#hello-device--server), [plugin TTS decision](https://github.com/Adolanium/hermes-gadget-sdk/blob/c3df35f82746d1f711ee1ee885d46552210dac2a/plugin/adapter.py#L582-L590) |

The protocol does not document a live `mute` or `response_modality` message. This design adds no such wire message. Changing gateway chat state would add command handling and synchronization; changing handshake capabilities would require reconnect behavior. Local playback control directly serves the requested speaker toggle.

## User behavior

### Voice control

Replace the online action menu's **Volume** entry with **Reply Audio**. Its three entries are **Voice replies: On/Off**, **Volume**, and **Back**. Opening Reply Audio selects the toggle; clicking it changes the setting and returns to the main Hermes screen. Volume opens the existing volume editor, whose save returns to Reply Audio. Back returns to the action menu. Keep Back as the action menu's default selection.

Also offer Reply Audio in the offline/pairing action menu so silence can be selected before connecting. This uses four rows and needs no shared menu-layout change. Approval prompts retain their existing dedicated controls.

| Action | Proposed result |
| --- | --- |
| Turn voice replies off | Stop current playback and discard queued speech. Continue the current answer and display its text. Future incoming speech stays silent. |
| Turn voice replies on | Allow subsequent audio streams at the saved volume. Do not replay discarded audio. If the gateway starts another audio segment during the current answer, that segment may be audible. |
| Hold knob about 300 ms, then release | Existing Listening → record → send flow, in either output mode. |
| Change volume while voice replies are off | Save the volume for later; remain silent. |
| Restart or reopen Hermes | Restore the saved voice-reply setting before handling network audio. |

Use a **TEXT** prefix in the existing title bar while voice replies are off, including while Listening and Thinking. Keep the existing connection and activity information. No audible confirmation is needed.

For compatibility, an absent setting defaults to On. Store Off/On independently of volume. An unreadable or invalid stored preference selects Off with a visible configuration error. If saving fails, keep the requested setting for the running session and display **SETTING NOT SAVED**; do not imply it will survive restart. These defaults and error behaviors are proposed product decisions.

### Reading and scrolling

Apply the same reading behavior with voice replies on or off:

1. Show the streamed reply using the existing text renderer. Initially follow the newest text.
2. Counterclockwise scrolls toward earlier lines; clockwise scrolls toward later lines, one line per detent, clamped at either end. The first deliberate scroll takes manual control.
3. Subsequent streaming previews replace the cumulative text without moving a manually selected position. Clamp the position if replacement text becomes shorter. Do not re-enable following merely because the reader reaches the bottom.
4. When final text arrives, start at the top if the reader has not scrolled; otherwise preserve the selected position. Disable automatic reply paging on this board.
5. Keep the completed reply visible while idle. Opening and closing a local menu preserves the reading position; no timer returns it to the mascot.
6. Starting the next recording or New Conversation replaces/clears the current reply, consistent with the existing single-reply model. Reboot does not restore reply text. Prompts, errors, and connection screens can still take priority.

Rotation during recording keeps its existing discard-recording behavior; it does not scroll. Use a short reply-screen hint such as **TURN READ / HOLD TALK**. This is a plain-text reader using existing wrapping and fonts, with no search, chat-history storage, or new rendering framework.

## Minimum implementation

All new identifiers below are proposals, not claims about existing SDK support.

| Area | Required change |
| --- | --- |
| `main/targets/esp32s3/t_embed_hermes.cpp` | Add Reply Audio menu routing and persist one `voice_reply` key (`on`/`off`) in the existing Hermes storage. Load and apply it before `app.begin()`/event processing. Check storage errors through the board adapter. Preserve the existing microphone, prompt, and cancel controls. |
| `platform/hermes_core/include/hg/app.hpp`, `src/app.cpp` | Add one runtime playback-enabled flag, setter, and getter. Off calls existing `stop_playback()`, which resets the active incoming audio stream. Ignore new output `audio.start` and audio payloads while disabled. Gate only output audio; continue processing transcript, reply, turn, prompt, and microphone events. Derive the TEXT indicator from that flag. |
| Same core files, reply scrolling only | Use existing `profile_.has_scroll_buttons` to select manual reply reading. Preserve explicit scroll positions across previews/final text, skip automatic reply paging, and retain the idle reply. Keep existing behavior for profiles without scroll controls and for card/prompt handling. No second copy of the reply is needed. |
| `platform/hermes_core/PROVENANCE.md` | Record the local changes to the imported core. |
| Focused tests and `tools/README-t-embed-hermes.md` | Cover the new behavior and document controls plus the current-reply-only limit. |

Keep `Hal::speaker` and `caps.speaker` tied to hardware availability. The runtime switch controls local playback, so toggling needs no reconnect, re-pairing, turn cancellation, or gateway change. Speaker-volume actions cannot bypass this output gate. On permits playback; actual speech still depends on the host's TTS configuration and working speaker hardware.

Reuse the existing board audio abort path first. It closes the software stream and requests an asynchronous queue reset/I2S restart in the speaker task; source inspection alone does **not** prove the physical output becomes silent without residual DMA audio. Measure this, and change the adapter only if needed to meet the requirement. [Board abort implementation](../main/targets/esp32s3/t_embed_hermes_audio.cpp#L322), [speaker worker](../main/targets/esp32s3/t_embed_hermes_audio.cpp#L129).

Retain the existing transport limits; its 16 KiB ceiling is for an entire incoming message, not a promised reply-text capacity. This feature does not claim unlimited answers, lower TTS cost, less network traffic, or offline transcription. [Transport limit](../main/targets/esp32s3/t_embed_hermes_transport.h#L27).

## Implementation plan and acceptance checks

1. **Playback preference:** add focused host tests against this repository's modified core, then implement the flag and board menu/storage. Off must block speaker begin/write calls while microphone samples and text still flow. Toggle during playback must call abort and clear stream tracking without sending `cancel`. Feed late packets and another `audio.start` while Off; both remain silent. Turning On must not replay the discarded stream. Verify volume changes cannot unmute, default/loading behavior, save failure feedback, and reconnect persistence.
2. **Manual reader:** implement against a fake 320 × 170 display. Verify scrolling both directions and both bounds, cumulative-preview replacement without duplicated text, stable position during updates/finalization, and a reply still visible at the same position after 60 seconds idle. Verify local menu return, next recording, and New Conversation. Keep prompt expiry/approval behavior and non-scroll profile paging covered.
3. **Integration:** run `python3 -m unittest discover -s tests`, `python3 validation/check_controls.py`, and the microphone bridge suite. Build T-Embed with the exact ESP-IDF pin. A test run against an untouched sibling SDK checkout does not validate the modified vendored core.
4. **Hardware:** with audible speech playing, switch Off and measure the last output sample. Proposed acceptance target: no continued speech more than 100 ms after the toggle is applied; this is a target, not an observed result. Check repeated toggles, queued audio, fresh streams while Off, saved Off after restart, and a real whispered/quietly spoken request in representative noise. Record transcription success separately from output silence. Capture the menu, TEXT indicator, and long-reply reading on the actual screen.
5. **Before distribution:** build every supported firmware profile, as required by the repository guidelines. Report hardware results separately from host/build checks.

Specification verification: checked the pinned SDK protocol, core and plugin source, and local board adapters. Implementation results are recorded separately in [validation](../validation/hermes-text-mode.md); the 100 ms physical cutoff target requires hardware measurement.
