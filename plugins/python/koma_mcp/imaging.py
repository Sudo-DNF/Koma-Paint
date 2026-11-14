# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pixel decoding, coverage statistics and PNG replies."""

import base64
import struct

from PyQt6.QtCore import QBuffer, QByteArray, QIODevice, Qt
from PyQt6.QtGui import QColor, QImage

# Channel layout of Krita's colour spaces as stored in paint devices:
# integer RGBA is stored BGRA, floating point RGBA is stored RGBA.
_FORMATS = {
    ("RGBA", "U8"): ("<4B", (2, 1, 0, 3), 255.0),
    ("RGBA", "U16"): ("<4H", (2, 1, 0, 3), 65535.0),
    ("RGBA", "F16"): ("<4e", (0, 1, 2, 3), 1.0),
    ("RGBA", "F32"): ("<4f", (0, 1, 2, 3), 1.0),
    ("GRAYA", "U8"): ("<2B", (0, 0, 0, 1), 255.0),
    ("GRAYA", "U16"): ("<2H", (0, 0, 0, 1), 65535.0),
    ("GRAYA", "F16"): ("<2e", (0, 0, 0, 1), 1.0),
    ("GRAYA", "F32"): ("<2f", (0, 0, 0, 1), 1.0),
}


def decode_pixel(data, color_model, color_depth):
    layout = _FORMATS.get((color_model, color_depth))
    if layout is None or len(data) < struct.calcsize(layout[0]):
        return {"raw": data.hex(), "color_model": color_model, "color_depth": color_depth}
    pattern, order, scale = layout
    channels = struct.unpack_from(pattern, data)
    rgba = [channels[index] / scale for index in order]
    clamped = [max(0, min(255, round(value * 255.0))) for value in rgba]
    return {"rgba": [round(value, 5) for value in rgba], "rgba8": clamped,
            "hex": QColor(*clamped).name(QColor.NameFormat.HexArgb)}


def node_region_image(node, x, y, width, height):
    """The node's composited pixels in a region as a non-premultiplied 8-bit QImage."""
    data = bytes(node.projectionPixelData(x, y, width, height))
    color_model, color_depth = node.colorModel(), node.colorDepth()
    if (color_model, color_depth) == ("RGBA", "U8"):
        # BGRA bytes are QImage's little-endian ARGB32 layout.
        return QImage(data, width, height, width * 4, QImage.Format.Format_ARGB32).copy()
    layout = _FORMATS.get((color_model, color_depth))
    if layout is None:
        raise ValueError("cannot decode %s/%s pixels" % (color_model, color_depth))
    pattern, order, scale = layout
    stride = struct.calcsize(pattern)
    image = QImage(width, height, QImage.Format.Format_ARGB32)
    for index, channels in enumerate(struct.iter_unpack(pattern, data[:width * height * stride])):
        rgba = [max(0, min(255, round(channels[channel] / scale * 255.0))) for channel in order]
        image.setPixelColor(index % width, index // width, QColor(*rgba))
    return image


def qimage_stats(image, origin_x=0, origin_y=0, max_samples=262144):
    """Coverage and mean colour of a QImage; bounds are reported in image coordinates."""
    image = image.convertToFormat(QImage.Format.Format_ARGB32)
    width, height = image.width(), image.height()
    total = width * height
    if total == 0:
        return {"pixels": 0, "non_transparent": 0, "coverage": 0.0, "bounds": None, "mean_rgba8": None}
    pointer = image.constBits()
    pointer.setsize(image.sizeInBytes())
    data = bytes(pointer)
    row_bytes = image.bytesPerLine()
    alpha = b"".join(data[row * row_bytes + 3:row * row_bytes + width * 4:4] for row in range(height))
    opaque = total - alpha.count(0)

    bounds = None
    if opaque:
        top = bottom = None
        left, right = width, -1
        for row in range(height):
            line = alpha[row * width:(row + 1) * width]
            stripped = line.lstrip(b"\0")
            if not stripped:
                continue
            top = row if top is None else top
            bottom = row
            left = min(left, width - len(stripped))
            right = max(right, len(line.rstrip(b"\0")) - 1)
        bounds = [origin_x + left, origin_y + top, right - left + 1, bottom - top + 1]

    mean = None
    if opaque:
        step = max(1, total // max_samples)
        sums = [0, 0, 0, 0]
        weight = 0
        for index in range(0, total, step):
            row, column = divmod(index, width)
            offset = row * row_bytes + column * 4
            a = data[offset + 3]
            if a:
                sums[0] += data[offset + 2] * a
                sums[1] += data[offset + 1] * a
                sums[2] += data[offset] * a
                sums[3] += a
                weight += 1
        if weight:
            mean = [round(sums[0] / sums[3]), round(sums[1] / sums[3]), round(sums[2] / sums[3]),
                    round(sums[3] / weight)]

    return {"pixels": total, "non_transparent": opaque, "coverage": round(opaque / total, 6),
            "bounds": bounds, "mean_rgba8": mean}


def encode_png(image):
    array = QByteArray()
    buffer = QBuffer(array)
    buffer.open(QIODevice.OpenModeFlag.WriteOnly)
    image.save(buffer, "PNG")
    buffer.close()
    return base64.b64encode(bytes(array)).decode("ascii")


def image_reply(image, max_size, path=None, extra=None):
    """A reply carrying the image as PNG; the image is scaled down to max_size on its long edge."""
    reply = dict(extra or {})
    reply["width"], reply["height"] = image.width(), image.height()
    if path:
        if not image.save(path):
            raise ValueError("cannot write '%s'" % path)
        reply["saved"] = path
    if max_size > 0 and max(image.width(), image.height()) > max_size:
        image = image.scaled(max_size, max_size, Qt.AspectRatioMode.KeepAspectRatio,
                             Qt.TransformationMode.SmoothTransformation)
        reply["scaled_to"] = [image.width(), image.height()]
    reply["_images"] = [{"mime": "image/png", "data": encode_png(image)}]
    return reply
