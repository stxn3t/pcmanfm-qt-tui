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
#include <QLinearGradient>
#include <QListWidgetItem>
#include <QSignalBlocker>
#include <QPushButton>
#include <algorithm>
#include <QFileInfo>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QSlider>
#include <QListWidget>
#include <QRegularExpression>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QAbstractButton>
#include <QGroupBox>
#include <QGridLayout>
#include <QTabBar>
#include <QPixmap>
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
#include <QStyle>

namespace PCManFM {

namespace {

bool g_enabled = false;
bool g_lowercase = true;

// Written to ~/.config/pcmanfm-tty/tty.conf on first run. Same knobs and
// colour names as heroic-tty / jellyfin-tty.
const char* kDefaultConf = R"(# pcmanfm-tty settings
# Change a value, save, and restart pcmanfm-tty. The flavor and opacity can
# also be picked in the app: menu > view > theme...

[tty]
# set to false to get the plain PCManFM-Qt look back
enabled=true

# palette: black, black-glass, light-glass, or the name of a file in
# ~/.config/pcmanfm-tty/flavors/ (without .conf)
flavor=black

# background opacity, 0.0 - 1.0. Leave it out to use the flavor's (black: 1,
# black-glass: 0.75, light-glass: 0.65). Below 1 the background is
# see-through and your wallpaper shows behind it (blurred, if Hyprland's blur
# is on); text, lines and icons stay solid.
# opacity=1.0

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

[colors]
# Uncomment a line to override that colour of the flavor. Colours are #rrggbb
# or rgba(r, g, b, a) with a from 0.0 to 1.0. The black flavor's values:
# bg=#000000
# bg_alt=#111113
# fg=#d4d4d8
# fg_dim=#8a8a94
# border=#3f3f46
# border_hover=#a1a1aa
# accent=#a6e3a1
# accent_alt=#c6f6c1
# red=#f38ba8
# yellow=#f9e2af
# blue=#89b4fa
# green=#a6e3a1
# an extra see-through layer in the file list, dialogs and menus, on top of
# the window's bg, like Zen's darker pages (e.g. rgba(0, 0, 0, 0.5)); unset = none
# panel=rgba(0, 0, 0, 0)
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

// "#rrggbb", or "rgba(r, g, b, a)" with a from 0.0 to 1.0 (as in CSS)
QColor parseColor(const QString& text) {
    const QString t = text.trimmed();
    if(t.startsWith(QLatin1String("rgba("), Qt::CaseInsensitive) && t.endsWith(QLatin1Char(')'))) {
        const QStringList parts = t.mid(5, t.length() - 6).split(QLatin1Char(','));
        if(parts.size() == 4) {
            QColor c(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(), parts[2].trimmed().toInt());
            c.setAlphaF(qBound(0.0, parts[3].trimmed().toDouble(), 1.0));
            return c;
        }
        return QColor();
    }
    return QColor(t);
}

QColor readColor(QSettings& s, const char* key, const QColor& fallback) {
    // QSettings splits "rgba(1, 2, 3, 0.5)" at the commas: join it back
    const QVariant v = s.value(QLatin1String(key));
    const QString text = v.typeId() == QMetaType::QStringList ? v.toStringList().join(QLatin1Char(',')) : v.toString();
    QColor c = parseColor(text);
    return c.isValid() ? c : fallback;
}

// Qt stylesheets take alpha as 0-255
QString rgba(const QColor& col) {
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(col.red()).arg(col.green()).arg(col.blue()).arg(col.alpha());
}

// what apply() read from tty.conf, kept for restyle()
struct Style {
    QString family;
    QString fontSize = QStringLiteral("13px");
    QString borderWidth = QStringLiteral("1px");
    QString borderStyle = QStringLiteral("solid");
    QString radius = QStringLiteral("3px");
    QString labelSize = QStringLiteral("12px");
    QString labelWeight = QStringLiteral("500");
};
Style g_style;
QString g_flavor = QStringLiteral("black");
bool g_startedTranslucent = false;
TtyTheme* g_instance = nullptr;

void readColors(QSettings& s, Fm::Tty::Colors& c) {
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
    c.panel = readColor(s, "panel", c.panel);
}

QList<TtyFlavor> builtinFlavors() {
    QList<TtyFlavor> list;
    TtyFlavor black;
    black.id = QStringLiteral("black");
    black.name = QStringLiteral("black");
    black.opacity = 1.0;
    list << black;

    TtyFlavor blackGlass = black;
    blackGlass.id = QStringLiteral("black-glass");
    blackGlass.name = QStringLiteral("black glass");
    blackGlass.opacity = 0.75;
    list << blackGlass;

    // Zenhanced's colours (userChrome.css / userContent.css): the window is
    // #main-window's slate tint at 20%, the same everywhere (no darker
    // layer behind the file list, unlike Zen's pages)
    auto rgbaF = [](int r, int g, int b, qreal a) { QColor c(r, g, b); c.setAlphaF(a); return c; };
    TtyFlavor light = black;
    light.id = QStringLiteral("light-glass");
    light.name = QStringLiteral("light glass");
    light.opacity = 0.2;
    light.colors.bg = QColor(41, 47, 52);                      // rgba(41, 47, 52, 0.2)
    light.colors.bgAlt = rgbaF(128, 128, 128, 0.12);           // selected tab fill
    light.colors.fg = rgbaF(220, 220, 220, 0.85);              // --zen-text-color
    light.colors.fgDim = rgbaF(160, 160, 160, 0.8);            // --zen-primary-color
    light.colors.border = rgbaF(160, 160, 160, 0.55);          // --cli-border-color
    light.colors.borderHover = rgbaF(220, 220, 220, 0.75);     // --cli-border-hover-color
    light.colors.accent = QColor(0x3d, 0xdc, 0x84);            // --cli-prompt-color
    light.colors.accentAlt = rgbaF(61, 220, 132, 0.65);        // --cli-prompt-color-dim
    light.colors.blue = rgbaF(160, 180, 255, 0.8);             // links
    list << light;
    return list;
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

TtyTheme* TtyTheme::instance() {
    return g_instance;
}

QString TtyTheme::currentFlavor() {
    return g_flavor;
}

bool TtyTheme::startedTranslucent() {
    return g_startedTranslucent;
}

QList<TtyFlavor> TtyTheme::flavors() {
    QList<TtyFlavor> list = builtinFlavors();
    // your own: ~/.config/pcmanfm-tty/flavors/<id>.conf with a [colors] block
    // (missing colours come from black) and optionally [tty] name= / opacity=
    QDir dir(configDir() + QStringLiteral("/flavors"));
    for(const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name)) {
        QSettings f(fi.absoluteFilePath(), QSettings::IniFormat);
        TtyFlavor fl;
        fl.id = fi.completeBaseName();
        f.beginGroup(QStringLiteral("tty"));
        fl.name = f.value(QStringLiteral("name"), fl.id).toString();
        fl.opacity = qBound(0.0, f.value(QStringLiteral("opacity"), 1.0).toDouble(), 1.0);
        f.endGroup();
        f.beginGroup(QStringLiteral("colors"));
        readColors(f, fl.colors);
        f.endGroup();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const TtyFlavor& x) { return x.id == fl.id; }), list.end());
        list << fl;
    }
    return list;
}

TtyFlavor TtyTheme::flavor(const QString& id) {
    const auto list = flavors();
    for(const auto& f : list) {
        if(f.id == id) {
            return f;
        }
    }
    return list.first();
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
    g_style.fontSize = s.value(QStringLiteral("font_size"), QStringLiteral("13px")).toString();
    g_style.borderWidth = s.value(QStringLiteral("border_width"), QStringLiteral("1px")).toString();
    g_style.borderStyle = s.value(QStringLiteral("border_style"), QStringLiteral("solid")).toString();
    g_style.radius = s.value(QStringLiteral("radius"), QStringLiteral("3px")).toString();
    g_style.labelSize = s.value(QStringLiteral("label_size"), QStringLiteral("12px")).toString();
    g_style.labelWeight = s.value(QStringLiteral("label_weight"), QStringLiteral("500")).toString();
    g_lowercase = s.value(QStringLiteral("label_transform"), QStringLiteral("lowercase")).toString()
                  == QLatin1String("lowercase");
    const bool listIcons = s.value(QStringLiteral("list_icons"), false).toBool();
    g_flavor = s.value(QStringLiteral("flavor"), QStringLiteral("black")).toString();
    const TtyFlavor fl = flavor(g_flavor);
    g_flavor = fl.id;
    const qreal opacity = qBound(0.0, s.value(QStringLiteral("opacity"), fl.opacity).toDouble(), 1.0);
    s.endGroup();

    // the flavor's colours, then whatever [colors] overrides
    Fm::Tty::Colors c = fl.colors;
    s.beginGroup(QStringLiteral("colors"));
    readColors(s, c);
    s.endGroup();

    Fm::Tty::setEnabled(true);
    Fm::Tty::setColors(c);
    Fm::Tty::setListIcons(listIcons);
    Fm::Tty::setOpacity(opacity);
    g_startedTranslucent = opacity < 1.0;
    Fm::Tty::Metrics m;
    m.borderWidth = qMax(1, pixels(g_style.borderWidth, 1));
    m.borderStyle = g_style.borderStyle == QLatin1String("dashed") ? Qt::DashLine
                    : g_style.borderStyle == QLatin1String("dotted") ? Qt::DotLine : Qt::SolidLine;
    m.radius = qMax(0, pixels(g_style.radius, 3));
    m.labelPixelSize = pixels(g_style.labelSize, 12);
    m.labelWeight = g_style.labelWeight.toInt() > 0 ? g_style.labelWeight.toInt() : 500;
    Fm::Tty::setMetrics(m);

    // A stylesheet sits best on top of Fusion; a platform theme (Kvantum,
    // Breeze) would fight it.
    if(QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        QApplication::setStyle(fusion);
    }

    g_style.family = firstInstalledFont(fontList);
    QFont font(g_style.family);
    font.setPixelSize(pixels(g_style.fontSize, 13));
    QApplication::setFont(font);

    // lowercases menus, boxes dialogs, paints window backgrounds
    g_instance = new TtyTheme(qApp);
    qApp->installEventFilter(g_instance);

    restyle();
}

void TtyTheme::restyle() {
    const auto& c = Fm::Tty::colors();

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

    QString qss;
    QFile res(QStringLiteral(":/tty/tty.qss"));
    if(res.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss = QString::fromUtf8(res.readAll());
    }
    QFile user(configDir() + QStringLiteral("/user.qss"));
    if(user.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qss += QLatin1Char('\n') + QString::fromUtf8(user.readAll());
    }
    const int promptPx = pixels(g_style.fontSize, 13) + 2;

    // icons in the palette's colours; the file name carries the colour, so a
    // new flavor gets new files (Qt caches images by path)
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                             + QStringLiteral("/pcmanfm-tty");
    QDir().mkpath(cacheDir);
    auto writeSvg = [&cacheDir](const char* kind, const QColor& color, int size, const QString& body) {
        const QString path = cacheDir + QStringLiteral("/%1-%2.svg").arg(QLatin1String(kind), color.name(QColor::HexArgb).mid(1));
        if(!QFile::exists(path)) {
            QFile f(path);
            if(f.open(QIODevice::WriteOnly)) {
                f.write(QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='%1' height='%1' viewBox='0 0 %1 %1'>"
                                       "%2</svg>").arg(size).arg(body.arg(color.name())).toUtf8());
            }
        }
        return path;
    };
    const QString cross = QStringLiteral("<path d='M3 3 L9 9 M9 3 L3 9' stroke='%1' stroke-width='1.2' fill='none'/>");
    const QString down = QStringLiteral("<path d='M2 3.5 L5 6.5 L8 3.5' stroke='%1' stroke-width='1.3' fill='none'/>");
    const QString up = QStringLiteral("<path d='M2 6.5 L5 3.5 L8 6.5' stroke='%1' stroke-width='1.3' fill='none'/>");
    const QString right = QStringLiteral("<path d='M3.5 2 L6.5 5 L3.5 8' stroke='%1' stroke-width='1.3' fill='none'/>");

    const QList<QPair<const char*, QString>> vars = {
        // windows paint their own background (see eventFilter): Qt skips
        // stylesheet backgrounds on see-through windows
        {"bgWindow", QStringLiteral("transparent")},
        {"bgAltT", rgba(Fm::Tty::translucent(c.bgAlt))},
        {"bgAlt", rgba(c.bgAlt)},
        {"bg", c.bg.name()},
        {"fgDim", rgba(c.fgDim)},
        {"fg", rgba(c.fg)},
        {"borderHover", rgba(c.borderHover)},
        {"borderWidth", g_style.borderWidth},
        {"borderStyle", g_style.borderStyle},
        {"border", rgba(c.border)},
        {"accentAlt", rgba(c.accentAlt)},
        {"accent", rgba(c.accent)},
        {"red", rgba(c.red)},
        {"yellow", rgba(c.yellow)},
        {"blue", rgba(c.blue)},
        {"green", rgba(c.green)},
        {"radius", g_style.radius},
        {"labelSize", g_style.labelSize},
        {"labelWeight", g_style.labelWeight},
        {"fontSize", g_style.fontSize},
        {"promptSize", QString::number(promptPx) + QStringLiteral("px")},
        {"font", QLatin1Char('"') + g_style.family + QLatin1Char('"')},
        {"arrowDownHover", writeSvg("down", c.accent, 10, down)},
        {"arrowDown", writeSvg("down", c.fgDim, 10, down)},
        {"arrowUp", writeSvg("up", c.fgDim, 10, up)},
        {"arrowRight", writeSvg("right", c.fgDim, 10, right)},
        {"closeIconHover", writeSvg("close", c.red, 12, cross)},
        {"closeIcon", writeSvg("close", c.fgDim, 12, cross)},
    };
    for(const auto& v : vars) {
        qss.replace(QLatin1Char('@') + QLatin1String(v.first) + QLatin1Char('@'), v.second);
    }
    qApp->setStyleSheet(qss);

    for(QWidget* w : QApplication::allWidgets()) {
        if(w->property("ttyWindow").toString() == QLatin1String("solid")) {
            w->setStyleSheet(QStringLiteral("background: %1;").arg(c.bg.name()));
        }
        w->update();
    }
    if(g_instance) {
        Q_EMIT g_instance->restyled();
    }
}

bool TtyTheme::save(const QString& flavorId, qreal opacity) {
    const QString confPath = configDir() + QStringLiteral("/tty.conf");
    QFile f(confPath);
    if(!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    f.close();

    const QString flavorLine = QStringLiteral("flavor=") + flavorId;
    const QString opacityLine = QStringLiteral("opacity=") + QString::number(opacity, 'f', 2);
    static const QRegularExpression keyLine(QStringLiteral("^\\s*([A-Za-z_]+)\\s*="));
    QStringList out;
    QString section;
    bool wroteFlavor = false, wroteOpacity = false, notedColors = false, sawTty = false;
    auto finishTty = [&] {
        if(!wroteFlavor) { out << flavorLine; wroteFlavor = true; }
        if(!wroteOpacity) { out << opacityLine; wroteOpacity = true; }
    };
    for(const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if(trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            if(section == QLatin1String("tty")) {
                // keep a blank line between the new keys and the next section
                while(!out.isEmpty() && out.last().trimmed().isEmpty()) out.removeLast();
                finishTty();
                out << QString();
            }
            section = trimmed.mid(1, trimmed.length() - 2).toLower();
            sawTty = sawTty || section == QLatin1String("tty");
            out << line;
            continue;
        }
        const auto m = keyLine.match(line);
        if(section == QLatin1String("tty") && m.hasMatch()) {
            if(m.captured(1) == QLatin1String("flavor")) { out << flavorLine; wroteFlavor = true; continue; }
            if(m.captured(1) == QLatin1String("opacity")) { out << opacityLine; wroteOpacity = true; continue; }
        }
        if(section == QLatin1String("colors") && m.hasMatch()) {
            // the picked flavor wins: keep your colours, commented out
            if(!notedColors) {
                out << QStringLiteral("# commented out by the theme picker (menu > view > theme...);");
                out << QStringLiteral("# uncomment a line to override the flavor again");
                notedColors = true;
            }
            out << QStringLiteral("# ") + line;
            continue;
        }
        out << line;
    }
    if(section == QLatin1String("tty")) {
        finishTty();
    }
    if(!sawTty) {
        out << QStringLiteral("[tty]");
        finishTty();
    }
    if(!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    f.write(out.join(QLatin1Char('\n')).toUtf8());
    g_flavor = flavorId;
    return true;
}

// Message boxes: a box labelled by severity, and in place of the stock icon a
// small ASCII box glyph in the severity's colour:  ┌───┐
//                                                  │ x │
//                                                  └───┘
void TtyTheme::decorateMessageBox(QMessageBox* box) {
    const auto& c = Fm::Tty::colors();
    QString label, glyph;
    QColor color;
    switch(box->icon()) {
    case QMessageBox::Critical: label = QStringLiteral("error"); glyph = QStringLiteral("x"); color = c.red; break;
    case QMessageBox::Warning: label = QStringLiteral("warning"); glyph = QStringLiteral("!"); color = c.yellow; break;
    case QMessageBox::Question: label = QStringLiteral("question"); glyph = QStringLiteral("?"); color = c.accent; break;
    case QMessageBox::Information: label = QStringLiteral("info"); glyph = QStringLiteral("i"); color = c.blue; break;
    default: label = labelText(box->windowTitle()); break;
    }
    if(QLabel* icon = box->findChild<QLabel*>(QStringLiteral("qt_msgboxex_icon_label"))) {
        if(glyph.isEmpty()) {
            icon->hide();
        }
        else {
            icon->setPixmap(QPixmap());
            icon->setTextFormat(Qt::RichText);
            icon->setText(QStringLiteral("<pre style=\"color:%1; font-family:'%2'; margin:0\">"
                                         "┌───┐\n│ %3 │\n└───┘</pre>")
                          .arg(color.name(), QApplication::font().family(), glyph));
            icon->setMinimumSize(0, 0);
            icon->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            icon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            icon->setContentsMargins(0, 2, 0, 0);
            icon->show();
        }
    }
    // the message itself starts at the top, right next to the glyph
    if(QLabel* text = box->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"))) {
        text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        text->setIndent(0);
        text->setMargin(0);
    }
    if(auto grid = qobject_cast<QGridLayout*>(box->layout())) {
        for(int col = 0; col < grid->columnCount(); ++col) {
            grid->setColumnMinimumWidth(col, 0);
        }
        grid->setHorizontalSpacing(4);
    }
    Fm::Tty::boxDialog(box, label);
}

// Dialogs: Qt group boxes ("Ownership", "Access Control"...) get the same
// gapped-line box as the rest, and form labels, buttons and tabs are
// lowercased (label_transform). File names, paths and messages keep their case:
// only labels ending in ":" are touched.
void TtyTheme::decorateDialog(QDialog* dialog) {
    for(QGroupBox* group : dialog->findChildren<QGroupBox*>()) {
        if(group->isCheckable() || group->title().isEmpty() || group->property("ttyBox").toBool()) {
            continue;
        }
        const QString title = labelText(group->title().remove(QLatin1Char('&')));
        group->setTitle(QString());
        group->setProperty("ttyBox", true);
        group->style()->unpolish(group);
        group->style()->polish(group);
        Fm::Tty::FloatingLabel::attach(group, title);
    }
    if(!g_lowercase) {
        return;
    }
    for(QLabel* l : dialog->findChildren<QLabel*>()) {
        const QString t = l->text();
        if(l->textFormat() != Qt::RichText && !Qt::mightBeRichText(t) && t.trimmed().endsWith(QLatin1Char(':'))) {
            l->setText(t.toLower());
        }
    }
    for(QAbstractButton* b : dialog->findChildren<QAbstractButton*>()) {
        if(!b->text().isEmpty() && !b->property("ttyKeepCase").toBool()) {
            b->setText(b->text().toLower());
        }
    }
    for(QTabBar* bar : dialog->findChildren<QTabBar*>()) {
        for(int i = 0; i < bar->count(); ++i) {
            bar->setTabText(i, bar->tabText(i).toLower());
        }
    }
}

// Lowercase menu items as they are shown (the tty "label_transform").
// Menus listing user data (bookmarks, recent files, history) keep their case.
bool TtyTheme::eventFilter(QObject* watched, QEvent* event) {
    // Window backgrounds. The main window, dialogs and menus paint the bg
    // tint themselves: solid, or at the opacity when they are see-through
    // (they get a translucent surface before they are created while
    // opacity < 1). Other pop-ups (completion lists, drop-down lists) stay
    // solid so they are readable over anything.
    if(event->type() == QEvent::Paint && watched->isWidgetType()
       && watched->property("ttyWindow").toString() == QLatin1String("paint")) {
        QWidget* w = static_cast<QWidget*>(watched);
        const QColor bg = Fm::Tty::colors().bg;
        QPainter p(w);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(static_cast<QPaintEvent*>(event)->rect(),
                   w->testAttribute(Qt::WA_TranslucentBackground) ? Fm::Tty::translucent(bg) : bg);
        // dialogs and menus are content, like the file list: the panel layer on top
        const QColor panel = Fm::Tty::colors().panel;
        if(panel.alpha() > 0 && !qobject_cast<QMainWindow*>(w)) {
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.fillRect(static_cast<QPaintEvent*>(event)->rect(), panel);
        }
    }
    if(watched->isWidgetType() && (event->type() == QEvent::Polish || event->type() == QEvent::Show)) {
        QWidget* w = static_cast<QWidget*>(watched);
        if(w->isWindow() && !w->property("ttyWindow").isValid()) {
            const bool painted = qobject_cast<QDialog*>(w) || qobject_cast<QMenu*>(w) || qobject_cast<QMainWindow*>(w);
            if(painted) {
                w->setProperty("ttyWindow", QStringLiteral("paint"));
                if(Fm::Tty::opacity() < 1.0 && !w->testAttribute(Qt::WA_WState_Created)) {
                    w->setAttribute(Qt::WA_TranslucentBackground, true);
                }
            }
            else if(event->type() == QEvent::Show && !w->inherits("QLabel")) {
                w->setProperty("ttyWindow", QStringLiteral("solid"));
                w->setStyleSheet(QStringLiteral("background: %1;").arg(Fm::Tty::colors().bg.name()));
            }
        }
    }
    if(event->type() == QEvent::Polish) {
        // every dialog sits in a box labelled with its title
        if(QDialog* dialog = qobject_cast<QDialog*>(watched)) {
            if(!qobject_cast<QMessageBox*>(dialog) && !dialog->windowTitle().isEmpty()) {
                Fm::Tty::boxDialog(dialog, TtyTheme::labelText(dialog->windowTitle()));
            }
        }
    }
    if(event->type() == QEvent::Show) {
        if(QDialog* dialog = qobject_cast<QDialog*>(watched)) {
            if(!dialog->property("ttyDecorated").toBool()) {
                dialog->setProperty("ttyDecorated", true);
                if(QMessageBox* box = qobject_cast<QMessageBox*>(dialog)) {
                    decorateMessageBox(box);
                }
                decorateDialog(dialog);
                QTimer::singleShot(0, dialog, [dialog] { dialog->adjustSize(); });
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

// ---------------------------------------------------------------------------
// menu > view > theme...

namespace {
// The flavor as a strip: first a "window" tile (a wallpaper-like gradient
// with the flavor's tint at its opacity, the panel layer in its right half,
// framed in the flavor's line colour), then its text and accent colours.
QIcon swatches(const Fm::Tty::Colors& c, qreal opacity, const QColor& frame) {
    const int cell = 12, gap = 3, tile = 30;
    const QList<QColor> colors = {c.fg, c.accent, c.red, c.yellow, c.blue, c.green};
    QPixmap pm(tile + gap + colors.size() * (cell + gap), cell + 2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const QRect t(1, 1, tile - 1, cell - 1);
    QLinearGradient wall(t.topLeft(), t.bottomRight());
    wall.setColorAt(0.0, QColor(0x2a, 0x4a, 0x7a));
    wall.setColorAt(0.6, QColor(0x1a, 0x2a, 0x44));
    wall.setColorAt(1.0, QColor(0xb0, 0x6a, 0x30));
    p.fillRect(t, wall);
    QColor tint = c.bg;
    if(tint.alpha() == 255) {
        tint.setAlphaF(opacity);
    }
    p.fillRect(t, tint);
    if(c.panel.alpha() > 0) {
        p.fillRect(QRect(t.center().x(), t.top(), t.width() / 2 + 1, t.height()), c.panel);
    }
    p.setPen(c.border.alpha() > 0 ? c.border : frame);
    p.drawRect(t);
    for(int i = 0; i < colors.size(); ++i) {
        const QRect r(tile + gap + i * (cell + gap) + 1, 1, cell - 1, cell - 1);
        p.fillRect(r, colors[i]);
        p.setPen(frame);
        p.drawRect(r);
    }
    // same pixmap when selected: Qt would darken it otherwise
    QIcon icon;
    icon.addPixmap(pm, QIcon::Normal);
    icon.addPixmap(pm, QIcon::Selected);
    icon.addPixmap(pm, QIcon::Active);
    return icon;
}
}

TtyThemeDialog::TtyThemeDialog(QWidget* parent)
    : QDialog(parent),
      flavors_(TtyTheme::flavors()),
      origColors_(Fm::Tty::colors()),
      origOpacity_(Fm::Tty::opacity()) {
    setWindowTitle(tr("Theme"));
    auto layout = new QVBoxLayout(this);

    list_ = new QListWidget(this);
    list_->setIconSize(QSize(30 + 3 + 6 * 15, 14));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    for(const auto& f : flavors_) {
        auto item = new QListWidgetItem(swatches(f.colors, f.opacity, origColors_.borderHover), f.name, list_);
        item->setData(Qt::UserRole, f.id);
        if(f.id == TtyTheme::currentFlavor()) {
            list_->setCurrentItem(item);
        }
    }
    list_->setMinimumWidth(320);
    list_->setMinimumHeight(qMin(8, int(flavors_.size()) + 1) * 24);
    layout->addWidget(list_);

    auto row = new QHBoxLayout;
    auto label = new QLabel(tr("opacity:"), this);
    opacity_ = new QSlider(Qt::Horizontal, this);
    opacity_->setRange(5, 100);
    opacity_->setValue(qRound(origOpacity_ * 100));
    opacityValue_ = new QLabel(this);
    opacityValue_->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("1.00")));
    row->addWidget(label);
    row->addWidget(opacity_, 1);
    row->addWidget(opacityValue_);
    layout->addLayout(row);

    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setProperty("ttyKeepCase", true);
    layout->addWidget(note_);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // picking a flavor also picks its opacity; the slider can change it after
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if(row >= 0 && row < flavors_.size()) {
            const QSignalBlocker block(opacity_);
            opacity_->setValue(qRound(flavors_[row].opacity * 100));
        }
        preview();
    });
    connect(opacity_, &QSlider::valueChanged, this, &TtyThemeDialog::preview);
    updateNote();
}

void TtyThemeDialog::preview() {
    const int row = list_->currentRow();
    if(row >= 0 && row < flavors_.size()) {
        Fm::Tty::setColors(flavors_[row].colors);
    }
    Fm::Tty::setOpacity(opacity_->value() / 100.0);
    TtyTheme::restyle();
    updateNote();
}

void TtyThemeDialog::updateNote() {
    const qreal op = opacity_->value() / 100.0;
    opacityValue_->setText(QString::number(op, 'f', 2));
    QString text;
    if((op < 1.0) != TtyTheme::startedTranslucent()) {
        text = op < 1.0
            ? tr("See-through needs a restart: pcmanfm-tty --quit, then start it again. Until then windows stay solid.")
            : tr("Solid needs a restart: pcmanfm-tty --quit, then start it again.");
    }
    else if(op < 1.0) {
        text = tr("With Hyprland's blur on, the wallpaper behind is blurred.");
    }
    note_->setText(text);
    note_->setVisible(!text.isEmpty());
}

void TtyThemeDialog::accept() {
    const int row = list_->currentRow();
    const QString id = (row >= 0 && row < flavors_.size()) ? flavors_[row].id : TtyTheme::currentFlavor();
    TtyTheme::save(id, opacity_->value() / 100.0);
    QDialog::accept();
}

void TtyThemeDialog::reject() {
    // put back what was there before the dialog opened
    Fm::Tty::setColors(origColors_);
    Fm::Tty::setOpacity(origOpacity_);
    TtyTheme::restyle();
    QDialog::reject();
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
    // Ignored: the label's natural width (the whole path) must not size the
    // stack or the tool bar; long paths are cut from the left instead
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label_->setMinimumWidth(0);
    label_->installEventFilter(this);
    stack_->addWidget(label_);
    stack_->addWidget(edit_);
    layout->addWidget(stack_);
    stack_->setCurrentWidget(label_);

    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::textChanged, this, &TtyPathPrompt::updatePrompt);
    if(TtyTheme::instance()) {
        connect(TtyTheme::instance(), &TtyTheme::restyled, this, &TtyPathPrompt::updatePrompt);
    }
    connect(edit_, &QLineEdit::returnPressed, this, &TtyPathPrompt::stopEditing);
    updatePrompt();
}

QSize TtyPathPrompt::sizeHint() const {
    return QSize(fontMetrics().averageCharWidth() * 40, QWidget::sizeHint().height());
}

QSize TtyPathPrompt::minimumSizeHint() const {
    return QSize(fontMetrics().averageCharWidth() * 12, QWidget::minimumSizeHint().height());
}

void TtyPathPrompt::updatePrompt() {
    const auto& c = Fm::Tty::colors();
    const QString full = TtyTheme::promptPath(edit_->text());
    label_->setToolTip(full + QStringLiteral("\n") + tr("Click or press Ctrl+L to edit the path"));

    // room for the path: the label's width less its padding and " $ ▌"
    const QFontMetrics fm(label_->font());
    const int room = label_->contentsRect().width() - 12 - fm.horizontalAdvance(QStringLiteral(" $  "));
    QString path = full;
    bool cut = false;
    if(room > 0 && fm.horizontalAdvance(path) > room) {
        // keep as many trailing folders as fit: "…/Saved/Logs"
        const QString ellipsis = QStringLiteral("…");
        const QStringList parts = full.split(QLatin1Char('/'));
        QString tail;
        for(int i = parts.size() - 1; i >= 0; --i) {
            const QString next = parts[i] + (tail.isEmpty() ? QString() : QStringLiteral("/") + tail);
            if(fm.horizontalAdvance(ellipsis + QStringLiteral("/") + next) > room) {
                break;
            }
            tail = next;
        }
        path = tail.isEmpty() ? fm.elidedText(full, Qt::ElideLeft, room) : ellipsis + QStringLiteral("/") + tail;
        cut = true;
    }

    const QString shown = path.toHtmlEscaped();
    QString html;
    if(cut) {
        html = QStringLiteral("<span style=\"color:%1\">%2</span>%3")
                   .arg(c.fgDim.name(), shown.left(1), shown.mid(1));
    }
    else if(shown.startsWith(QLatin1Char('~'))) {
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
    if(watched == label_ && event->type() == QEvent::Resize) {
        updatePrompt(); // re-cut the path for the new width
    }
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
