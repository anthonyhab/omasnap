# Changelog

User-facing changes, newest first. This changelog starts with 1.20.0;
earlier development is recorded in the [commit history](https://github.com/omacom/omasnap/commits/main/).

## Unreleased

### Changed

- Reduce pointer repaint work on large displays and detect highlighter text
  rows on a worker to keep the overlay responsive.
- Reduce startup time and memory use by bypassing the GTK platform theme;
  overlay fonts and colors are supplied by Omasnap.
- Preserve the selected rectangle when switching between Region and
  Scrolling Region capture.

## 1.20.1

### Added

- Canvas growth around annotations, with Framed, Overflow, and Image
  boundary modes (`G` / `Shift+G`).
- Custom backdrop images and an optional default backdrop style.
- Highlighter snapping to screenshot text, with a freehand mode available.
- JetBrains Mono and Inter Display annotation fonts, alongside Neucha
  (`Shift+T`), and an outlined text style.

### Fixed

- Preserve normal framing as the canvas grows and keep custom backdrops
  consistent with canvas boundary modes.

## 1.20.0

### Changed

- Group the annotation toolbar into History, Style, Tools, and Actions.
- Present Region, Window, Scrolling Region, and Fullscreen capture tabs.
- Keep the pointer still during automatic scrolling capture.
- Allow zooming out to 10% and offset numbered markers from the pointer.

### Fixed

- Keep the toolbar, capture tabs, hotkey legend, and canvas geometry aligned.
- Remove the pixels actually covered by a cut-band drag.

[Unreleased changes](https://github.com/omacom/omasnap/compare/v1.20.1...main)
· [1.20.1 changes](https://github.com/omacom/omasnap/compare/v1.20.0...v1.20.1)
· [1.20.0 changes](https://github.com/omacom/omasnap/compare/v1.19.1...v1.20.0)
