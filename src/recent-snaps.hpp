/** @fileoverview The short shelf of earlier captures the select overlay offers
 *  to reopen. Each entry keeps the editor's working document (full source plus
 *  operation log) so a reopened capture keeps its layers editable, and a small
 *  pre-rendered thumbnail so listing them costs nothing at startup. */
#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <optional>

struct OperationLog;

struct RecentSnap {
  /// Full-resolution source the editor reopens.
  QString sourcePath;
  /// Operation log sidecar; empty when the entry has none.
  QString logPath;
  QString thumbPath;
  /// When it was taken, ms since the epoch; 0 when unknown.
  qint64 stampMs = 0;
  /// Flattened preview, longest edge `kRecentThumbEdge`. Null when not loaded.
  QImage thumbnail;
};

/// How many captures the shelf keeps; older ones are pruned on record.
constexpr int kRecentSnapLimit = 5;
/// Longest edge of a stored thumbnail, in pixels.
constexpr int kRecentThumbEdge = 320;

/// `$OMASNAP_RECENT_DIR`, else `$XDG_STATE_HOME/omasnap/recent`. Created on
/// demand; empty when it cannot be.
[[nodiscard]] QString recentSnapsDirectory();

/// Newest first, at most `kRecentSnapLimit`. Thumbnails are decoded when
/// `loadThumbnails` is set.
[[nodiscard]] QVector<RecentSnap> listRecentSnaps(bool loadThumbnails = true);

/// Saves a working document and thumbnail on a worker. Replaces an entry with
/// the same log.recentId and prunes beyond the limit; never consumes live files.
[[nodiscard]] bool recordRecentSnap(const QImage &source,
                                    const OperationLog &log,
                                    const QImage &rendered, QString &error);

/// Finds the working document behind a preview, while it remains on the shelf.
[[nodiscard]] std::optional<RecentSnap> findRecentSnap(const QString &recentId);

/// Deletes one entry's files.
void removeRecentSnap(const RecentSnap &snap);
