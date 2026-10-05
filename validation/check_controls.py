#!/usr/bin/env python3
"""Check the T-Embed PCNT decoder against the pinned ESP-IDF encoder example."""

from pathlib import Path
import os
import re

ROOT = Path(__file__).resolve().parents[1]
UI = (ROOT / "main/targets/esp32s3/t_embed_ui.c").read_text()
SDK = Path(os.environ["IDF_PATH"])
IDF_ENCODER = (
    SDK / "examples/peripherals/pcnt/rotary_encoder/main/"
    "rotary_encoder_example_main.c"
).read_text()


def action_tuple(source: str, function: str, channel: str) -> tuple[str, str]:
    match = re.search(
        rf"{function}\({channel},\s*([^,]+),\s*([^\)]+)\)", source
    )
    if not match:
        raise AssertionError(f"missing {function} for {channel}")
    return tuple(item.strip() for item in match.groups())


def level_action_tuple(source: str, channel: str) -> tuple[str, str]:
    return action_tuple(source, "pcnt_channel_set_level_action", channel)


for channel in ("encoder_channel_a", "encoder_channel_b"):
    assert action_tuple(UI, "pcnt_channel_set_edge_action", channel) == action_tuple(
        IDF_ENCODER,
        "pcnt_channel_set_edge_action",
        "pcnt_chan_a" if channel == "encoder_channel_a" else "pcnt_chan_b",
    )
    assert level_action_tuple(UI, channel) == level_action_tuple(
        IDF_ENCODER,
        "pcnt_chan_a" if channel == "encoder_channel_a" else "pcnt_chan_b",
    )


def pcnt_delta(old: int, new: int) -> int:
    """Apply Espressif's edge/level actions to one GPIO state transition."""
    old_a, old_b = (old >> 1) & 1, old & 1
    new_a, new_b = (new >> 1) & 1, new & 1
    delta = 0
    if old_a != new_a:
        delta = -1 if new_a else 1  # A posedge=decrease, negedge=increase
        if new_b == 0:
            delta = -delta  # B low inverts A channel
    if old_b != new_b:
        edge_delta = 1 if new_b else -1  # B posedge=increase, negedge=decrease
        if new_a == 0:
            edge_delta = -edge_delta  # A low inverts B channel
        delta += edge_delta
    return delta


def traverse(states: tuple[int, ...]) -> int:
    return sum(pcnt_delta(a, b) for a, b in zip(states, states[1:]))


# This follows the existing table's bit order (A as the high bit, B as low).
cw = (0, 1, 3, 2, 0)
ccw = tuple(reversed(cw))
assert traverse(cw) == -4, traverse(cw)
assert traverse(ccw) == 4, traverse(ccw)

# LILYGO's TWO03 mode reports steps when the signal reaches states 0 and 3;
# either half-cycle between those detents spans two PCNT edges.
assert traverse((0, 1, 3)) == -2
assert traverse((3, 2, 0)) == -2
assert traverse((0, 2, 3)) == 2
assert traverse((3, 1, 0)) == 2

# A one-phase contact bounce out and back has zero net PCNT movement.
assert traverse((0, 1, 0)) == 0
assert traverse((3, 2, 3)) == 0

# The UI must consume one tune step at ±2 edges, matching TWO03, and clamp.
assert re.search(r"while \(encoder_edges >= 2\)", UI)
assert re.search(r"while \(encoder_edges <= -2\)", UI)
assert "if (*frequency_mhz < 2483u)" in UI
assert "if (*frequency_mhz > 2400u)" in UI

# Both keys are falling-edge latched so a full tap during masked capture is
# retained until the receiver loop drains the event counters.
assert "gpio_set_intr_type(PIN_ENCODER_KEY, GPIO_INTR_NEGEDGE)" in UI
assert "gpio_set_intr_type(PIN_USER_KEY, GPIO_INTR_NEGEDGE)" in UI
assert "center_events = encoder_press_events" in UI
assert "span_events = user_press_events" in UI
assert "if (span_events && now - last_user_tap_us >= 30000)" in UI
assert "cycle_span();" in UI
assert "encoder_press_unsettled &&" in UI
assert "cycle_step();" in UI

print("PCNT actions match pinned ESP-IDF; CW/CCW, TWO03 half-cycle, bounce, and latched button paths pass.")
