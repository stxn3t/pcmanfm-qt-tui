/*
 * ttytheme.h - loads tty.conf and applies the pcmanfm-tty look.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PCMANFM_TTYTHEME_H
#define PCMANFM_TTYTHEME_H

#include <QObject>
#include <QString>
#include <QWidget>

class QDialog;
class QLabel;
class QMessageBox;
class QStackedWidget;

namespace Fm {
class PathEdit;
}

namespace PCManFM {

class TtyTheme : public QObject {
    Q_OBJECT
public:
    // Reads ~/.config/pcmanfm-tty/tty.conf (writing the defaults on first run)
    // and, if tty mode is enabled, styles the whole application.
    static void apply();

    static bool enabled();

    // Folder name for labels and tabs: lowercased when label_transform=lowercase.
    static QString labelText(const QString& text);

    // "~/downloads" style display of a path (home abbreviated).
    static QString promptPath(const QString& path);

    static QString configDir();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit TtyTheme(QObject* parent);
    static void decorateMessageBox(QMessageBox* box);
    static void decorateDialog(QDialog* dialog);
};

// Keeps a box's top border line level with another widget's: used so the
// side pane box lines up with the folder box below the tab bar.
class TtyAlign : public QObject {
    Q_OBJECT
public:
    TtyAlign(QWidget* box, QWidget* reference);
    void sync();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    QWidget* box_;
    QWidget* ref_;
};

// The path line in the tool bar: "~/downloads $ ▌". Clicking it (or Ctrl+L)
// swaps in the normal editable path entry; Enter, Esc or leaving the field
// swaps back.
class TtyPathPrompt : public QWidget {
    Q_OBJECT
public:
    TtyPathPrompt(Fm::PathEdit* edit, QWidget* parent = nullptr);
    void startEditing();
    void stopEditing();
    Fm::PathEdit* edit() const { return edit_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updatePrompt();
    QStackedWidget* stack_;
    QLabel* label_;
    Fm::PathEdit* edit_;
};

} // namespace PCManFM

#endif // PCMANFM_TTYTHEME_H
