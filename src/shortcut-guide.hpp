#pragma once

#include "overlay-chrome.hpp"

#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

/// Readable overlay reference, kept above the image and outside the document.
class ShortcutGuide final : public QWidget {
  Q_OBJECT
public:
  explicit ShortcutGuide(QWidget *parent);
  void setEntries(const QVector<QPair<QString, QString>> &entries);
  void setExpanded(bool expanded);
  /// What the card must keep clear of (the capture, the toolbar): it moves
  /// to whichever screen corner covers none of it, staying put while its
  /// corner is still clear.
  void setAvoid(const ChromeObstacles &avoid);
  [[nodiscard]] bool expanded() const { return expanded_; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  [[nodiscard]] QRect toggleRect() const;
  /// Sizes the card and places it in a corner: re-chosen against the
  /// obstacles unless `keepCorner`, so toggling it never moves its button
  /// out from under the pointer.
  void reflow(bool keepCorner = false);
  QVector<QPair<QString, QString>> entries_;
  ChromeObstacles avoid_;
  int corner_ = 0;
  bool expanded_ = true;
  bool hovered_ = false;
  int firstRow_ = 0;
  int visibleRows_ = 0;
  int keyWidth_ = 0;
  int contentWidth_ = 0;
};
