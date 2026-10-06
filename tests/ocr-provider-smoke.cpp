/** @fileoverview Tests OCR provider choice and fallback without a Mac or tesseract. */
#include "ocr-provider-smoke.hpp"

#include "capture.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QTemporaryDir>

namespace {
/** Writes an executable fake command. */
bool writeExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size())
    return false;
  file.close();
  return QFile::setPermissions(path, QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
}

struct OcrCase {
  const char *name;
  const char *provider; // OMASNAP_OCR, or nullptr for unset
  const char *gangway;  // fake gangway's behavior: ok, empty or fail
  QString text;         // expected text, empty for an error
  bool tesseractRuns;
};

bool runCase(const OcrCase &c, const QString &marker, QString &error) {
  c.provider ? qputenv("OMASNAP_OCR", c.provider) : qunsetenv("OMASNAP_OCR");
  qputenv("OMASNAP_TEST_GANGWAY", c.gangway);
  QFile::remove(marker);
  QImage image(8, 8, QImage::Format_RGB32);
  image.fill(Qt::white);
  QString ocrError;
  const QString text = recognizeText(image, ocrError);
  const bool ran = QFile::exists(marker);
  if (text != c.text || ran != c.tesseractRuns ||
      (c.text.isEmpty() && ocrError.isEmpty())) {
    error = QStringLiteral("OCR case %1: got \"%2\" (error \"%3\"), tesseract %4")
                .arg(QString::fromUtf8(c.name), text, ocrError,
                     ran ? QStringLiteral("ran") : QStringLiteral("did not run"));
    return false;
  }
  return true;
}
} // namespace

bool runOcrProviderSmoke(QString &error) {
  const QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create OCR test directory");
    return false;
  }
  const QString marker = directory.filePath(QStringLiteral("tesseract-ran"));
  const QByteArray gangway(
      "#!/usr/bin/env bash\n"
      "cat >/dev/null\n"
      "case \"$OMASNAP_TEST_GANGWAY\" in\n"
      "  ok) printf '{\"ok\":true,\"text\":\"from vision\\\\n\",\"host\":\"mac\","
      "\"round_trip_ms\":9}\\n' ;;\n"
      "  empty) printf '{\"ok\":true,\"text\":\"\",\"host\":\"mac\"}\\n' ;;\n"
      "  *) echo 'Error: no Mac running a Gangway with text recognition is "
      "connected' >&2; exit 1 ;;\n"
      "esac\n");
  const QByteArray tesseract =
      "#!/usr/bin/env bash\ncat >/dev/null\ntouch \"" + marker.toUtf8() +
      "\"\nprintf 'from tesseract\\n'\n";
  if (!writeExecutable(directory.filePath(QStringLiteral("gangway")), gangway) ||
      !writeExecutable(directory.filePath(QStringLiteral("tesseract")),
                       tesseract)) {
    error = QStringLiteral("Could not create fake OCR commands");
    return false;
  }

  const QByteArray oldPath = qgetenv("PATH");
  const bool providerWasSet = qEnvironmentVariableIsSet("OMASNAP_OCR");
  const QByteArray oldProvider = qgetenv("OMASNAP_OCR");
  const auto restoreEnvironment = qScopeGuard([=] {
    qputenv("PATH", oldPath);
    providerWasSet ? qputenv("OMASNAP_OCR", oldProvider)
                   : qunsetenv("OMASNAP_OCR");
    qunsetenv("OMASNAP_TEST_GANGWAY");
  });
  qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);

  const QString vision = QStringLiteral("from vision");
  const QString local = QStringLiteral("from tesseract");
  const OcrCase cases[] = {
      {"auto, Mac answers", nullptr, "ok", vision, false},
      {"auto, no Mac", nullptr, "fail", local, true},
      {"auto, Mac finds no text", nullptr, "empty", {}, false},
      {"tesseract only", "tesseract", "ok", local, true},
      {"gangway only, no Mac", "gangway", "fail", {}, false},
  };
  for (const OcrCase &c : cases)
    if (!runCase(c, marker, error))
      return false;
  return true;
}
