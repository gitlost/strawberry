/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <chrono>

#include <QApplication>
#include <QGuiApplication>
#include <QWindow>
#include <QScreen>
#include <QWidget>
#include <QString>
#include <QImage>
#include <QPixmap>
#include <QBitmap>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QColor>
#include <QBrush>
#include <QCursor>
#include <QPen>
#include <QRect>
#include <QPoint>
#include <QFont>
#include <QTimer>
#include <QTimeLine>
#include <QTransform>
#include <QBoxLayout>
#include <QLinearGradient>
#include <QSettings>
#include <QFlags>
#include <QtEvents>

#ifdef HAVE_QPA_QPLATFORMNATIVEINTERFACE
#  include <qpa/qplatformnativeinterface.h>
#endif

#include "core/settings.h"
#include "constants/notificationssettings.h"

#include "osdpretty.h"
#include "ui_osdpretty.h"

#ifdef Q_OS_WIN32
#  include <windows.h>
#endif

#ifdef Q_OS_WIN32
#  include "utilities/winutils.h"
#endif

using namespace std::chrono_literals;
using namespace Qt::Literals::StringLiterals;

namespace {

constexpr int kDropShadowSize = 13;
constexpr int kBorderRadius = 10;
constexpr int kMaxIconSize = 100;
constexpr int kSnapProximity = 20;

}  // namespace

OSDPretty::OSDPretty(const Mode mode, QWidget *parent)
    : QWidget(parent),
      ui_(new Ui_OSDPretty),
      mode_(mode),
      background_color_(OSDPrettySettings::kPresetBlue),
      background_opacity_(0.85),
      popup_screen_(nullptr),
      disable_duration_(false),
      timeout_(new QTimer(this)),
      fading_enabled_(false),
      fader_(new QTimeLine(300, this)),
      toggle_mode_(false) {

  setWindowTitle(u"OSDPretty"_s);
  setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint);

  setAttribute(Qt::WA_TranslucentBackground, true);
  setAttribute(Qt::WA_X11NetWmWindowTypeNotification, true);
  setAttribute(Qt::WA_ShowWithoutActivating, true);

  ui_->setupUi(this);

#ifdef Q_OS_WIN32
  // Don't show the window in the taskbar.  Qt::ToolTip does this too, but it adds an extra ugly shadow.
  int ex_style = GetWindowLong(reinterpret_cast<HWND>(winId()), GWL_EXSTYLE);
  ex_style |= WS_EX_NOACTIVATE;
  SetWindowLong(reinterpret_cast<HWND>(winId()), GWL_EXSTYLE, ex_style);
#endif

  // Mode settings
  switch (mode_) {
    case Mode::Popup:
      setCursor(QCursor(Qt::ArrowCursor));
      break;

    case Mode::Draggable:
      setCursor(QCursor(Qt::OpenHandCursor));
      break;
  }

  // Timeout
  timeout_->setSingleShot(true);
  timeout_->setInterval(5s);
  QObject::connect(timeout_, &QTimer::timeout, this, &OSDPretty::hide);

  ui_->icon->setMaximumSize(kMaxIconSize, kMaxIconSize);

  // Fader
  QObject::connect(fader_, &QTimeLine::valueChanged, this, &OSDPretty::FaderValueChanged);
  QObject::connect(fader_, &QTimeLine::finished, this, &OSDPretty::FaderFinished);

  // Load the show edges and corners
  QImage shadow_edge(u":/pictures/osd_shadow_edge.png"_s);
  QImage shadow_corner(u":/pictures/osd_shadow_corner.png"_s);
  for (int i = 0; i < 4; ++i) {
    QTransform rotation = QTransform().rotate(90 * i);
    shadow_edge_[i] = QPixmap::fromImage(shadow_edge.transformed(rotation));
    shadow_corner_[i] = QPixmap::fromImage(shadow_corner.transformed(rotation));
  }
  background_ = QPixmap(u":/pictures/osd_background.png"_s);

  // Set the margins to allow for the drop shadow
  QBoxLayout *l = qobject_cast<QBoxLayout*>(layout());
  QMargins margin = l->contentsMargins();
  margin.setTop(margin.top() + kDropShadowSize);
  margin.setBottom(margin.bottom() + kDropShadowSize);
  margin.setLeft(margin.left() + kDropShadowSize);
  margin.setRight(margin.right() + kDropShadowSize);
  l->setContentsMargins(margin);

  QObject::connect(qApp, &QApplication::screenAdded, this, &OSDPretty::ScreenAdded);
  QObject::connect(qApp, &QApplication::screenRemoved, this, &OSDPretty::ScreenRemoved);

}

OSDPretty::~OSDPretty() {
  delete ui_;
}

void OSDPretty::showEvent(QShowEvent *e) {

  screens_.clear();
  const QList<QScreen*> screens = QGuiApplication::screens();
  for (QScreen *screen : screens) {
    screens_.insert(screen->name(), screen);
  }

  // Get current screen resolution
  QScreen *screen = current_screen();
  if (screen) {
    QRect resolution = screen->availableGeometry();
    // Leave 200 px for icon
    ui_->summary->setMaximumWidth(resolution.width() - 200);
    ui_->message->setMaximumWidth(resolution.width() - 200);
    // Set maximum size for the OSD, a little margin here too
    setMaximumSize(resolution.width() - 100, resolution.height() - 100);
  }

  setWindowOpacity(fading_enabled_ ? 0.0 : 1.0);

  QWidget::showEvent(e);

  Load();
  Reposition();

  if (fading_enabled_) {
    fader_->setDirection(QTimeLine::Forward);
    fader_->start();  // Timeout will be started in FaderFinished
  }
  else if (mode_ == Mode::Popup) {
    if (!disable_duration()) {
      timeout_->start();
    }
    // Ensures it is above when showing the preview
    raise();
  }

}

void OSDPretty::ScreenAdded(QScreen *screen) {

  screens_.insert(screen->name(), screen);

}

void OSDPretty::ScreenRemoved(QScreen *screen) {

  if (screens_.contains(screen->name())) screens_.remove(screen->name());
  if (screen == popup_screen_) popup_screen_ = current_screen();

}

bool OSDPretty::IsTransparencyAvailable() {

#ifdef HAVE_QPA_QPLATFORMNATIVEINTERFACE
  if (qApp && QGuiApplication::platformName() == "xcb"_L1) {
    QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
    QScreen *screen = popup_screen_ == nullptr ? QGuiApplication::primaryScreen() : popup_screen_;
    if (native && screen) {
      return native->nativeResourceForScreen(QByteArray("compositingEnabled"), screen);
    }
  }
#endif

  return true;

}

void OSDPretty::Load() {

  Settings s;
  s.beginGroup(OSDPrettySettings::kSettingsGroup);
  foreground_color_ = QColor(static_cast<QRgb>(s.value(OSDPrettySettings::kForegroundColor, 0).toInt()));
  background_color_ = QColor(static_cast<QRgb>(s.value(OSDPrettySettings::kBackgroundColor, OSDPrettySettings::kPresetBlue).toInt()));
  background_opacity_ = s.value(OSDPrettySettings::kBackgroundOpacity, 0.85).toFloat();
  font_.fromString(s.value(OSDPrettySettings::kFont, u"Verdana,9,-1,5,50,0,0,0,0,0"_s).toString());
  disable_duration_ = s.value(OSDPrettySettings::kDisableDuration, false).toBool();
#ifdef Q_OS_WIN32
  fading_enabled_ = s.value(OSDPrettySettings::kFading, true).toBool();
#else
  fading_enabled_ = s.value(OSDPrettySettings::kFading, false).toBool();
#endif

  if (s.contains(OSDPrettySettings::kPopupScreen)) {
    popup_screen_name_ = s.value(OSDPrettySettings::kPopupScreen).toString();
    if (screens_.contains(popup_screen_name_)) {
      popup_screen_ = screens_.value(popup_screen_name_);
    }
    else {
      popup_screen_ = current_screen();
      if (current_screen()) popup_screen_name_ = current_screen()->name();
      else popup_screen_name_.clear();
    }
  }
  else {
    popup_screen_ = current_screen();
    if (current_screen()) popup_screen_name_ = current_screen()->name();
  }

  if (s.contains(OSDPrettySettings::kPopupPos)) {
    popup_pos_ = s.value(OSDPrettySettings::kPopupPos).toPoint();
  }
  else {
    if (popup_screen_) {
      QRect geometry = popup_screen_->availableGeometry();
      popup_pos_.setX(geometry.width() - width());
      popup_pos_.setY(0);
    }
    else {
      popup_pos_.setX(0);
      popup_pos_.setY(0);
    }
  }

  set_font(font());
  set_foreground_color(foreground_color());

  s.endGroup();

}

void OSDPretty::ReloadSettings() {
  Load();
  if (isVisible()) update();
}

QRect OSDPretty::BoxBorder() const {
  return rect().adjusted(kDropShadowSize, kDropShadowSize, -kDropShadowSize, -kDropShadowSize);
}

void OSDPretty::paintEvent(QPaintEvent *e) {

  Q_UNUSED(e)

  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  QRect box(BoxBorder());

  // Shadow corners
  const int kShadowCornerSize = kDropShadowSize + kBorderRadius;
  p.drawPixmap(0, 0, shadow_corner_[0]);
  p.drawPixmap(width() - kShadowCornerSize, 0, shadow_corner_[1]);
  p.drawPixmap(width() - kShadowCornerSize, height() - kShadowCornerSize, shadow_corner_[2]);
  p.drawPixmap(0, height() - kShadowCornerSize, shadow_corner_[3]);

  // Shadow edges
  p.drawTiledPixmap(kShadowCornerSize, 0, width() - kShadowCornerSize * 2, kDropShadowSize, shadow_edge_[0]);
  p.drawTiledPixmap(width() - kDropShadowSize, kShadowCornerSize, kDropShadowSize, height() - kShadowCornerSize * 2, shadow_edge_[1]);
  p.drawTiledPixmap(kShadowCornerSize, height() - kDropShadowSize, width() - kShadowCornerSize * 2, kDropShadowSize, shadow_edge_[2]);
  p.drawTiledPixmap(0, kShadowCornerSize, kDropShadowSize, height() - kShadowCornerSize * 2, shadow_edge_[3]);

  // Box background
  p.setBrush(background_color_);
  p.setPen(QPen());
  p.setOpacity(background_opacity_);
  p.drawRoundedRect(box, kBorderRadius, kBorderRadius);

  // Background pattern
  QPainterPath background_path;
  background_path.addRoundedRect(box, kBorderRadius, kBorderRadius);
  p.setClipPath(background_path);
  p.setOpacity(1.0);
  p.drawPixmap(box.right() - background_.width(), box.bottom() - background_.height(), background_);
  p.setClipping(false);

  // Gradient overlay
  QLinearGradient gradient(0, 0, 0, height());
  gradient.setColorAt(0, QColor(255, 255, 255, 130));
  gradient.setColorAt(1, QColor(255, 255, 255, 50));
  p.setBrush(gradient);
  p.drawRoundedRect(box, kBorderRadius, kBorderRadius);

  // Box border
  p.setBrush(QBrush());
  p.setPen(QPen(background_color_.darker(150), 2));
  p.drawRoundedRect(box, kBorderRadius, kBorderRadius);

}

void OSDPretty::SetMessage(const QString &summary, const QString &message, const QImage &image) {

  if (!image.isNull()) {
    QImage scaled_image = image.scaled(static_cast<int>(kMaxIconSize * devicePixelRatioF()), static_cast<int>(kMaxIconSize * devicePixelRatioF()), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled_image.setDevicePixelRatio(devicePixelRatioF());
    ui_->icon->setPixmap(QPixmap::fromImage(scaled_image));
    ui_->icon->show();
  }
  else {
    ui_->icon->hide();
  }

  ui_->summary->setText(summary);
  ui_->message->setText(message);

  if (isVisible()) Reposition();

}

// Set the desired message and then show the OSD
void OSDPretty::ShowMessage(const QString &summary, const QString &message, const QImage &image) {

  SetMessage(summary, message, image);

  if (isVisible() && mode_ == Mode::Popup) {
    // The OSD is already visible, toggle or restart the timer
    if (toggle_mode()) {
      set_toggle_mode(false);
      // If timeout is disabled, timer hadn't been started
      if (!disable_duration()) {
        timeout_->stop();
      }
      hide();
    }
    else {
      if (!disable_duration()) {
        timeout_->start();  // Restart the timer
      }
    }
  }
  else {
    if (toggle_mode()) {
      set_toggle_mode(false);
    }
    // The OSD is not visible, show it
    show();
  }

}

void OSDPretty::setVisible(bool visible) {

  if (!visible && fading_enabled_ && fader_->direction() == QTimeLine::Forward) {
    fader_->setDirection(QTimeLine::Backward);
    fader_->start();
  }
  else {
    QWidget::setVisible(visible);
  }

}

void OSDPretty::FaderFinished() {

  if (fader_->direction() == QTimeLine::Backward) {
    hide();
  }
  else if (mode_ == Mode::Popup && !disable_duration()) {
    timeout_->start();
  }

}

void OSDPretty::FaderValueChanged(const qreal value) {
  setWindowOpacity(value);
}

void OSDPretty::Reposition() {

  // Make the OSD the proper size
  layout()->activate();
  resize(sizeHint());

  // Work out where to place the OSD.  -1 for x or y means "on the right or bottom edge".
  if (popup_screen_) {

    QRect geometry = popup_screen_->availableGeometry();

    int x = popup_pos_.x() < 0 ? geometry.right() - width() : geometry.left() + popup_pos_.x();
    int y = popup_pos_.y() < 0 ? geometry.bottom() - height() : geometry.top() + popup_pos_.y();

#ifndef Q_OS_WIN32
    x = qBound(0, x, geometry.right() - width());
    y = qBound(0, y, geometry.bottom() - height());
#endif
    move(x, y);
  }

  // Create a mask for the actual area of the OSD
  QBitmap mask(size());
  mask.clear();

  QPainter p(&mask);
  p.setBrush(Qt::color1);
  p.drawRoundedRect(BoxBorder().adjusted(-1, -1, 0, 0), kBorderRadius, kBorderRadius);
  p.end();

  // If there's no compositing window manager running then we have to set an XShape mask.
  if (IsTransparencyAvailable())
    clearMask();
  else {
    setMask(mask);
  }

  // On windows, enable blurbehind on the masked area
#ifdef Q_OS_WIN32
  Utilities::enableBlurBehindWindow(windowHandle(), QRegion(mask));
#endif

}

void OSDPretty::enterEvent(QEnterEvent *e) {

  Q_UNUSED(e)

  if (mode_ == Mode::Popup) {
    setWindowOpacity(0.25);
  }

}

void OSDPretty::leaveEvent(QEvent *e) {

  Q_UNUSED(e)

  setWindowOpacity(1.0);

}

void OSDPretty::mousePressEvent(QMouseEvent *e) {

  if (mode_ == Mode::Popup) {
    hide();
  }
  else {
    original_window_pos_ = pos();
    drag_start_pos_ = e->globalPosition().toPoint();
  }

}

void OSDPretty::mouseMoveEvent(QMouseEvent *e) {

  if (mode_ == Mode::Draggable) {
    QPoint delta = e->globalPosition().toPoint() - drag_start_pos_;
    QPoint new_pos = original_window_pos_ + delta;

    // Keep it to the bounds of the desktop
    QScreen *screen = current_screen(e->globalPosition().toPoint());
    if (!screen) return;

    QRect geometry = screen->availableGeometry();

    new_pos.setX(qBound(geometry.left(), new_pos.x(), geometry.right() - width()));
    new_pos.setY(qBound(geometry.top(), new_pos.y(), geometry.bottom() - height()));

    // Snap to center
    int snap_x = geometry.center().x() - width() / 2;
    if (new_pos.x() > snap_x - kSnapProximity && new_pos.x() < snap_x + kSnapProximity) {
      new_pos.setX(snap_x);
    }

    move(new_pos);

    popup_screen_ = screen;
    popup_screen_name_ = screen->name();
  }

}

void OSDPretty::mouseReleaseEvent(QMouseEvent *e) {

  Q_UNUSED(e)

  if (current_screen() && mode_ == Mode::Draggable) {
    popup_screen_ = current_screen();
    popup_screen_name_ = current_screen()->name();
    popup_pos_ = current_pos();
    Q_EMIT PositionChanged();
  }

}

QScreen *OSDPretty::current_screen(const QPoint pos) const {

  QScreen *screen = QGuiApplication::screenAt(pos);
  if (!screen) screen = QGuiApplication::primaryScreen();

  return screen;

}

QScreen *OSDPretty::current_screen() const { return current_screen(pos()); }

QPoint OSDPretty::current_pos() const {

  if (current_screen()) {
    QRect geometry = current_screen()->availableGeometry();

    int x = pos().x() >= geometry.right() - width() ? -1 : pos().x() - geometry.left();
    int y = pos().y() >= geometry.bottom() - height() ? -1 : pos().y() - geometry.top();

    return QPoint(x, y);
  }

  return QPoint(0, 0);

}

void OSDPretty::set_background_color(const QRgb color) {
  background_color_ = color;
  if (isVisible()) update();
}

void OSDPretty::set_background_opacity(const qreal opacity) {
  background_opacity_ = opacity;
  if (isVisible()) update();
}

void OSDPretty::set_foreground_color(const QRgb color) {

  foreground_color_ = QColor(color);

  QPalette p;
  p.setColor(QPalette::WindowText, foreground_color_);

  ui_->summary->setPalette(p);
  ui_->message->setPalette(p);

}

void OSDPretty::set_popup_duration(const int msec) {
  timeout_->setInterval(msec);
}

void OSDPretty::set_font(const QFont &font) {

  font_ = font;

  // Update the UI
  ui_->summary->setFont(font);
  ui_->message->setFont(font);
  // Now adjust OSD size so everything fits
  ui_->verticalLayout->activate();
  resize(sizeHint());
  // Update the position after font change
  Reposition();

}
