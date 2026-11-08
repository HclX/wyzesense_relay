#!/usr/bin/env python3
"""The user guide's wiring diagrams (docs/guide/, #65): a v1 contact's pad side wired to a LaunchPad or to an ESP relay.

    uv run tools/wiring_svg.py [--out docs/img]

The pads sit where they are in docs/img/contact_v1.jpg (pixel positions below, scaled down), in the photo's colours,
so a diagram and the photo read the same way. Pin assignments: docs/design/v1-contact.md (LaunchPad) and esp32/README.md
(relays). Each SVG has its own light background, so it reads on GitHub's light and dark themes alike.
"""

import argparse
import pathlib

SCALE = 0.3
# pad: (x, y) in docs/img/contact_v1.jpg, colour as marked there, label
PADS = {
    'W': ((405, 315), '#1e88e5', 'W'),
    '-': ((633, 235), '#8e1b1b', 'V− (GND)'),
    'S': ((350, 708), '#2e9e3e', 'S'),
    'C': ((503, 820), '#e53935', 'C'),
    'M': ((742, 828), '#8e44ad', 'M'),
    '+': ((1052, 760), '#f48fb1', 'V+ (3.3 V)'),
    'R': ((1298, 846), '#5c7fa8', 'R'),
    'T': ((1232, 945), '#9ccc28', 'T'),
}
BOARD = (180, 95, 1530, 1035)  # the board's outline in the photo
# The LaunchPad's jumper block between its XDS110 (debugger) and its own CC1310, in the silkscreen's order
LAUNCHPAD_BLOCK = ['GND', '5V', '3V3', 'RXD', 'TXD', 'RST', 'TMS', 'TCK', 'TDO', 'TDI', 'SWO']
# board: (title, subtitle, [(pad, pin label as printed on the board, note)])
RELAYS = {
    'esp32s3': (
        'ESP32-S3 DevKit',
        'relay image ows-relay-esp32s3-*',
        [
            ('+', '3V3', ''),
            ('-', 'GND', ''),
            ('R', 'GPIO17', 'UART1 TX'),
            ('T', 'GPIO18', 'UART1 RX'),
            ('S', 'GPIO4', 'reset'),
        ],
    ),
    'lolin32': (
        'Classic ESP32 (LOLIN32)',
        'relay image ows-relay-esp32-*',
        [
            ('+', '3V', ''),
            ('-', 'GND', ''),
            ('R', '17', 'UART1 TX'),
            ('T', '18', 'UART1 RX'),
            ('S', '4', 'reset'),
        ],
    ),
    'd1mini': (
        'ESP8266 (D1 mini / NodeMCU)',
        'relay image ows-relay-esp8266-*',
        [
            ('+', '3V3', ''),
            ('-', 'G', 'GND'),
            ('R', 'D4', 'GPIO2, UART1 TX'),
            ('T', 'D7', 'GPIO13, UART0 RX'),
            ('S', 'D2', 'GPIO4, reset'),
        ],
    ),
}
FONT = 'font-family="Helvetica, Arial, sans-serif"'
INK, FAINT, PAPER = '#1f2328', '#8c959f', '#ffffff'


def pad_xy(pad, ox, oy):
    (x, y), _, _ = PADS[pad]
    return ox + (x - BOARD[0]) * SCALE, oy + (y - BOARD[1]) * SCALE


def contact(ox, oy, used):
    """The contact board, pad side, at (ox, oy); only the pads in `used` are drawn in colour."""
    w, h = (BOARD[2] - BOARD[0]) * SCALE, (BOARD[3] - BOARD[1]) * SCALE
    s = [
        f'<rect x="{ox}" y="{oy}" width="{w:.0f}" height="{h:.0f}" rx="10" fill="#2d333b"/>',
        # the battery clip and the reed switch's glass, as landmarks
        f'<circle cx="{ox + (680 - BOARD[0]) * SCALE:.0f}" cy="{oy + (450 - BOARD[1]) * SCALE:.0f}" '
        f'r="{230 * SCALE:.0f}" fill="none" stroke="#c9a227" stroke-width="10" opacity="0.55"/>',
        f'<rect x="{ox + (430 - BOARD[0]) * SCALE:.0f}" y="{oy + h + 2:.0f}" width="{720 * SCALE:.0f}" height="12" '
        'rx="6" fill="#b6e3e0" opacity="0.8"/>',
        f'<text x="{ox + (790 - BOARD[0]) * SCALE:.0f}" y="{oy + h + 28:.0f}" text-anchor="middle" font-size="11" '
        f'fill="{FAINT}" {FONT}>reed switch (glass, under the board)</text>',
        f'<text x="{ox + w / 2:.0f}" y="{oy - 12:.0f}" text-anchor="middle" font-size="14" font-weight="bold" '
        f'fill="{INK}" {FONT}>Wyze v1 contact, pad side</text>',
        f'<text x="{ox + w - 10:.0f}" y="{oy + 22:.0f}" text-anchor="end" font-size="11" fill="#adbac7" {FONT}>'
        'antenna side →</text>',
    ]
    for pad, ((_, _), colour, label) in PADS.items():
        x, y = pad_xy(pad, ox, oy)
        on = pad in used
        s.append(
            f'<circle cx="{x:.0f}" cy="{y:.0f}" r="11" fill="#d4b483" stroke="{colour if on else "#57606a"}" '
            f'stroke-width="{4 if on else 2}"/>'
        )
        s.append(
            f'<text x="{x:.0f}" y="{y - 17:.0f}" text-anchor="middle" font-size="13" font-weight="bold" '
            f'fill="{"#ffffff" if on else "#768390"}" {FONT}>{label}</text>'
        )
    return s


def wire(x1, y1, x2, y2, colour, dashed=False):
    mx = (x1 + x2) / 2
    dash = ' stroke-dasharray="8 6"' if dashed else ''
    return (
        f'<path d="M{x1:.0f},{y1:.0f} C{mx:.0f},{y1:.0f} {mx:.0f},{y2:.0f} {x2:.0f},{y2:.0f}" fill="none" '
        f'stroke="{colour}" stroke-width="4" stroke-linecap="round"{dash}/>'
    )


def svg(width, height, body, title):
    return '\n'.join(
        [
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
            f'role="img" aria-label="{title}">',
            f'<title>{title}</title>',
            f'<rect width="{width}" height="{height}" rx="14" fill="{PAPER}"/>',
            *body,
            '</svg>',
            '',
        ]
    )


def launchpad():
    ox, oy = 40, 70
    wires = [('M', 'TMS', False), ('C', 'TCK', False), ('S', 'RST', False), ('+', '3V3', False), ('-', 'GND', False)]
    console = [('T', 'TXD'), ('R', 'RXD')]
    body = contact(ox, oy, {p for p, _, _ in wires} | {p for p, _ in console})
    bx, by, step = 620, 70, 34
    body += [
        f'<text x="{bx + 60}" y="{by - 34}" text-anchor="middle" font-size="14" font-weight="bold" fill="{INK}" '
        f'{FONT}>LAUNCHXL-CC1310 jumper block</text>',
        f'<text x="{bx + 60}" y="{by - 16}" text-anchor="middle" font-size="11" fill="{FAINT}" {FONT}>'
        '(turned on its side for the drawing)</text>',
        f'<rect x="{bx}" y="{by}" width="120" height="{step * len(LAUNCHPAD_BLOCK) + 8}" rx="6" fill="#f6f8fa" '
        f'stroke="{FAINT}"/>',
        f'<text x="{bx + 22}" y="{by + step * len(LAUNCHPAD_BLOCK) + 28}" text-anchor="middle" font-size="11" '
        f'fill="{INK}" {FONT}>XDS110</text>',
        f'<text x="{bx + 98}" y="{by + step * len(LAUNCHPAD_BLOCK) + 28}" text-anchor="middle" font-size="11" '
        f'fill="{FAINT}" {FONT}>its CC1310</text>',
    ]
    used = {pin: pad for pad, pin, _ in wires} | {pin: pad for pad, pin in console}
    keep = {'3V3', 'GND', '5V'}  # jumpers that may stay
    for i, pin in enumerate(LAUNCHPAD_BLOCK):
        y = by + 21 + i * step
        body.append(f'<circle cx="{bx + 22}" cy="{y}" r="6" fill="{INK}"/>')
        body.append(f'<circle cx="{bx + 98}" cy="{y}" r="6" fill="{FAINT}"/>')
        if pin in keep:
            body.append(f'<rect x="{bx + 14}" y="{y - 9}" width="92" height="18" rx="5" fill="#57606a" opacity="0.5"/>')
        body.append(f'<text x="{bx + 132}" y="{y + 5}" font-size="13" fill="{INK}" {FONT}>{pin}</text>')
        if pin not in keep:
            body.append(f'<text x="{bx + 175}" y="{y + 5}" font-size="11" fill="#cf222e" {FONT}>jumper off</text>')
        if pin in used:
            pad = used[pin]
            x, py = pad_xy(pad, ox, oy)
            body.append(wire(x, py, bx + 22, y, PADS[pad][1], dashed=pad in ('R', 'T')))
    body += [
        f'<text x="40" y="505" font-size="12" fill="{INK}" {FONT}>Solid: needed (M, C, S, V+, V−). Dashed: optional, '
        "the sensor's console (115200 8N1) on the LaunchPad's UART.</text>",
        f'<text x="40" y="525" font-size="12" fill="{INK}" {FONT}>Wires go to the XDS110 (USB) side. Pull every '
        "jumper except GND / 5V / 3V3, so the LaunchPad's own chip is off the lines. Battery out.</text>",
    ]
    return svg(900, 545, body, 'Wiring a Wyze v1 contact to a LaunchPad (LAUNCHXL-CC1310)')


def relay(key):
    title, subtitle, pins = RELAYS[key]
    ox, oy = 40, 70
    body = contact(ox, oy, {p for p, _, _ in pins})
    order = sorted(pins, key=lambda p: pad_xy(p[0], ox, oy)[1])
    bx, by, step = 640, 70, 50
    body += [
        f'<rect x="{bx}" y="{by - 40}" width="220" height="{step * len(order) + 70}" rx="10" fill="#1b4f72"/>',
        f'<text x="{bx + 110}" y="{by - 16}" text-anchor="middle" font-size="14" font-weight="bold" fill="#ffffff" '
        f'{FONT}>{title}</text>',
        f'<text x="{bx + 110}" y="{by + step * len(order) + 22}" text-anchor="middle" font-size="11" fill="#d0d7de" '
        f'{FONT}>{subtitle}</text>',
    ]
    for i, (pad, pin, note) in enumerate(order):
        y = by + 25 + i * step
        x, py = pad_xy(pad, ox, oy)
        body.append(wire(x, py, bx + 14, y, PADS[pad][1]))
        body.append(f'<circle cx="{bx + 14}" cy="{y}" r="7" fill="#d4b483" stroke="#ffffff" stroke-width="2"/>')
        body.append(
            f'<text x="{bx + 30}" y="{y + 5}" font-size="14" font-weight="bold" fill="#ffffff" {FONT}>{pin}</text>'
        )
        if note:
            body.append(f'<text x="{bx + 100}" y="{y + 5}" font-size="11" fill="#d0d7de" {FONT}>{note}</text>')
    body += [
        f'<text x="40" y="440" font-size="12" fill="{INK}" {FONT}>Five wires; pin names as printed on the ESP board. '
        "R is the contact's input (the ESP transmits), T its output.</text>",
        f'<text x="40" y="460" font-size="12" fill="{INK}" {FONT}>The ESP powers the contact from its 3.3 V pin '
        '(battery out). S lets the relay reset and reflash the contact by itself.</text>',
    ]
    return svg(900, 480, body, f'Wiring a Wyze v1 contact to an {title} relay')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=str(pathlib.Path(__file__).resolve().parents[1] / 'docs' / 'img'))
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    files = {'wiring-contact-launchpad.svg': launchpad()}
    files |= {f'wiring-contact-{k}.svg': relay(k) for k in RELAYS}
    for name, text in files.items():
        (out / name).write_text(text)
        print(out / name)


if __name__ == '__main__':
    main()
