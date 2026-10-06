/** @fileoverview Declares OCR provider selection smoke checks. */
#pragma once

#include <QString>

/** Runs OCR provider checks with fake gangway and tesseract commands. */
[[nodiscard]] bool runOcrProviderSmoke(QString &error);
