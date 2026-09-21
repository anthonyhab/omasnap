#include "recent-snaps.hpp"

#include "capture.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>
#include <QStandardPaths>
#include <QLatin1StringView>
#include <QStringList>

#include <algorithm>

namespace {
// Entries are named by a zero-padded millisecond stamp so a plain name sort is
// a time sort. The capture identity groups repeated edits of the same shot.
constexpr QLatin1StringView kThumbSuffix(".thumb.png");

QString entryStem() {
  return QStringLiteral("%1").arg(QDateTime::currentMSecsSinceEpoch(), 16, 10,
                                  QChar('0'));
}

QString stemOf(const QString &thumbName) {
  return thumbName.chopped(kThumbSuffix.size());
}

RecentSnap snapForStem(const QDir &dir, const QString &stem) {
  RecentSnap snap;
  snap.sourcePath = dir.filePath(stem + QStringLiteral(".png"));
  snap.thumbPath = dir.filePath(stem + kThumbSuffix);
  snap.stampMs = stem.section(QLatin1Char('-'), 0, 0).toLongLong();
  const QString log = dir.filePath(stem + QStringLiteral(".json"));
  if (QFile::exists(log))
    snap.logPath = log;
  return snap;
}

bool validRecentId(const QString &id) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
  return pattern.match(id).hasMatch();
}

QStringList thumbNamesNewestFirst(const QDir &dir) {
  QStringList names =
      dir.entryList({QStringLiteral("*") + kThumbSuffix}, QDir::Files);
  std::ranges::sort(names, std::greater<>());
  return names;
}
} // namespace

QString recentSnapsDirectory() {
  QString root = qEnvironmentVariable("OMASNAP_RECENT_DIR");
  if (root.isEmpty()) {
    const QString state =
        QStandardPaths::writableLocation(QStandardPaths::StateLocation);
    if (state.isEmpty())
      return {};
    root = QDir(state).filePath(QStringLiteral("recent"));
  }
  // Working documents hold whole-monitor pixels, so the shelf is as private
  // as the runtime directory those came from.
  return ensurePrivateDirectory(root) ? QDir::cleanPath(root) : QString();
}

QVector<RecentSnap> listRecentSnaps(bool loadThumbnails) {
  const QString root = recentSnapsDirectory();
  if (root.isEmpty())
    return {};
  const QDir dir(root);
  QVector<RecentSnap> snaps;
  for (const QString &name : thumbNamesNewestFirst(dir)) {
    RecentSnap snap = snapForStem(dir, stemOf(name));
    if (!QFile::exists(snap.sourcePath)) {
      QFile::remove(snap.thumbPath); // orphaned by a failed move; tidy up
      continue;
    }
    if (loadThumbnails) {
      snap.thumbnail.load(snap.thumbPath);
      if (snap.thumbnail.isNull())
        continue;
    }
    snaps.push_back(std::move(snap));
    if (snaps.size() >= kRecentSnapLimit)
      break;
  }
  return snaps;
}

std::optional<RecentSnap> findRecentSnap(const QString &recentId) {
  if (!validRecentId(recentId))
    return std::nullopt;
  for (const RecentSnap &snap : listRecentSnaps(false)) {
    if (snap.thumbPath.endsWith(QLatin1Char('-') + recentId + kThumbSuffix))
      return snap;
  }
  return std::nullopt;
}

bool recordRecentSnap(const QImage &source, const OperationLog &log,
                      const QImage &rendered, QString &error) {
  if (rendered.isNull() || source.isNull()) {
    error = QStringLiteral("Nothing to remember: no working document");
    return false;
  }
  OperationLog savedLog = log;
  if (savedLog.recentId.isEmpty())
    savedLog.recentId = QUuid::createUuid().toString(QUuid::Id128);
  if (!validRecentId(savedLog.recentId)) {
    error = QStringLiteral("Invalid recent capture identity");
    return false;
  }
  const QString root = recentSnapsDirectory();
  if (root.isEmpty()) {
    error = QStringLiteral("Could not create the recent captures directory");
    return false;
  }
  const QDir dir(root);
  QLockFile lock(dir.filePath(QStringLiteral(".record.lock")));
  if (!lock.tryLock(5000)) {
    error = QStringLiteral("Could not lock the recent captures directory");
    return false;
  }
  QString stem = entryStem();
  const QString suffix = QLatin1Char('-') + savedLog.recentId;
  while (QFile::exists(dir.filePath(stem + suffix + kThumbSuffix)))
    stem = QStringLiteral("%1").arg(stem.toLongLong() + 1, 16, 10, QChar('0'));
  const RecentSnap snap = snapForStem(dir, stem + suffix);

  // Publish the thumbnail last: readers never see a half-written document.
  // Keep the previous entry until its replacement is completely ready.
  QSaveFile sourceFile(snap.sourcePath);
  sourceFile.setDirectWriteFallback(false);
  if (!sourceFile.open(QIODevice::WriteOnly) ||
      !sourceFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      !source.save(&sourceFile, "PNG") || !sourceFile.commit()) {
    error = QStringLiteral("Could not write recent capture source: %1")
                .arg(sourceFile.errorString());
    return false;
  }
  if (!saveOperationLog(operationLogPath(snap.sourcePath), savedLog, error)) {
    removeRecentSnap(snap);
    return false;
  }
  const QImage thumb = rendered.scaled(kRecentThumbEdge, kRecentThumbEdge,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation);
  QSaveFile thumbFile(snap.thumbPath);
  thumbFile.setDirectWriteFallback(false);
  if (!thumbFile.open(QIODevice::WriteOnly) ||
      !thumbFile.setPermissions(QFileDevice::ReadOwner |
                                QFileDevice::WriteOwner) ||
      !thumb.save(&thumbFile, "PNG") || !thumbFile.commit()) {
    error = QStringLiteral("Could not write recent capture thumbnail: %1")
                .arg(thumbFile.errorString());
    removeRecentSnap(snap);
    return false;
  }

  for (const QString &name : dir.entryList({QStringLiteral("*") + suffix + kThumbSuffix}, QDir::Files)) {
    if (dir.filePath(name) != snap.thumbPath)
      removeRecentSnap(snapForStem(dir, stemOf(name)));
  }

  const QStringList names = thumbNamesNewestFirst(dir);
  for (qsizetype index = kRecentSnapLimit; index < names.size(); ++index)
    removeRecentSnap(snapForStem(dir, stemOf(names.at(index))));
  return true;
}

void removeRecentSnap(const RecentSnap &snap) {
  QFile::remove(snap.thumbPath);
  QFile::remove(snap.sourcePath);
  if (!snap.logPath.isEmpty())
    QFile::remove(snap.logPath);
  else if (!snap.sourcePath.isEmpty())
    QFile::remove(operationLogPath(snap.sourcePath));
}
