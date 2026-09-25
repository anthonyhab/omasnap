/** @fileoverview Tests line snapping: the line map, its adaptive pull and
 *  motion rules, fit/step, and the editor's drag and keyboard paths. */
#include "line-snap-smoke.hpp"

#include "editor.hpp"
#include "line-snap.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QTest>

namespace {
// A light desktop, a slightly lighter panel inside it, a dark card inside
// that, a 1 px outlined box, and a short stray stroke that must never count
// as a line.
const QRect kPanel(60, 40, 680, 520);
const QRect kCard(200, 150, 300, 230);
const QRect kOutline(560, 420, 141, 101); // outer edges 560..700, 420..520

QImage nestedFrame() {
  QImage image(800, 600, QImage::Format_RGB32);
  image.fill(QColor(216, 220, 227));
  QPainter painter(&image);
  painter.fillRect(kPanel, QColor(244, 245, 247));
  painter.fillRect(kCard, QColor(59, 66, 82));
  painter.setPen(QPen(Qt::black, 1));
  painter.drawRect(kOutline.adjusted(0, 0, -1, -1));
  painter.fillRect(QRect(300, 100, 10, 1), Qt::black);
  return image;
}

/** Full-height 1 px rules every `spacing` pixels from x = 100. */
QImage ruledFrame(int spacing) {
  QImage image(800, 600, QImage::Format_RGB32);
  image.fill(Qt::white);
  QPainter painter(&image);
  for (int x = 100; x < 700; x += spacing)
    painter.fillRect(QRect(x, 0, 1, 600), Qt::black);
  return image;
}

QString describe(const QRectF &rect) {
  return QStringLiteral("%1,%2 %3x%4")
      .arg(rect.x())
      .arg(rect.y())
      .arg(rect.width())
      .arg(rect.height());
}

bool expectRect(const QRectF &actual, const QRectF &wanted,
                const QString &what, QString &error) {
  const qreal slop = 0.01;
  if (std::abs(actual.left() - wanted.left()) > slop ||
      std::abs(actual.top() - wanted.top()) > slop ||
      std::abs(actual.right() - wanted.right()) > slop ||
      std::abs(actual.bottom() - wanted.bottom()) > slop) {
    error = QStringLiteral("%1: got %2, expected %3")
                .arg(what, describe(actual), describe(wanted));
    return false;
  }
  return true;
}

/** Where a lone left edge lands at `x`, over the card's rows. */
qreal leftEdge(const LineMap &map, qreal x, qreal velocity = 0.0,
               std::optional<qreal> held = std::nullopt) {
  linesnap::Held memory{};
  memory[0] = held;
  return linesnap::snapRect(map, QRectF(QPointF(x, 160), QPointF(480, 370)),
                            linesnap::Left, linesnap::Left,
                            QPointF(velocity, 0), memory, 1.0, 16)
      .rect.left();
}
} // namespace

bool runLineSnapSmoke(QString &error) {
  const auto map = LineMap::build(nestedFrame(), 1.0);
  if (!map || map->width() != 800 || map->height() != 600) {
    error = QStringLiteral("line map did not build for an 800x600 frame");
    return false;
  }
  if (LineMap::build(QImage(1, 1, QImage::Format_RGB32), 1.0)) {
    error = QStringLiteral("a 1x1 frame built a line map");
    return false;
  }

  const QVector<LineMap::Line> cardSide =
      map->lines(Qt::Vertical, 190, 210, 160, 370);
  if (cardSide.size() != 1 || cardSide.first().low != 200 ||
      cardSide.first().high != 200) {
    error = QStringLiteral("the card's left side is not one line at 200");
    return false;
  }
  const QVector<LineMap::Line> border =
      map->lines(Qt::Vertical, 555, 565, 430, 510);
  if (border.size() != 1 || border.first().outer(true) != 560 ||
      border.first().outer(false) != 561) {
    error = QStringLiteral("a 1 px border is not one line spanning 560..561");
    return false;
  }
  if (!map->lines(Qt::Horizontal, 95, 105, 250, 450).isEmpty()) {
    error = QStringLiteral("a ten pixel stroke became a snap target");
    return false;
  }

  // Rows of real monospace text, the way a terminal draws them, never read
  // as lines: glyph tops and baselines line up but are not smooth along.
  {
    QImage terminal(900, 200, QImage::Format_RGB32);
    terminal.fill(QColor(22, 24, 30));
    QPainter painter(&terminal);
    painter.setPen(QColor(210, 214, 222));
    QFont font(QStringLiteral("JetBrains Mono"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(18);
    painter.setFont(font);
    for (int row = 0; row < 6; ++row)
      painter.drawText(QPointF(10, 30 + row * 28),
                       QStringLiteral("names umami minimum xenon vanessa "
                                      "summon nominee cumin anemone"));
    painter.end();
    const auto text = LineMap::build(terminal, 1.0);
    if (!text->lines(Qt::Horizontal, 0, 200, 10, 880).isEmpty()) {
      error = QStringLiteral("rows of text were read as horizontal lines");
      return false;
    }
  }

  // A lone line pulls from far: its nearest neighbour (the panel, 140 px
  // away) is past the cap, so its pull is the full 0.45 * 64.
  if (leftEdge(*map, 225) != 200 || leftEdge(*map, 232) != 232) {
    error = QStringLiteral("a lone line's pull is not about 29 px");
    return false;
  }
  // Heading toward it reaches further; a flick glides past.
  if (leftEdge(*map, 232, -0.5) != 200) {
    error = QStringLiteral("moving toward a line did not reach it early");
    return false;
  }
  if (leftEdge(*map, 215, -5.0) != 215) {
    error = QStringLiteral("a fast flick was caught by a line");
    return false;
  }
  // Held, it survives a pause; pulled off, it lets go at once.
  if (leftEdge(*map, 236, 0.0, 200.0) != 200) {
    error = QStringLiteral("a held line let go without being pulled");
    return false;
  }
  if (leftEdge(*map, 208, 1.0, 200.0) != 208) {
    error = QStringLiteral("a held line did not release when pulled off");
    return false;
  }

  // Dense rules share the space between them: 20 px apart, a pull of 9.
  const auto rules = LineMap::build(ruledFrame(20), 1.0);
  const auto ruled = [&](qreal x) {
    return linesnap::snapRect(*rules, QRectF(QPointF(x, 100), QPointF(700, 500)),
                              linesnap::Left, linesnap::Left, {}, {}, 1.0, 16)
        .rect.left();
  };
  if (ruled(126) != 120 || ruled(130) != 130) {
    error = QStringLiteral("dense rules pulled from past half their spacing");
    return false;
  }

  // Edges keep a 1 px border inside the selection on both sides.
  const linesnap::Snapped outline = linesnap::snapRect(
      *map, QRectF(QPointF(563, 423), QPointF(698, 517)), linesnap::All,
      linesnap::All, {}, {}, 1.0, 16);
  if (!expectRect(outline.rect, QRectF(QPointF(560, 420), QPointF(701, 521)),
                  QStringLiteral("outline snap"), error))
    return false;

  const linesnap::Snapped fitted = linesnap::fitRect(
      *map, QRectF(QPointF(182, 138), QPointF(518, 396)), 16);
  if (!expectRect(fitted.rect, kCard, QStringLiteral("fit"), error))
    return false;
  const linesnap::Snapped grown = linesnap::stepAxis(
      *map, kCard, Qt::Horizontal, true, 800, 24, 16);
  if (!expectRect(grown.rect, QRectF(QPointF(60, 150), QPointF(740, 380)),
                  QStringLiteral("grow to the enclosing panel"), error))
    return false;
  // Nothing encloses the panel but the frame's own border; the fixed step
  // takes over.
  const linesnap::Snapped fallback = linesnap::stepAxis(
      *map, grown.rect, Qt::Horizontal, true, 800, 24, 16);
  if (!expectRect(fallback.rect, QRectF(QPointF(36, 150), QPointF(764, 380)),
                  QStringLiteral("fallback step"), error))
    return false;
  const linesnap::Snapped shrunk = linesnap::stepAxis(
      *map, kPanel, Qt::Horizontal, false, 800, 24, 16);
  if (!expectRect(shrunk.rect, QRectF(QPointF(200, 40), QPointF(500, 560)),
                  QStringLiteral("shrink to the card"), error))
    return false;
  return true;
}

bool runLineSnapEditorSmoke(QApplication &application, QString &error) {
  Q_UNUSED(application);
  // A 2x monitor: 800x600 native pixels shown at 400x300, so every check
  // also proves the logical/native mapping.
  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 400, 300);
  capture.monitor.pixelSize = QSize(800, 600);
  capture.monitor.scale = 2.0;
  capture.source = nestedFrame();
  capture.previewSize = QSize(400, 300);
  const QRectF card(100, 75, 150, 115);
  const QRectF panel(30, 20, 340, 260);

  const auto open = [&](CaptureEditor &editor) {
    editor.setSuppressSnapshots(true);
    editor.resize(400, 300);
    editor.show();
    QElapsedTimer clock;
    clock.start();
    while (!editor.lineMapReadyForTest() && clock.elapsed() < 5000)
      QTest::qWait(5);
    return editor.lineMapReadyForTest();
  };

  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
    if (!open(editor)) {
      error = QStringLiteral("line map never became ready");
      return false;
    }
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {102, 77});
    QTest::mouseMove(&editor, {244, 184}, 20);
    if (!expectRect(editor.currentSelection(), card,
                    QStringLiteral("live drag"), error))
      return false;
    if (editor.snapGuideXsForTest().size() != 2 ||
        editor.snapGuideYsForTest().size() != 2) {
      error = QStringLiteral("live drag drew no guides for its snapped edges");
      return false;
    }
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, {244, 184});
    if (!editor.editingForTest() ||
        !expectRect(editor.currentSelection(), card,
                    QStringLiteral("released drag"), error))
      return false;
    if (!editor.snapGuideXsForTest().isEmpty()) {
      error = QStringLiteral("guides outlived the drag");
      return false;
    }
    editor.close();
  }

  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
    open(editor);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {102, 77});
    QTest::mouseMove(&editor, {248, 188}, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier, {248, 188});
    if (!expectRect(editor.currentSelection(), QRectF(102, 77, 146, 111),
                    QStringLiteral("Alt release"), error))
      return false;
    editor.close();
  }

  {
    // Space carries the selection: it keeps its size, and leaves the card
    // edges it was holding instead of dragging them along.
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
    open(editor);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {102, 77});
    QTest::mouseMove(&editor, {248, 188}, 20);
    QTest::keyPress(&editor, Qt::Key_Space);
    QTest::mouseMove(&editor, {288, 188}, 20);
    const QRectF moved = editor.currentSelection();
    QTest::keyRelease(&editor, Qt::Key_Space);
    if (std::abs(moved.width() - 146) > 0.01 ||
        std::abs(moved.left() - 142) > 0.01) {
      error = QStringLiteral("Space move gave %1").arg(describe(moved));
      return false;
    }
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier, {288, 188});
    editor.close();
  }

  {
    // In place: the crop stays where it was drawn, over the frozen frame,
    // with the toolbar below it; the centred layout is only a fallback.
    CaptureEditor::setInPlaceEditingForTest(true);
    CaptureData live = capture;
    live.monitor.name = QStringLiteral("TEST");
    CaptureEditor editor(live, CaptureEditor::CaptureMode::Region);
    open(editor);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {102, 77});
    QTest::mouseMove(&editor, {244, 184}, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, {244, 184});
    const QRectF shown = editor.editImageRectForTest();
    CaptureEditor::setInPlaceEditingForTest(false);
    if (!expectRect(shown, card, QStringLiteral("in-place edit"), error))
      return false;
    editor.close();
  }

  {
    // Adjusting after the fact: a crop edge dragged near a window edge
    // lands on it even where the pixels show no line at all (a translucent
    // window over a blurred wallpaper), grabbed anywhere along the edge.
    CaptureEditor::setInPlaceEditingForTest(true);
    CaptureData flat = capture;
    flat.monitor.name = QStringLiteral("TEST");
    flat.source.fill(QColor(40, 44, 52));
    flat.windows = {{QRect(60, 40, 200, 150), QStringLiteral("1"),
                     QStringLiteral("terminal"), QStringLiteral("kitty")}};
    CaptureEditor editor(flat, CaptureEditor::CaptureMode::Region);
    open(editor);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {70, 50});
    QTest::mouseMove(&editor, {250, 170}, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier, {250, 170});
    const QRectF drawn = editor.currentSelection();
    // Grab the right edge 3 px outside it, well above its square handle.
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {253, 70});
    QTest::mouseMove(&editor, {257, 70}, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, {257, 70});
    const QRectF snapped = editor.currentSelection();
    // The bottom edge dragged far from any window edge stays where it is put.
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {150, 173});
    QTest::mouseMove(&editor, {150, 233}, 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, {150, 233});
    const QRectF free = editor.currentSelection();
    CaptureEditor::setInPlaceEditingForTest(false);
    if (!expectRect(drawn, QRectF(70, 50, 180, 120),
                    QStringLiteral("unsnapped frame"), error) ||
        !expectRect(snapped, QRectF(QPointF(70, 50), QPointF(260, 170)),
                    QStringLiteral("crop edge to window edge"), error) ||
        !expectRect(free, QRectF(QPointF(70, 50), QPointF(260, 230)),
                    QStringLiteral("crop edge away from windows"), error))
      return false;
    editor.close();
  }

  // A rough frame with padding around the card.
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
  open(editor);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, {90, 66});
  QTest::mouseMove(&editor, {262, 202}, 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::AltModifier, {262, 202});
  if (!expectRect(editor.currentSelection(), QRectF(90, 66, 172, 136),
                  QStringLiteral("rough frame"), error))
    return false;
  QTest::keyClick(&editor, Qt::Key_F, Qt::AltModifier);
  if (!expectRect(editor.currentSelection(), card, QStringLiteral("Alt+F"),
                  error))
    return false;
  QTest::keyClick(&editor, Qt::Key_Equal, Qt::AltModifier);
  if (!expectRect(editor.currentSelection(),
                  QRectF(QPointF(panel.left(), card.top()),
                         QPointF(panel.right(), card.bottom())),
                  QStringLiteral("Alt+="), error))
    return false;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  if (!expectRect(editor.currentSelection(), card,
                  QStringLiteral("undo after Alt+="), error))
    return false;
  QTest::keyClick(&editor, Qt::Key_Plus,
                  Qt::AltModifier | Qt::ShiftModifier);
  if (!expectRect(editor.currentSelection(),
                  QRectF(QPointF(card.left(), panel.top()),
                         QPointF(card.right(), panel.bottom())),
                  QStringLiteral("Alt+Shift+="), error))
    return false;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  // One native pixel per side is half a logical pixel on this monitor.
  QTest::keyClick(&editor, Qt::Key_Equal,
                  Qt::AltModifier | Qt::ControlModifier);
  if (!expectRect(editor.currentSelection(), card.adjusted(-0.5, 0, 0.5, 0),
                  QStringLiteral("Alt+Ctrl+="), error))
    return false;
  QTest::keyClick(&editor, Qt::Key_Minus,
                  Qt::AltModifier | Qt::ControlModifier);
  if (!expectRect(editor.currentSelection(), card,
                  QStringLiteral("Alt+Ctrl+-"), error))
    return false;
  // Bare = still zooms; the crop keys never steal it.
  QTest::keyClick(&editor, Qt::Key_Equal);
  if (!expectRect(editor.currentSelection(), card,
                  QStringLiteral("bare = changed the crop"), error))
    return false;
  editor.close();
  return true;
}
