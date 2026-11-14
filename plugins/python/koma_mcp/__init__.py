# SPDX-FileCopyrightText: 2026 Koma Paint contributors
# SPDX-License-Identifier: GPL-3.0-or-later

from krita import Krita

from .extension import KomaMcpExtension

Krita.instance().addExtension(KomaMcpExtension(Krita.instance()))
