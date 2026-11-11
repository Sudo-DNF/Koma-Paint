/*
 *  SPDX-FileCopyrightText: 2026 Koma Paint contributors
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "KomaCorrectLineTool.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QWidget>
#include <QtMath>

#include <klocalizedstring.h>

#include <KoCanvasBase.h>
#include <KoIcon.h>
#include <KoPointerEvent.h>
#include <KoShapeController.h>
#include <KoViewConverter.h>
#include <KisViewManager.h>
#include <KomaPaintTokens.h>
#include <flake/kis_shape_layer.h>
#include <kis_canvas2.h>
#include <kis_config.h>
#include <kis_cursor.h>
#include <kis_image.h>
#include <kis_slider_spin_box.h>

namespace
{
/// Points within this many view pixels of the cursor count as under it.
constexpr qreal pointPickPixels = 8.0;

/// Width change per pointer event at full strength.
constexpr qreal widthStepAtFullStrength = 0.12;

qreal distance(const QPointF &a, const QPointF &b)
{
    const QPointF d = a - b;
    return qSqrt(d.x() * d.x() + d.y() * d.y());
}
}

KomaCorrectLineTool::KomaCorrectLineTool(KoCanvasBase *canvas, Mode mode)
    : KisTool(canvas, KisCursor::arrowCursor())
    , m_mode(mode)
{
    KisConfig cfg(true);
    const int defaultSize = mode == Mode::ControlPoint ? 8 : (mode == Mode::Connect ? 16 : 20);
    m_size = cfg.readEntry<int>(configKey("Size"), defaultSize);
    m_amount = cfg.readEntry<int>(configKey("Amount"), 30);
    m_thicken = cfg.readEntry<bool>(configKey("Thicken"), true);
    setObjectName(KomaCorrectLineToolFactory::idForMode(mode));
}

KomaCorrectLineTool::~KomaCorrectLineTool()
{
}

QString KomaCorrectLineTool::configKey(const QString &name) const
{
    return QStringLiteral("CorrectLine/%1/%2").arg(KomaCorrectLineToolFactory::idForMode(m_mode), name);
}

KisShapeLayer *KomaCorrectLineTool::shapeLayer() const
{
    return dynamic_cast<KisShapeLayer*>(currentNode().data());
}

QList<KomaLineartShape*> KomaCorrectLineTool::lines() const
{
    QList<KomaLineartShape*> result;
    if (KisShapeLayer *layer = shapeLayer()) {
        const QList<KoShape*> shapes = layer->shapes();
        for (KoShape *shape : shapes) {
            KomaLineartShape *line = dynamic_cast<KomaLineartShape*>(shape);
            if (line && line->isVisible() && line->isSelectable()) {
                result.append(line);
            }
        }
    }
    return result;
}

QList<KomaLineartShape*> KomaCorrectLineTool::linesTouching(const QPointF &documentPoint, qreal radius) const
{
    QPainterPath brush;
    brush.addEllipse(documentPoint, radius, radius);
    QList<KomaLineartShape*> result;
    for (KomaLineartShape *line : lines()) {
        if (line->absoluteTransformation().map(line->outline()).intersects(brush)) {
            result.append(line);
        }
    }
    return result;
}

qreal KomaCorrectLineTool::pixelsToDocument(qreal pixels) const
{
    const qreal pixelsPerUnit = image() ? qMax(image()->xRes(), 1e-6) : 1.0;
    return pixels / pixelsPerUnit;
}

qreal KomaCorrectLineTool::radiusInDocument() const
{
    return pixelsToDocument(m_size);
}

void KomaCorrectLineTool::requestRepaint()
{
    canvas()->updateCanvas(canvas()->canvasWidget() ? QRectF(canvas()->viewConverter()->viewToDocument(
                               QRectF(canvas()->canvasWidget()->rect()))) : QRectF());
}

void KomaCorrectLineTool::touchLines(const QPointF &documentPoint)
{
    const qreal radius = radiusInDocument();
    for (KomaLineartShape *line : linesTouching(documentPoint, radius)) {
        if (m_edits.contains(line)) {
            continue;
        }
        Edit edit;
        edit.oldCenterline = line->centerline();
        edit.oldTransformation = line->transformation();
        // Width changes need samples along the line, not just its points.
        edit.working = m_mode == Mode::AdjustWidth
                ? KomaLineartShape::densify(line->centerlineInDocument(), radius / 4.0)
                : line->centerlineInDocument();
        m_edits.insert(line, edit);
    }
}

void KomaCorrectLineTool::beginPrimaryAction(KoPointerEvent *event)
{
    if (!shapeLayer() || !nodeEditable()) {
        if (KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas())) {
            canvas2->viewManager()->showFloatingMessage(
                        i18n("Correct Line works on lineart in a vector layer."), koIcon("object-locked"));
        }
        event->ignore();
        return;
    }

    setMode(KisTool::PAINT_MODE);
    m_dragging = true;
    m_cursor = event->point;
    m_dragStart = event->point;
    m_edits.clear();
    m_pointShape = nullptr;
    m_pointIndex = -1;

    if (m_mode == Mode::ControlPoint) {
        // The nearest centerline point under the cursor.
        const qreal pickRadius = qMax(radiusInDocument(),
                                      canvas()->viewConverter()->viewToDocumentX(pointPickPixels));
        qreal best = pickRadius;
        for (KomaLineartShape *line : lines()) {
            const QVector<KomaLineartPoint> points = line->centerlineInDocument();
            for (int i = 0; i < points.size(); ++i) {
                const qreal d = distance(points[i].position, event->point);
                if (d <= best) {
                    best = d;
                    m_pointShape = line;
                    m_pointIndex = i;
                }
            }
        }
        if (!m_pointShape) {
            return;
        }
        Edit edit;
        edit.oldCenterline = m_pointShape->centerline();
        edit.oldTransformation = m_pointShape->transformation();
        edit.working = m_pointShape->centerlineInDocument();
        m_edits.insert(m_pointShape, edit);

        if (event->modifiers() & Qt::ShiftModifier) {
            Edit &e = m_edits[m_pointShape];
            if (e.working.size() > 2) {
                e.working.remove(m_pointIndex);
                m_pointShape->setCenterlineFromDocument(e.working);
                commitEdits(kundo2_i18n("Delete Control Point"));
            }
            m_pointShape = nullptr;
            m_dragging = false;
            setMode(KisTool::HOVER_MODE);
        }
        return;
    }

    if (m_mode != Mode::Connect) {
        continuePrimaryAction(event);
    }
}

void KomaCorrectLineTool::continuePrimaryAction(KoPointerEvent *event)
{
    if (!m_dragging) {
        return;
    }
    m_cursor = event->point;

    switch (m_mode) {
    case Mode::ControlPoint:
        if (m_pointShape && m_edits.contains(m_pointShape)) {
            Edit &edit = m_edits[m_pointShape];
            edit.working[m_pointIndex].position = event->point;
            m_pointShape->setCenterlineFromDocument(edit.working);
        }
        break;
    case Mode::AdjustWidth: {
        touchLines(event->point);
        const qreal step = widthStepAtFullStrength * m_amount / 100.0;
        const qreal factor = m_thicken ? 1.0 + step : 1.0 / (1.0 + step);
        for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
            it->working = KomaLineartShape::adjustWidth(it->working, event->point, radiusInDocument(), factor);
            it.key()->setCenterlineFromDocument(it->working);
        }
        break;
    }
    case Mode::Simplify:
        touchLines(event->point);
        break;
    case Mode::Connect:
        break;
    }
    requestRepaint();
}

void KomaCorrectLineTool::endPrimaryAction(KoPointerEvent *event)
{
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    setMode(KisTool::HOVER_MODE);

    switch (m_mode) {
    case Mode::ControlPoint:
        commitEdits(kundo2_i18n("Move Control Point"));
        break;
    case Mode::AdjustWidth: {
        // Drop the extra samples again once the width is set.
        const qreal tolerance = pixelsToDocument(0.35);
        for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
            it->working = KomaLineartShape::simplify(it->working, tolerance);
            it.key()->setCenterlineFromDocument(it->working);
        }
        commitEdits(m_thicken ? kundo2_i18n("Thicken Lines") : kundo2_i18n("Narrow Lines"));
        break;
    }
    case Mode::Simplify: {
        // Higher strength smooths more and keeps fewer points.
        const qreal tolerance = pixelsToDocument(0.25 + 4.0 * m_amount / 100.0);
        const qreal spacing = pixelsToDocument(2.0);
        for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
            it->working = KomaLineartShape::processRawStroke(KomaLineartShape::densify(it->working, spacing), tolerance);
            it.key()->setCenterlineFromDocument(it->working);
        }
        commitEdits(kundo2_i18n("Simplify Lines"));
        break;
    }
    case Mode::Connect:
        finishConnect(event->point);
        break;
    }
    m_edits.clear();
    m_pointShape = nullptr;
    requestRepaint();
}

void KomaCorrectLineTool::commitEdits(const KUndo2MagicString &text)
{
    if (m_edits.isEmpty()) {
        return;
    }
    KUndo2Command *command = new KUndo2Command(text);
    for (auto it = m_edits.begin(); it != m_edits.end(); ++it) {
        KomaLineartShape *line = it.key();
        if (line->centerline() == it->oldCenterline && line->transformation() == it->oldTransformation) {
            continue;
        }
        new KomaLineartEditCommand(line, it->oldCenterline, it->oldTransformation,
                                   line->centerline(), line->transformation(), text, command);
    }
    if (command->childCount() == 0) {
        delete command;
        return;
    }
    canvas()->addCommand(command);
}

void KomaCorrectLineTool::finishConnect(const QPointF &documentPoint)
{
    // The line end nearest each end of the drag.
    struct End {
        KomaLineartShape *line {nullptr};
        bool atEnd {false};
        qreal distance {0};
    };
    const qreal reach = radiusInDocument();
    auto nearestEnd = [this, reach](const QPointF &point, KomaLineartShape *exclude) {
        End best;
        best.distance = reach;
        for (KomaLineartShape *line : lines()) {
            if (line == exclude) {
                continue;
            }
            const QVector<KomaLineartPoint> points = line->centerlineInDocument();
            if (points.isEmpty()) {
                continue;
            }
            const qreal toStart = distance(points.first().position, point);
            const qreal toEnd = distance(points.last().position, point);
            if (toStart <= best.distance) {
                best = {line, false, toStart};
            }
            if (toEnd <= best.distance) {
                best = {line, true, toEnd};
            }
        }
        return best;
    };

    const End from = nearestEnd(m_dragStart, nullptr);
    if (!from.line) {
        return;
    }
    const End to = nearestEnd(documentPoint, from.line);
    if (!to.line) {
        return;
    }

    const QVector<KomaLineartPoint> joined = KomaLineartShape::connect(
                from.line->centerlineInDocument(), from.atEnd,
                to.line->centerlineInDocument(), !to.atEnd);

    KisShapeLayer *layer = shapeLayer();
    KUndo2Command *command = new KUndo2Command(kundo2_i18n("Connect Lines"));
    KomaLineartShape *connected = new KomaLineartShape();
    connected->setLineColor(from.line->lineColor());
    connected->setZIndex(qMax(from.line->zIndex(), to.line->zIndex()));
    connected->setCenterlineInDocument(joined);
    connected->setBrush(from.line->brush(),
                        from.line->fullPressureWidth() * qSqrt(qAbs(from.line->absoluteTransformation().determinant())));
    canvas()->shapeController()->removeShapes({from.line, to.line}, command);
    canvas()->shapeController()->addShape(connected, layer, command);
    canvas()->addCommand(command);
}

void KomaCorrectLineTool::mouseMoveEvent(KoPointerEvent *event)
{
    m_cursor = event->point;
    requestRepaint();
    KisTool::mouseMoveEvent(event);
}

void KomaCorrectLineTool::paint(QPainter &gc, const KoViewConverter &converter)
{
    const QTransform toView = converter.documentToView();
    const KomaPaint::Theme t = KomaPaint::themeFor(qApp->palette());

    gc.save();
    gc.setRenderHint(QPainter::Antialiasing);

    // Brush circle.
    if (m_mode != Mode::ControlPoint) {
        const qreal radius = radiusInDocument();
        QPainterPath circle;
        circle.addEllipse(m_cursor, radius, radius);
        gc.setPen(QPen(t.pulse, 1.0));
        gc.setBrush(Qt::NoBrush);
        gc.drawPath(toView.map(circle));
    }

    if (m_mode == Mode::ControlPoint || m_mode == Mode::Connect) {
        // Show the points that can be grabbed near the cursor.
        const qreal nearby = qMax(radiusInDocument() * 6.0, converter.viewToDocumentX(120.0));
        const qreal handle = 3.0;
        gc.setPen(QPen(t.text, 1.0));
        for (KomaLineartShape *line : lines()) {
            const QVector<KomaLineartPoint> points = line->centerlineInDocument();
            for (int i = 0; i < points.size(); ++i) {
                const bool isEnd = i == 0 || i == points.size() - 1;
                if (m_mode == Mode::Connect && !isEnd) {
                    continue;
                }
                if (distance(points[i].position, m_cursor) > nearby) {
                    continue;
                }
                const QPointF p = toView.map(points[i].position);
                const bool active = line == m_pointShape && i == m_pointIndex;
                gc.setBrush(active ? QBrush(t.pulse) : QBrush(t.surface));
                gc.drawRect(QRectF(p.x() - handle, p.y() - handle, 2 * handle, 2 * handle));
            }
        }
    }

    // The connection being dragged.
    if (m_mode == Mode::Connect && m_dragging) {
        gc.setPen(QPen(t.pulse, 1.5, Qt::DashLine));
        gc.drawLine(toView.map(m_dragStart), toView.map(m_cursor));
    }
    gc.restore();
}

QWidget *KomaCorrectLineTool::createOptionWidget()
{
    QWidget *widget = new QWidget();
    widget->setObjectName(toolId() + QStringLiteral(" option widget"));
    QFormLayout *layout = new QFormLayout(widget);

    KisSliderSpinBox *size = new KisSliderSpinBox(widget);
    size->setRange(1, 200);
    size->setSuffix(i18nc("Pixel unit", " px"));
    size->setValue(m_size);
    connect(size, QOverload<int>::of(&KisSliderSpinBox::valueChanged), this, [this](int value) {
        m_size = value;
        KisConfig(false).writeEntry<int>(configKey("Size"), value);
        requestRepaint();
    });
    layout->addRow(m_mode == Mode::ControlPoint ? i18n("Pick distance:")
                   : m_mode == Mode::Connect ? i18n("Reach:") : i18n("Size:"), size);

    if (m_mode == Mode::AdjustWidth || m_mode == Mode::Simplify) {
        KisSliderSpinBox *amount = new KisSliderSpinBox(widget);
        amount->setRange(1, 100);
        amount->setSuffix(QStringLiteral("%"));
        amount->setValue(m_amount);
        connect(amount, QOverload<int>::of(&KisSliderSpinBox::valueChanged), this, [this](int value) {
            m_amount = value;
            KisConfig(false).writeEntry<int>(configKey("Amount"), value);
        });
        layout->addRow(i18n("Strength:"), amount);
    }

    if (m_mode == Mode::AdjustWidth) {
        QComboBox *direction = new QComboBox(widget);
        direction->addItem(i18nc("@item:inlistbox Adjust line width", "Thicken"));
        direction->addItem(i18nc("@item:inlistbox Adjust line width", "Narrow"));
        direction->setCurrentIndex(m_thicken ? 0 : 1);
        connect(direction, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            m_thicken = index == 0;
            KisConfig(false).writeEntry<bool>(configKey("Thicken"), m_thicken);
        });
        layout->addRow(i18n("Direction:"), direction);
    }

    QLabel *hint = new QLabel(widget);
    hint->setWordWrap(true);
    hint->setForegroundRole(QPalette::PlaceholderText);
    switch (m_mode) {
    case Mode::ControlPoint:
        hint->setText(i18n("Drag a point to reshape the line. Shift+click a point to delete it."));
        break;
    case Mode::AdjustWidth:
        hint->setText(i18n("Brush over lines to thicken or narrow them."));
        break;
    case Mode::Simplify:
        hint->setText(i18n("Brush over lines to smooth them and remove extra points."));
        break;
    case Mode::Connect:
        hint->setText(i18n("Drag from the end of one line to the end of another to join them."));
        break;
    }
    layout->addRow(hint);
    return widget;
}

KomaCorrectLineToolFactory::KomaCorrectLineToolFactory(KomaCorrectLineTool::Mode mode)
    : KoToolFactoryBase(idForMode(mode))
    , m_mode(mode)
{
    setSection(ToolBoxSection::Shape);
    setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
    switch (mode) {
    case KomaCorrectLineTool::Mode::ControlPoint:
        setToolTip(i18n("Control Point"));
        setIconName(koIconNameCStr("pathpoint-curve"));
        setPriority(20);
        break;
    case KomaCorrectLineTool::Mode::AdjustWidth:
        setToolTip(i18n("Adjust Line Width"));
        setIconName(koIconNameCStr("calligraphy_increase_width"));
        setPriority(21);
        break;
    case KomaCorrectLineTool::Mode::Simplify:
        setToolTip(i18n("Simplify Line"));
        setIconName(koIconNameCStr("interpolation_bezier"));
        setPriority(22);
        break;
    case KomaCorrectLineTool::Mode::Connect:
        setToolTip(i18n("Connect Lines"));
        setIconName(koIconNameCStr("pathpoint-join"));
        setPriority(23);
        break;
    }
}

KoToolBase *KomaCorrectLineToolFactory::createTool(KoCanvasBase *canvas)
{
    return new KomaCorrectLineTool(canvas, m_mode);
}

QString KomaCorrectLineToolFactory::idForMode(KomaCorrectLineTool::Mode mode)
{
    switch (mode) {
    case KomaCorrectLineTool::Mode::ControlPoint:
        return QStringLiteral("KomaCorrectLineControlPoint");
    case KomaCorrectLineTool::Mode::AdjustWidth:
        return QStringLiteral("KomaCorrectLineWidth");
    case KomaCorrectLineTool::Mode::Simplify:
        return QStringLiteral("KomaCorrectLineSimplify");
    case KomaCorrectLineTool::Mode::Connect:
        return QStringLiteral("KomaCorrectLineConnect");
    }
    return QString();
}
