/*
   Strawberry Music Player
   This file was part of Clementine.
   Copyright 2011, Tyler Rhodes <tyler.s.rhodes@gmail.com>
   Copyright 2011-2012, 2014, David Sansome <me@davidsansome.com>
   Copyright 2011, 2014, John Maguire <john.maguire@gmail.com>
   Copyright 2014, Alibek Omarov <a1ba.omarov@gmail.com>
   Copyright 2014, Krzysztof Sobiecki <sobkas@gmail.com>
   Copyright 2014-2015, Mark Furneaux <mark@furneaux.ca>
   Copyright 2015, Arun Narayanankutty <n.arun.lifescience@gmail.com>

   Strawberry is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Strawberry is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "rainbowanalyzer.h"

#include <cmath>

#include <QtGlobal>
#include <QWidget>
#include <QPixmap>
#include <QPainter>
#include <QColor>
#include <QBrush>
#include <QPen>
#include <QPoint>
#include <QSize>
#include <QTimerEvent>

#include "fht.h"
#include "analyzerbase.h"

using namespace Qt::Literals::StringLiterals;

const char *NyanCatAnalyzer::kName = "Nyanalyzer Cat";
const char *RainbowDashAnalyzer::kName = "Rainbow Dash";

RainbowAnalyzer::RainbowType RainbowAnalyzer::rainbowtype;
const int RainbowAnalyzer::kHeight[] = { 21, 33 };
const int RainbowAnalyzer::kWidth[] = { 34, 53 };
const int RainbowAnalyzer::kFrameCount[] = { 6, 16 };
const int RainbowAnalyzer::kSleepingHeight[] = { 24, 33 };

namespace {
constexpr int kFrameIntervalMs = 150;
constexpr int kRainbowHeight[] = { 21, 16 };
constexpr int kRainbowOverlap[] = { 13, 15 };
constexpr float kPixelScale = 0.02F;
}  // namespace

RainbowAnalyzer::RainbowAnalyzer(const RainbowType rbtype, QWidget *parent)
    : AnalyzerBase(parent, 9),
      timer_id_(startTimer(kFrameIntervalMs)),
      frame_(0),
      current_buffer_(0),
      available_rainbow_width_(0),
      px_per_frame_(0),
      x_offset_(0),
      background_brush_(QColor(0x0f, 0x43, 0x73)) {

  rainbowtype = rbtype;
  cat_dash_[0] = QPixmap(u":/pictures/nyancat.png"_s);
  cat_dash_[1] = QPixmap(u":/pictures/rainbowdash.png"_s);
  memset(history_, 0, sizeof(history_));

  for (int i = 0; i < kRainbowBands; ++i) {
    colors_[i] = QPen(QColor::fromHsv(i * 255 / kRainbowBands, 255, 255), kRainbowHeight[rainbowtype] / kRainbowBands, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);  // NOLINT(bugprone-integer-division)

    // pow constants computed so that
    // | band_scale(0) | ~= .5 and | band_scale(5) | ~= 32
    band_scale_[i] = -static_cast<float>(std::cos(M_PI * i / (kRainbowBands - 1))) * 0.5F * static_cast<float>(std::pow(2.3, i));
  }

}

void RainbowAnalyzer::transform(Scope &s) { fht_->spectrum(s.data()); }

void RainbowAnalyzer::timerEvent(QTimerEvent *e) {

  if (e->timerId() == timer_id_) {
    frame_ = (frame_ + 1) % kFrameCount[rainbowtype];
  }
  else {
    AnalyzerBase::timerEvent(e);
  }

}

void RainbowAnalyzer::resizeEvent(QResizeEvent *e) {

  Q_UNUSED(e);

  // Invalidate the buffer so it's recreated from scratch in the next paint event.
  buffer_[0] = QPixmap();
  buffer_[1] = QPixmap();

  available_rainbow_width_ = width() - kWidth[rainbowtype] + kRainbowOverlap[rainbowtype];
  px_per_frame_ = available_rainbow_width_ / (kHistorySize - 1) + 1;
  x_offset_ = px_per_frame_ * (kHistorySize - 1) - available_rainbow_width_;

}

void RainbowAnalyzer::analyze(QPainter &p, const Scope &s, const bool new_frame) {

  // Discard the second half of the transform
  const int scope_size = static_cast<int>(s.size() / 2);

  if ((new_frame && is_playing_) || (buffer_[0].isNull() && buffer_[1].isNull())) {
    // Transform the music into rainbows!
    for (int band = 0; band < kRainbowBands; ++band) {
      float *band_start = history_ + band * kHistorySize;

      // Move the history of each band across by 1 frame.
      memmove(band_start, band_start + 1, (kHistorySize - 1) * sizeof(float));
    }

    // Now accumulate the scope data into each band.  Should maybe use a series
    // of band pass filters for this, so bands can leak into neighbouring bands,
    // but for now it's a series of separate square filters.
    const int samples_per_band = scope_size / kRainbowBands;
    size_t sample = 0;
    for (int band = 0; band < kRainbowBands; ++band) {
      float accumulator = 0.0;
      for (int i = 0; i < samples_per_band; ++i) {
        accumulator += s[sample++];
      }

      history_[(band + 1) * kHistorySize - 1] = accumulator * band_scale_[band];
    }

    // Create polylines for the rainbows.
    QPointF polyline[kRainbowBands * kHistorySize];
    QPointF *dest = polyline;
    float *source = history_;

    const float top_of = static_cast<float>(height()) / 2 - static_cast<float>(kRainbowHeight[rainbowtype]) / 2;
    for (int band = 0; band < kRainbowBands; ++band) {
      // Calculate the Y position of this band.
      const float y = static_cast<float>(kRainbowHeight[rainbowtype]) / static_cast<float>(kRainbowBands + 1) * (static_cast<float>(band) + 0.5F) + top_of;

      // Add each point in the line.
      for (int x = 0; x < kHistorySize; ++x) {
        *dest = QPointF(px_per_frame_ * x, y + *source * kPixelScale);
        ++dest;
        ++source;
      }
    }

    // Do we have to draw the whole rainbow into the buffer?
    if (buffer_[0].isNull()) {
      for (int i = 0; i < 2; ++i) {
        buffer_[i] = QPixmap(QSize(width() + x_offset_, height()));
        buffer_[i].fill(background_brush_.color());
      }
      current_buffer_ = 0;

      QPainter buffer_painter(&buffer_[0]);
      buffer_painter.setRenderHint(QPainter::Antialiasing);
      for (int band = kRainbowBands - 1; band >= 0; --band) {
        buffer_painter.setPen(colors_[band]);
        buffer_painter.drawPolyline(&polyline[band * kHistorySize], kHistorySize);
        buffer_painter.drawPolyline(&polyline[band * kHistorySize], kHistorySize);
      }
    }
    else {
      const int last_buffer = current_buffer_;
      current_buffer_ = (current_buffer_ + 1) % 2;

      // We can just shuffle the buffer along a bit and draw the new frame's data.
      QPainter buffer_painter(&buffer_[current_buffer_]);
      buffer_painter.setRenderHint(QPainter::Antialiasing);

      buffer_painter.drawPixmap(0, 0, buffer_[last_buffer], px_per_frame_, 0, x_offset_ + available_rainbow_width_ - px_per_frame_, 0);
      buffer_painter.fillRect(x_offset_ + available_rainbow_width_ - px_per_frame_, 0, kWidth[rainbowtype] - kRainbowOverlap[rainbowtype] + px_per_frame_, height(), background_brush_);

      for (int band = kRainbowBands - 1; band >= 0; --band) {
        buffer_painter.setPen(colors_[band]);
        buffer_painter.drawPolyline(&polyline[(band + 1) * kHistorySize - 3], 3);
      }
    }
  }

  // Draw the buffer on to the widget
  p.drawPixmap(0, 0, buffer_[current_buffer_], x_offset_, 0, 0, 0);

  // Draw rainbow analyzer (nyan cat or rainbowdash)
  // Nyan nyan nyan nyan dash dash dash dash.
  if (!is_playing_) {
    // Ssshhh!
    p.drawPixmap(SleepingDestRect(rainbowtype), cat_dash_[rainbowtype], SleepingSourceRect(rainbowtype));
  }
  else {
    p.drawPixmap(DestRect(rainbowtype), cat_dash_[rainbowtype], SourceRect(rainbowtype));
  }

}

NyanCatAnalyzer::NyanCatAnalyzer(QWidget *parent)
    : RainbowAnalyzer(RainbowAnalyzer::Nyancat, parent) {}

RainbowDashAnalyzer::RainbowDashAnalyzer(QWidget *parent)
    : RainbowAnalyzer(RainbowAnalyzer::Dash, parent) {}
