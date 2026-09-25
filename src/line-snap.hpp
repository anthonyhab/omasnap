#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QtCore/qtypes.h>

/**
 * The straight lines in a captured frame: window borders, card outlines,
 * table rules, panel edges. Built once per source image, off the UI thread.
 *
 * Boundary `b` sits between pixel columns (or rows) `b - 1` and `b`. Each
 * boundary keeps the runs along it where the colour steps sharply; runs
 * shorter than a glyph stroke are dropped at build time, so text never
 * becomes a snap target. Neighbouring boundaries a few pixels apart are one
 * line (a 1 px border has a boundary on each side), reported as `low` and
 * `high` so a selection edge can take whichever side keeps the border inside.
 *
 * All coordinates are native source-image pixels.
 */
class LineMap {
public:
  struct Segment {
    int first = 0; // inclusive pixel range along the boundary
    int last = 0;
  };
  struct Line {
    int low = 0;
    int high = 0;
    /** Placed by the compositor (a window or bar edge), not read from pixels. */
    bool anchored = false;
    /** The side a selection edge takes to keep the line inside it. */
    [[nodiscard]] int outer(bool lowEdge) const { return lowEdge ? low : high; }
  };

  /**
   * `anchors` are compositor rectangles (windows, bars) in native pixels.
   * Their edges are exact even where the pixels show no line, such as a
   * translucent window over a blurred wallpaper, and outrank what the image
   * suggests.
   */
  [[nodiscard]] static std::shared_ptr<const LineMap>
  build(const QImage &source, qreal pixelsPerLogical,
        const QVector<QRectF> &anchors = {});

  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }
  [[nodiscard]] qreal pixelsPerLogical() const { return pixelsPerLogical_; }

  /**
   * Lines of one orientation between boundaries `from` and `to` that run
   * along the pixel span [spanFirst, spanLast]. `Qt::Vertical` lines have a
   * constant x and bound left/right edges; `Qt::Horizontal` ones bound
   * top/bottom. Sorted by position.
   */
  [[nodiscard]] QVector<Line> lines(Qt::Orientation orientation, int from,
                                    int to, int spanFirst, int spanLast) const;

  /** Pixels of boundary `b` covered by runs inside [first, last]. */
  [[nodiscard]] int coverage(Qt::Orientation orientation, int b, int first,
                             int last) const;

private:
  LineMap() = default;
  [[nodiscard]] bool runsAlong(Qt::Orientation orientation, int b,
                               int spanFirst, int spanLast) const;
  [[nodiscard]] bool anchoredAlong(Qt::Orientation orientation, int b,
                                   int spanFirst, int spanLast) const;

  int width_ = 0;
  int height_ = 0;
  qreal pixelsPerLogical_ = 1.0;
  std::vector<std::vector<Segment>> vertical_;
  std::vector<std::vector<Segment>> horizontal_;
  std::vector<std::vector<Segment>> anchoredVertical_;
  std::vector<std::vector<Segment>> anchoredHorizontal_;
};

namespace linesnap {

enum Edge : unsigned { Left = 1, Top = 2, Right = 4, Bottom = 8, All = 15 };

/** Per-edge snap memory the caller keeps between pointer events. */
using Held = std::array<std::optional<qreal>, 4>; // left, top, right, bottom

struct Snapped {
  QRectF rect;
  /** Positions of the edges that snapped, for the guide lines. */
  QVector<qreal> xs;
  QVector<qreal> ys;
  Held held;
};

/**
 * Snaps the edges in `edges`. The pull around each line reaches about half
 * way to its nearest neighbouring line, from 6 to 32 screen pixels, so a lone
 * border grabs from far while dense rules stay precise. Compositor edges
 * always pull at full reach and win a tie with a line read from pixels. `velocity` is the
 * pointer's motion in native pixels per millisecond; only `moving` edges
 * follow it. Heading toward a line reaches for it early, a fast flick glides
 * past, and a held line lets go as soon as the pointer pulls away from it.
 */
[[nodiscard]] Snapped snapRect(const LineMap &map, const QRectF &rect,
                               unsigned edges, unsigned moving,
                               QPointF velocity, const Held &held,
                               qreal nativePerScreenPixel, qreal minimumSize);

/**
 * Steps both edges of one axis to the next line outward (`grow`) or inward.
 * `Qt::Horizontal` changes the width. An edge that finds no line within
 * `maxDistance` moves by `fallbackStep`.
 */
[[nodiscard]] Snapped stepAxis(const LineMap &map, const QRectF &rect,
                               Qt::Orientation axis, bool grow,
                               qreal maxDistance, qreal fallbackStep,
                               qreal minimumSize);

/**
 * Shrink-wraps a rough selection: each edge takes the first line inside it,
 * or failing that one just outside, so padding around an element resolves
 * to the element itself.
 */
[[nodiscard]] Snapped fitRect(const LineMap &map, const QRectF &rect,
                              qreal minimumSize);

} // namespace linesnap
