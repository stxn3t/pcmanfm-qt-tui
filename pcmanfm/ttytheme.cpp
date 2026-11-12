/*
 * ttytheme.cpp - loads tty.conf and applies the pcmanfm-tty look.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "ttytheme.h"

#include <libfm-qt6/tty.h>
#include <libfm-qt6/pathedit.h>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QTimer>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleFactory>

namespace PCManFM {

namespace {

bool g_enabled = false;
bool g_lowercase = true;

// Written to ~/.config/pcmanfm-tty/tty.conf on first run. Same knobs and
// colour names as heroic-tty / jellyfin-tty.
const char* kDefaultConf = R"(# pcmanfm-tty settings
# Change a value, save, and restart pcmanfm-tty.

[tty]
# set to false to get the plain PCManFM-Qt look back
enabled=true

# the first installed font in the list wins
font="JetBrains Mono, Iosevka, Fira Code, Cascadia Code, Source Code Pro, DejaVu Sans Mono, monospace"
font_size=13px

# borders: width, style (solid / dashed / dotted), corner radius
border_width=1px
border_style=solid
radius=3px

# floating labels on the box borders
label_size=12px
label_weight=500
# lowercase / none: also applies to tabs and menu items
label_transform=lowercase

# file icons in list views and the side pane (the icon view always shows them)
list_icons=false

# background opacity, 0.0 - 1.0. Below 1 the background is see-through and
# your wallpaper shows behind it (blurred, if Hyprland's blur is on);
# text, lines and icons stay solid. Try 0.6 - 0.8.
opacity=1.0

[colors]
# palette: true black (default flavor)
bg=#000000
bg_alt=#111113
fg=#d4d4d8
fg_dim=#8a8a94
border=#3f3f46
border_hover=#a1a1aa
accent=#a6e3a1
accent_alt=#c6f6c1
red=#f38ba8
yellow=#f9e2af
blue=#89b4fa
green=#a6e3a1
)";

QString firstInstalledFont(const QString& list) {
    const QStringList families = QFontDatabase::families();
    for(QString f : list.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        f = f.trimmed().remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
        if(f == QLatin1String("monospace")) {
            return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        }
        for(const QString& installed : families) {
            if(installed.compare(f, Qt::CaseInsensitive) == 0) {
                return installed;
            }
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

QColor readColor(QSettings& s, const char* key, const QColor& fallback) {
    QColor c(s.value(QLatin1String(key)).toString());
    return c.isValid() ? c : fallback;
}

int pixels(const QString& value, int fallback) {
    bool ok = false;
    int n = QString(value).remove(QLatin1String("px")).trimmed().toInt(&ok);
    return ok ? n : fallback;
}

} // namespace

TtyTheme::TtyTheme(QObject* parent) : QObject(parent) {}

QString TtyTheme::configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/pcmanfm-tty");
}

bool TtyTheme::enabled() {
    return g_enabled;
}

QString TtyTheme::labelText(const QString& text) {
    return (g_enabled && g_lowercase) ? text.toLower() : text;
}

QString TtyTheme::promptPath(const QString& path) {
    const QString home = QDir::homePath();
    if(path == home) {
        return QStringLiteral("~");
    }
    if(path.startsWith(home + QLatin1Char('/'))) {
        return QStringLiteral("~") + path.mid(home.length());
    }
    return path;
}

void TtyTheme::apply() {
    const QString dir = configDir();
    const QString confPath = dir + QStringLiteral("/tty.conf");
    if(!QFile::exists(confPath)) {
        QDir().mkpath(dir);
        QFile f(confPath);
        if(f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(kDefaultConf);
        }
    }

    QSettings s(confPath, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("tty"));
    g_enabled = s.value(QStringLiteral("enabled"), true).toBool();
    if(!g_enabled) {
        Fm::Tty::setEnabled(false);
        return;
    }
    const QString fontList = s.value(QStringLiteral("font"), QStringLiteral("JetBrains Mono, monospace")).toString();
    const QString fontSize = s.value(QStringLiteral("font_size"), QStringLiteral("13px")).toString();
    const QString borderWidth = s.value(QStringLiteral("border_width"), QStringLiteral("1px")).toString();
    const QString borderStyle = s.value(QStringLiteral("border_style"), QStringLiteral("solid")).toString();
    const QString radius = s.value(QStringLiteral("radius"), QStringLiteral("3px")).toString();
    const QString labelSize = s.value(QStringLiteral("label_size"), QStringLiteral("12px")).toString();
    const QString labelWeight = s.value(QStringLiteral("label_weight"), QStringLiteral("500")).toString();
    g_lowercase = s.value(QStringLiteral("label_transform"), QStringLiteral("lowercase")).toString()
                  == QLatin1String("lowercase");
    const bool listIcons = s.value(QStringLiteral("list_icons"), false).toBool();
    const qreal opacity = qBound(0.0, s.value(QStringLiteral("opacity"), 1.0).toDouble(), 1.0);
    s.endGroup();

    Fm::Tty::Colors c;
    s.beginGroup(QStringLiteral("colors"));
    c.bg = readColor(s, "bg", c.bg);
    c.bgAlt = readColor(s, "bg_alt", c.bgAlt);
    c.fg = readColor(s, "fg", c.fg);
    c.fgDim = readColor(s, "fg_dim", c.fgDim);
    c.border = readColor(s, "border", c.border);
    c.borderHover = readColor(s, "border_hover", c.borderHover);
    c.accent = readColor(s, "accent", c.accent);
    c.accentAlt = readColor(s, "accent_alt", c.accentAlt);
    c.red = readColor(s, "red", c.red);
    c.yellow = readColor(s, "yellow", c.yellow);
    c.blue = readColor(s, "blue", c.blue);
    c.green = readColor(s, "green", c.green);
    s.endGroup();

    Fm::Tty::setEnabled(true);
    Fm::Tty::setColors(c);
    Fm::Tty::setListIcons(listIcons);
    Fm::Tty::setOpacity(opacity);
    Fm::Tty::Metrics m;
    m.borderWidth = qMax(1, pixels(borderWidth, 1));
    m.borderStyle = borderStyle == QLatin1String("dashed") ? Qt::DashLine
                    : borderStyle == QLatin1String("dotted") ? Qt::DotLine : Qt::SolidLine;
    m.radius = qMax(0, pixels(radius, 3));
    m.labelPixelSize = pixels(labelSize, 12);
    m.labelWeight = labelWeight.toInt() > 0 ? labelWeight.toInt() : 500;
    Fm::Tty::setMetrics(m);

    // A stylesheet sits best on top of Fusion; a platform theme (Kvantum,
    // Breeze) would fight it.
    if(QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }

    QPalette pal;
    pal.setColor(QPalette::Window, c.bg);
    pal.setColor(QPalette::Base, c.bg);
    pal.setColor(QPalette::AlternateBase, c.bg);
    pal.setColor(QPalette::Button, c.bg);
    pal.setColor(QPalette::WindowText, c.fg);
    pal.setColor(QPalette::Text, c.fg);
    pal.setColor(QPalette::ButtonText, c.fg);
    pal.setColor(QPalette::BrightText, c.accentAlt);
    pal.setColor(QPalette::PlaceholderText, c.fgDim);
    pal.setColor(QPalette::Highlight, c.bgAlt);
    pal.setColor(QPalette::HighlightedText, c.accent);
    pal.setColor(QPalette::Link, c.accent);
    pal.setColor(QPalette::LinkVisited, c.accentAlt);
    pal.setColor(QPalette::ToolTipBase, c.bg);
    pal.setColor(QPalette::ToolTipText, c.fg);
    pal.setColor(QPalette::Mid, c.border);
    pal.setColor(QPalette::Dark, c.border);
    pal.setColor(QPalette::Shadow, c.bg);
    pal.setColor(QPalette::Light, c.border);
    pal.setColor(QPalette::Midlight, c.border);
    pal.setColor(QPalette::Disabled, QPalette::Text, c.border);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, c.border);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, c.border);
    QApplication::setPalette(pal);

    const QString family = firstInstalledFont(fontList);
    QFont font(family);
    font.setPixelSize(pixels(fontSize, 13));
    QApplication::setFont(font);

    QString qss;
    QFile res(QStringLiteral(":/tty/tty.qss"));
    if(res.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss = QString::fromUtf8(res.readAll());
    }
    QFile user(dir + QStringLiteral("/user.qss"));
    if(user.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss += QLatin1Char('\n') + QString::fromUtf8(user.readAll());
    }
    const int promptPx = pixels(fontSize, 13) + 2;

    // the tab close button: a thin "x" in the palette's colours
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                             + QStringLiteral("/pcmanfm-tty");
    QDir().mkpath(cacheDir);
    auto writeIcon = [&cacheDir](const QString& name, const QColor& color) {
        const QString path = cacheDir + QLatin1Char('/') + name;
        QFile f(path);
        if(f.open(QIODevice::WriteOnly)) {
            f.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'>"
                                   "<path d='M3 3 L9 9 M9 3 L3 9' stroke='%1' stroke-width='1.2' fill='none'/></svg>")
                        .arg(color.name()).toUtf8());
        }
        return path;
    };
    const QString closeIcon = writeIcon(QStringLiteral("close.svg"), c.fgDim);
    const QString closeIconHover = writeIcon(QStringLiteral("close-hover.svg"), c.red);
    auto rgba = [](const QColor& col) {
        // Qt stylesheets take alpha as 0-255
        return QStringLiteral("rgba(%1, %2, %3, %4)").arg(col.red()).arg(col.green()).arg(col.blue()).arg(col.alpha());
    };
    const QList<QPair<const char*, QString>> vars = {
        // see-through windows paint their tint themselves (see eventFilter):
        // Qt skips stylesheet backgrounds on translucent windows
        {"bgWindow", opacity < 1.0 ? QStringLiteral("transparent") : c.bg.name()},
        {"bgAltT", rgba(Fm::Tty::translucent(c.bgAlt))},
        {"bgAlt", c.bgAlt.name()},
        {"bg", c.bg.name()},
        {"fgDim", c.fgDim.name()},
        {"fg", c.fg.name()},
        {"borderHover", c.borderHover.name()},
        {"borderWidth", borderWidth},
        {"borderStyle", borderStyle},
        {"border", c.border.name()},
        {"accentAlt", c.accentAlt.name()},
        {"accent", c.accent.name()},
        {"red", c.red.name()},
        {"yellow", c.yellow.name()},
        {"blue", c.blue.name()},
        {"green", c.green.name()},
        {"radius", radius},
        {"labelSize", labelSize},
        {"labelWeight", labelWeight},
        {"fontSize", fontSize},
        {"promptSize", QString::number(promptPx) + QStringLiteral("px")},
        {"font", QLatin1Char('"') + family + QLatin1Char('"')},
        {"closeIconHover", closeIconHover},
        {"closeIcon", closeIcon},
    };
    for(const auto& v : vars) {
        qss.replace(QLatin1Char('@') + QLatin1String(v.first) + QLatin1Char('@'), v.second);
    }
    qApp->setStyleSheet(qss);

    // lowercases menus, boxes message boxes, makes windows see-through
    qApp->installEventFilter(new TtyTheme(qApp));
}

// Lowercase menu items as they are shown (the tty "label_transform").
// Menus listing user data (bookmarks, recent files, history) keep their case.
bool TtyTheme::eventFilter(QObject* watched, QEvent* event) {
    // see-through windows: dialogs and menus get a translucent surface before
    // they are created; other pop-ups (completion lists, combo box lists)
    // stay solid so they are readable over anything
    if(event->type() == QEvent::Paint && watched->isWidgetType()
       && watched->property("ttyWindow").toString() == QLatin1String("glass")) {
        // the see-through tint behind everything in the window
        QWidget* w = static_cast<QWidget*>(watched);
        QPainter p(w);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(static_cast<QPaintEvent*>(event)->rect(), Fm::Tty::translucent(Fm::Tty::colors().bg));
    }
    if(Fm::Tty::opacity() < 1.0 && watched->isWidgetType()
       && (event->type() == QEvent::Polish || event->type() == QEvent::Show)) {
        QWidget* w = static_cast<QWidget*>(watched);
        if(w->isWindow() && !w->property("ttyWindow").isValid()) {
            const bool glass = qobject_cast<QDialog*>(w) || qobject_cast<QMenu*>(w) || qobject_cast<QMainWindow*>(w);
            if(glass && (!w->testAttribute(Qt::WA_WState_Created) || w->testAttribute(Qt::WA_TranslucentBackground))) {
                w->setProperty("ttyWindow", QStringLiteral("glass"));
                w->setAttribute(Qt::WA_TranslucentBackground, true);
            }
            else if(!glass && event->type() == QEvent::Show && !w->inherits("QLabel")) {
                w->setProperty("ttyWindow", QStringLiteral("solid"));
                w->setStyleSheet(QStringLiteral("background: %1;").arg(Fm::Tty::colors().bg.name()));
            }
        }
    }
    if(event->type() == QEvent::Show) {
        // message boxes ("Operation not supported"...): a box labelled by
        // severity instead of the stock icon, like the other dialogs
        if(QMessageBox* box = qobject_cast<QMessageBox*>(watched)) {
            if(!box->property("ttyBoxed").toBool()) {
                box->setProperty("ttyBoxed", true);
                QString label;
                switch(box->icon()) {
                case QMessageBox::Critical: label = QStringLiteral("error"); break;
                case QMessageBox::Warning: label = QStringLiteral("warning"); break;
                case QMessageBox::Question: label = QStringLiteral("question"); break;
                case QMessageBox::Information: label = QStringLiteral("info"); break;
                default: label = box->windowTitle(); break;
                }
                if(QLabel* icon = box->findChild<QLabel*>(QStringLiteral("qt_msgboxex_icon_label"))) {
                    icon->hide();
                    icon->setFixedSize(0, 0);
                }
                Fm::Tty::boxDialog(box, label);
                QTimer::singleShot(0, box, [box] { box->adjustSize(); });
            }
            return QObject::eventFilter(watched, event);
        }
        if(!g_lowercase) {
            return QObject::eventFilter(watched, event);
        }
        if(QMenu* menu = qobject_cast<QMenu*>(watched)) {
            const QString name = menu->objectName();
            if(name != QLatin1String("menuRecentFiles") && name != QLatin1String("menu_Bookmarks")
               && !menu->property("ttyKeepCase").toBool()) {
                for(QAction* a : menu->actions()) {
                    if(!a->isSeparator() && !a->property("ttyKeepCase").toBool()) {
                        const QString lower = a->text().toLower();
                        if(lower != a->text()) {
                            a->setText(lower);
                        }
                    }
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------

TtyAlign::TtyAlign(QWidget* box, QWidget* reference)
    : QObject(box), box_(box), ref_(reference) {
    ref_->installEventFilter(this);
    box_->installEventFilter(this);
    connect(ref_, &QObject::destroyed, this, [this] { ref_ = nullptr; });
}

void TtyAlign::sync() {
    if(!ref_ || !box_->window() || ref_->window() != box_->window()) {
        return;
    }
    // the reference box's border line sits 8px below its top (margin-top)
    const int refLine = ref_->mapTo(box_->window(), QPoint(0, 0)).y() + 8;
    const int boxTop = box_->mapTo(box_->window(), QPoint(0, 0)).y();
    const int lineY = qMax(8, refLine - boxTop);
    if(box_->property("ttyLineY").toInt() != lineY) {
        box_->setProperty("ttyLineY", lineY);
        box_->setStyleSheet(QStringLiteral("#%1 { margin-top: %2px; }").arg(box_->objectName()).arg(lineY));
    }
}

bool TtyAlign::eventFilter(QObject* watched, QEvent* event) {
    if(event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Show) {
        sync();
    }
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------

TtyPathPrompt::TtyPathPrompt(Fm::PathEdit* edit, QWidget* parent)
    : QWidget(parent), edit_(edit) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(this);
    label_ = new QLabel(stack_);
    label_->setObjectName(QStringLiteral("ttyPrompt"));
    label_->setTextFormat(Qt::RichText);
    label_->setCursor(Qt::IBeamCursor);
    label_->setToolTip(tr("Click or press Ctrl+L to edit the path"));
    stack_->addWidget(label_);
    stack_->addWidget(edit_);
    layout->addWidget(stack_);
    stack_->setCurrentWidget(label_);

    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::textChanged, this, &TtyPathPrompt::updatePrompt);
    connect(edit_, &QLineEdit::returnPressed, this, &TtyPathPrompt::stopEditing);
    updatePrompt();
}

void TtyPathPrompt::updatePrompt() {
    const auto& c = Fm::Tty::colors();
    QString shown = TtyTheme::promptPath(edit_->text()).toHtmlEscaped();
    QString html;
    if(shown.startsWith(QLatin1Char('~'))) {
        html = QStringLiteral("<span style=\"color:%1\">~/</span>%2")
                   .arg(c.accent.name(), shown.mid(shown.length() > 1 ? 2 : 1));
    }
    else {
        html = shown;
    }
    html += QStringLiteral("<span style=\"color:%1\"> $ </span><span style=\"background-color:%2\">&nbsp;</span>")
                .arg(c.fgDim.name(), c.accent.name());
    label_->setText(html);
}

void TtyPathPrompt::startEditing() {
    stack_->setCurrentWidget(edit_);
    edit_->setFocus(Qt::ShortcutFocusReason);
    edit_->selectAll();
}

void TtyPathPrompt::stopEditing() {
    stack_->setCurrentWidget(label_);
    updatePrompt();
}

void TtyPathPrompt::mousePressEvent(QMouseEvent* event) {
    if(event->button() == Qt::LeftButton) {
        startEditing();
    }
    QWidget::mousePressEvent(event);
}

bool TtyPathPrompt::eventFilter(QObject* watched, QEvent* event) {
    if(watched == edit_ && event->type() == QEvent::FocusOut) {
        auto fe = static_cast<QFocusEvent*>(event);
        // the completer popup steals focus while typing: stay in edit mode
        if(fe->reason() != Qt::PopupFocusReason) {
            stopEditing();
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace PCManFM
