#include "line-snap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <QtGui/qrgb.h>

namespace {
// A colour step this large on the strongest channel is part of a line.
// Coloured borders whose brightness matches the background still register.
constexpr int kStep = 16;
// Antialiasing and dashed rules break a run for a pixel or two; bridge it.
constexpr int kRunGap = 2;
// Runs shorter than this never become targets: glyph strokes, icon details.
constexpr qreal kStoredRunLogical = 10.0;
// A snap target must also be at least this long where it meets the edge.
constexpr qreal kMinimumLineLogical = 18.0;
// ...and run along this fraction of the edge, unless it is simply long.
constexpr qreal kAlongFraction = 0.3;
constexpr qreal kLongLineLogical = 160.0;
// Boundaries this close together are the two sides of one drawn line.
constexpr int kClusterGap = 3;
// Past this the segment lists cost more than the feature is worth.
constexpr qint64 kMaximumPixels = 40'000'000;

// Pull, in screen pixels.
constexpr qreal kPullMinimum = 6.0;
constexpr qreal kPullMaximum = 32.0;
// A line's pull reaches this far toward its nearest neighbour.
constexpr qreal kPullShare = 0.45;
constexpr qreal kTowardBoost = 1.25;
constexpr qreal kHeldBoost = 1.35;
// Screen pixels per millisecond: above the first a pass is a flick, above
// the second a held edge is being pulled off deliberately.
constexpr qreal kFlickSpeed = 2.5;
constexpr qreal kFlickDamping = 0.35;
constexpr qreal kReleaseSpeed = 0.25;

// Fit and step score the middle of a side, clear of rounded corners.
constexpr qreal kCentralInset = 0.15;

int colorStep(QRgb first, QRgb second) {
  return std::max({std::abs(qRed(first) - qRed(second)),
                   std::abs(qGreen(first) - qGreen(second)),
                   std::abs(qBlue(first) - qBlue(second))});
}

struct RunBuilder {
  int start = -1;
  int lastStrong = -1;
};

void extend(RunBuilder &run, std::vector<LineMap::Segment> &segments,
            int position, bool strong, int minimum) {
  if (strong) {
    if (run.start < 0)
      run.start = position;
    run.lastStrong = position;
    return;
  }
  if (run.start >= 0 && position - run.lastStrong > kRunGap) {
    if (run.lastStrong - run.start + 1 >= minimum)
      segments.push_back({run.start, run.lastStrong});
    run.start = -1;
  }
}

void finish(RunBuilder &run, std::vector<LineMap::Segment> &segments,
            int minimum) {
  if (run.start >= 0 && run.lastStrong - run.start + 1 >= minimum)
    segments.push_back({run.start, run.lastStrong});
  run.start = -1;
}
} // namespace

std::shared_ptr<const LineMap> LineMap::build(const QImage &source,
                                              qreal pixelsPerLogical) {
  const int width = source.width();
  const int height = source.height();
  if (width < 2 || height < 2 ||
      static_cast<qint64>(width) * height > kMaximumPixels)
    return nullptr;
  const QImage image =
      source.format() == QImage::Format_RGB32 ||
              source.format() == QImage::Format_ARGB32 ||
              source.format() == QImage::Format_ARGB32_Premultiplied
          ? source
          : source.convertToFormat(QImage::Format_RGB32);
  std::shared_ptr<LineMap> map(new LineMap);
  map->width_ = width;
  map->height_ = height;
  map->pixelsPerLogical_ = std::max<qreal>(pixelsPerLogical, 0.01);
  map->vertical_.resize(width + 1);
  map->horizontal_.resize(height + 1);
  const int minimum =
      std::max(2, qRound(kStoredRunLogical * map->pixelsPerLogical_));

  // Rows are walked in memory order; each vertical boundary keeps its own
  // open run across rows.
  std::vector<RunBuilder> columns(width + 1);
  const QRgb *previous = nullptr;
  for (int y = 0; y < height; ++y) {
    const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
    for (int b = 1; b < width; ++b)
      extend(columns[b], map->vertical_[b], y,
             colorStep(line[b - 1], line[b]) >= kStep, minimum);
    if (previous) {
      RunBuilder row;
      std::vector<Segment> &segments = map->horizontal_[y];
      for (int x = 0; x < width; ++x)
        extend(row, segments, x, colorStep(previous[x], line[x]) >= kStep,
               minimum);
      finish(row, segments, minimum);
    }
    previous = line;
  }
  for (int b = 1; b < width; ++b)
    finish(columns[b], map->vertical_[b], minimum);
  return map;
}

int LineMap::coverage(Qt::Orientation orientation, int b, int first,
                      int last) const {
  const auto &planes = orientation == Qt::Vertical ? vertical_ : horizontal_;
  if (b < 0 || b >= static_cast<int>(planes.size()))
    return 0;
  int covered = 0;
  for (const Segment &segment : planes[b]) {
    const int from = std::max(segment.first, first);
    const int to = std::min(segment.last, last);
    if (to >= from)
      covered += to - from + 1;
  }
  return covered;
}

bool LineMap::runsAlong(Qt::Orientation orientation, int b, int spanFirst,
                        int spanLast) const {
  const auto &planes = orientation == Qt::Vertical ? vertical_ : horizontal_;
  if (b < 1 || b >= static_cast<int>(planes.size()) - 1)
    return false;
  const qreal span = spanLast - spanFirst + 1;
  const qreal minimumLine = kMinimumLineLogical * pixelsPerLogical_;
  const qreal longLine = kLongLineLogical * pixelsPerLogical_;
  int overlap = 0;
  int longest = 0;
  for (const Segment &segment : planes[b]) {
    const int from = std::max(segment.first, spanFirst);
    const int to = std::min(segment.last, spanLast);
    if (to < from)
      continue;
    overlap += to - from + 1;
    longest = std::max(longest, segment.last - segment.first + 1);
  }
  // Judge the line by its whole length, so a card side still counts when
  // the selection only grazes it, but require that it meets the edge.
  return longest >= minimumLine &&
         (overlap >= kAlongFraction * span || overlap >= longLine);
}

QVector<LineMap::Line> LineMap::lines(Qt::Orientation orientation, int from,
                                      int to, int spanFirst,
                                      int spanLast) const {
  QVector<Line> result;
  const int extent = orientation == Qt::Vertical ? width_ : height_;
  const int crossExtent = orientation == Qt::Vertical ? height_ : width_;
  spanFirst = std::clamp(spanFirst, 0, crossExtent - 1);
  spanLast = std::clamp(spanLast, 0, crossExtent - 1);
  if (spanLast < spanFirst)
    return result;
  from = std::max(from, 1);
  to = std::min(to, extent - 1);
  for (int b = from; b <= to; ++b) {
    if (!runsAlong(orientation, b, spanFirst, spanLast))
      continue;
    if (!result.isEmpty() && b - result.last().high <= kClusterGap)
      result.last().high = b;
    else
      result.push_back({b, b});
  }
  return result;
}

namespace {
struct Span {
  int first;
  int last;
};

Span pixelSpan(qreal start, qreal end) {
  return {static_cast<int>(std::floor(std::min(start, end))),
          static_cast<int>(std::ceil(std::max(start, end))) - 1};
}

Span centralSpan(qreal start, qreal end) {
  const qreal inset = (end - start) * kCentralInset;
  return pixelSpan(start + inset, end - inset);
}

/** Where one edge lands, and whether a line caught it. */
std::optional<qreal> snapEdge(const LineMap &map, Qt::Orientation orientation,
                              qreal position, bool lowEdge, Span span,
                              qreal velocity, std::optional<qreal> held,
                              qreal nativePerScreen) {
  const qreal pullMinimum = kPullMinimum * nativePerScreen;
  const qreal pullMaximum = kPullMaximum * nativePerScreen;
  const int reach = static_cast<int>(std::ceil(pullMaximum * kHeldBoost * 2));
  const int center = qRound(position);
  const QVector<LineMap::Line> lines = map.lines(
      orientation, center - reach, center + reach, span.first, span.last);
  const qreal speed = std::abs(velocity) / std::max(nativePerScreen, 0.01);
  std::optional<qreal> best;
  qreal bestScore = std::numeric_limits<qreal>::max();
  for (qsizetype index = 0; index < lines.size(); ++index) {
    const qreal target = lines.at(index).outer(lowEdge);
    const qreal distance = std::abs(target - position);
    qreal gap = 2.0 * pullMaximum;
    if (index > 0)
      gap = std::min(gap, target - lines.at(index - 1).outer(lowEdge));
    if (index + 1 < lines.size())
      gap = std::min(gap, lines.at(index + 1).outer(lowEdge) - target);
    qreal pull = std::clamp(kPullShare * gap, pullMinimum, pullMaximum);
    const qreal toward = (target - position) * velocity;
    if (held && std::abs(*held - target) < 0.5) {
      // Moving off the line the edge sits on is intent; everything else
      // (a wobble, a pause, drifting along it) keeps it held.
      const bool leaving = (position - target) * velocity > 0.0;
      pull = leaving && speed > kReleaseSpeed ? std::min(pull, pullMinimum)
                                              : pull * kHeldBoost;
    } else {
      if (toward > 0.0)
        pull *= kTowardBoost;
      if (speed > kFlickSpeed)
        pull *= kFlickDamping;
    }
    if (distance > pull)
      continue;
    const qreal score = distance / std::max(pull, 0.01);
    if (score < bestScore) {
      bestScore = score;
      best = target;
    }
  }
  return best;
}

/** The first line past `position` in `direction`, skipping the one it is on. */
std::optional<int> nextLine(const LineMap &map, Qt::Orientation orientation,
                            qreal position, int direction, bool lowEdge,
                            Span span, qreal maxDistance) {
  const int origin = qRound(position);
  const int far = origin + direction * static_cast<int>(std::ceil(maxDistance));
  const QVector<LineMap::Line> lines =
      map.lines(orientation, std::min(origin, far), std::max(origin, far),
                span.first, span.last);
  std::optional<int> best;
  for (const LineMap::Line &line : lines) {
    // The edge sits on this line (either side of it): not a step.
    if (origin >= line.low - 1 && origin <= line.high + 1)
      continue;
    const int target = line.outer(lowEdge);
    if ((target - origin) * direction <= 0)
      continue;
    if (!best || std::abs(target - origin) < std::abs(*best - origin))
      best = target;
  }
  return best;
}
} // namespace

namespace linesnap {

Snapped snapRect(const LineMap &map, const QRectF &rect, unsigned edges,
                 unsigned moving, QPointF velocity, const Held &held,
                 qreal nativePerScreenPixel, qreal minimumSize) {
  const QRectF original = rect.normalized();
  Snapped result{original, {}, {}, {}};
  const Span rows = pixelSpan(original.top(), original.bottom());
  const Span columns = pixelSpan(original.left(), original.right());
  const auto edgeVelocity = [&](Edge edge, qreal axis) {
    return (moving & edge) ? axis : 0.0;
  };
  const auto snap = [&](Edge edge, int slot, Qt::Orientation orientation,
                        qreal position, bool lowEdge, Span span, qreal axis) {
    if (!(edges & edge))
      return std::optional<qreal>();
    return snapEdge(map, orientation, position, lowEdge, span,
                    edgeVelocity(edge, axis), held[slot],
                    nativePerScreenPixel);
  };
  const auto left = snap(Left, 0, Qt::Vertical, original.left(), true, rows,
                         velocity.x());
  const auto top = snap(Top, 1, Qt::Horizontal, original.top(), true, columns,
                        velocity.y());
  const auto right = snap(Right, 2, Qt::Vertical, original.right(), false,
                          rows, velocity.x());
  const auto bottom = snap(Bottom, 3, Qt::Horizontal, original.bottom(), false,
                           columns, velocity.y());
  QRectF &snapped = result.rect;
  if (left && original.right() - *left >= minimumSize) {
    snapped.setLeft(*left);
    result.xs.push_back(*left);
    result.held[0] = *left;
  }
  if (right && *right - snapped.left() >= minimumSize) {
    snapped.setRight(*right);
    result.xs.push_back(*right);
    result.held[2] = *right;
  }
  if (top && original.bottom() - *top >= minimumSize) {
    snapped.setTop(*top);
    result.ys.push_back(*top);
    result.held[1] = *top;
  }
  if (bottom && *bottom - snapped.top() >= minimumSize) {
    snapped.setBottom(*bottom);
    result.ys.push_back(*bottom);
    result.held[3] = *bottom;
  }
  return result;
}

Snapped stepAxis(const LineMap &map, const QRectF &rect, Qt::Orientation axis,
                 bool grow, qreal maxDistance, qreal fallbackStep,
                 qreal minimumSize) {
  const QRectF original = rect.normalized();
  Snapped result{original, {}, {}, {}};
  const bool horizontal = axis == Qt::Horizontal;
  const Qt::Orientation lines = horizontal ? Qt::Vertical : Qt::Horizontal;
  const qreal low = horizontal ? original.left() : original.top();
  const qreal high = horizontal ? original.right() : original.bottom();
  const Span span = horizontal ? centralSpan(original.top(), original.bottom())
                               : centralSpan(original.left(), original.right());
  const qreal extent = horizontal ? map.width() : map.height();
  const int outward = grow ? 1 : -1;
  // Either way each edge keeps the line it lands on inside the crop.
  const auto lowHit =
      nextLine(map, lines, low, -outward, true, span, maxDistance);
  const auto highHit =
      nextLine(map, lines, high, outward, false, span, maxDistance);
  const qreal newLow = std::clamp<qreal>(
      lowHit ? *lowHit : low - outward * fallbackStep, 0.0, extent);
  const qreal newHigh = std::clamp<qreal>(
      highHit ? *highHit : high + outward * fallbackStep, 0.0, extent);
  if (newHigh - newLow < minimumSize)
    return result;
  QVector<qreal> &guides = horizontal ? result.xs : result.ys;
  if (lowHit)
    guides.push_back(newLow);
  if (highHit)
    guides.push_back(newHigh);
  if (horizontal) {
    result.rect.setLeft(newLow);
    result.rect.setRight(newHigh);
  } else {
    result.rect.setTop(newLow);
    result.rect.setBottom(newHigh);
  }
  return result;
}

Snapped fitRect(const LineMap &map, const QRectF &rect, qreal minimumSize) {
  const QRectF original = rect.normalized();
  Snapped result{original, {}, {}, {}};
  const qreal scale = map.pixelsPerLogical();
  const qreal reachX =
      std::clamp(original.width() * 0.3, 48.0 * scale, 160.0 * scale);
  const qreal reachY =
      std::clamp(original.height() * 0.3, 48.0 * scale, 160.0 * scale);
  const Span rows = centralSpan(original.top(), original.bottom());
  const Span columns = centralSpan(original.left(), original.right());
  // A rough frame wraps what it means: the element's own edge is inside it
  // even when a neighbour's border sits closer just outside. Only an edge
  // the frame clipped is looked for outside, and not far.
  const auto side = [&](Qt::Orientation orientation, qreal position,
                        int inward, bool lowEdge, Span span,
                        qreal reach) -> std::optional<int> {
    const int origin = qRound(position);
    const QVector<LineMap::Line> inside = map.lines(
        orientation, std::min(origin, origin + inward * qRound(reach)),
        std::max(origin, origin + inward * qRound(reach)), span.first,
        span.last);
    std::optional<int> best;
    for (const LineMap::Line &line : inside) {
      const int target = line.outer(lowEdge);
      if ((target - origin) * inward < 0)
        continue;
      if (!best || std::abs(target - origin) < std::abs(*best - origin))
        best = target;
    }
    if (best)
      return best;
    const int outside = qRound(reach / 4.0);
    const QVector<LineMap::Line> beyond = map.lines(
        orientation, std::min(origin, origin - inward * outside),
        std::max(origin, origin - inward * outside), span.first, span.last);
    for (const LineMap::Line &line : beyond) {
      const int target = line.outer(lowEdge);
      if (!best || std::abs(target - origin) < std::abs(*best - origin))
        best = target;
    }
    return best;
  };
  const auto left =
      side(Qt::Vertical, original.left(), 1, true, rows, reachX);
  const auto right =
      side(Qt::Vertical, original.right(), -1, false, rows, reachX);
  const auto top =
      side(Qt::Horizontal, original.top(), 1, true, columns, reachY);
  const auto bottom =
      side(Qt::Horizontal, original.bottom(), -1, false, columns, reachY);
  const qreal newLeft = left ? *left : original.left();
  const qreal newRight = right ? *right : original.right();
  const qreal newTop = top ? *top : original.top();
  const qreal newBottom = bottom ? *bottom : original.bottom();
  if (newRight - newLeft >= minimumSize) {
    result.rect.setLeft(newLeft);
    result.rect.setRight(newRight);
    if (left)
      result.xs.push_back(newLeft);
    if (right)
      result.xs.push_back(newRight);
  }
  if (newBottom - newTop >= minimumSize) {
    result.rect.setTop(newTop);
    result.rect.setBottom(newBottom);
    if (top)
      result.ys.push_back(newTop);
    if (bottom)
      result.ys.push_back(newBottom);
  }
  return result;
}

} // namespace linesnap
