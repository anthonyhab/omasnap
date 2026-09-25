/** @fileoverview Declares the line-snap smoke tests. */
#pragma once

#include <QString>

class QApplication;

/** The line map and its snap/fit/step rules on synthetic frames. */
[[nodiscard]] bool runLineSnapSmoke(QString &error);
/** Drag snapping, Space move, Alt bypass, and the Alt crop keys. */
[[nodiscard]] bool runLineSnapEditorSmoke(QApplication &application,
                                          QString &error);
