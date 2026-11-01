#include "videodisplay.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QSvgRenderer>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QRadialGradient>
#include <cmath>
#include <algorithm>

namespace {
constexpr int kNumBars = 32;
constexpr int kNumParticles = 28;
}

VideoDisplay::VideoDisplay(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    auto palette = this->palette();
    palette.setColor(QPalette::Window, Qt::black);
    setPalette(palette);
    setMouseTracking(true);

    initVisualizerState();
    m_animTimer.setInterval(33);
    connect(&m_animTimer, &QTimer::timeout, this, &VideoDisplay::stepAnimation);
}

void VideoDisplay::initVisualizerState()
{
    m_targetBands.fill(0.0, kNumBars);
    m_smoothBands.fill(0.0, kNumBars);
    m_peakBands.fill(0.0, kNumBars);
    m_peakVel.fill(0.0, kNumBars);

    m_particles.resize(kNumParticles);
    for (int i = 0; i < kNumParticles; ++i)
    {
        Particle p;
        p.x = std::fmod((i * 0.61803398875), 1.0);
        p.y = static_cast<double>(i) / kNumParticles;
        p.radius = 2.5 + (i % 5) * 1.8;
        p.speedY = 0.0018 + (i % 4) * 0.0009;
        p.phase = i * 0.7;
        p.hue = 185 + (i * 11) % 130; // cyan -> violet -> magenta
        p.alpha = 55 + (i * 17) % 90;
        m_particles[i] = p;
    }
}

void VideoDisplay::setBackground(const QPixmap &pixmap)
{
    m_useDefaultBg = false;
    m_currentBg = pixmap;
    update();
}

void VideoDisplay::useDefaultBackground()
{
    m_useDefaultBg = true;
    update();
}

void VideoDisplay::setHasActiveVideo(const bool &value)
{
    if (m_hasActiveVideo != value)
    {
        if (value)
        {
            // Switching to having video.
            // Make sure the background is painted black initially to prevent
            // any old buffer from "bleeding though" when in HW mode.
            m_repaintBackgroundOnce = true;
            m_animTimer.stop();
        }
        else if (m_visualizerActive)
        {
            m_animTimer.start();
        }
        m_hasActiveVideo = value;
        update();
    }
}

void VideoDisplay::setVisualizerActive(bool active)
{
    if (m_visualizerActive == active)
        return;
    m_visualizerActive = active;
    if (m_visualizerActive && !m_hasActiveVideo)
    {
        m_animTimer.start();
    }
    else
    {
        m_animTimer.stop();
        m_targetBands.fill(0.0, kNumBars);
        m_smoothBands.fill(0.0, kNumBars);
        m_peakBands.fill(0.0, kNumBars);
        m_peakVel.fill(0.0, kNumBars);
        m_targetRms = 0.0;
        m_smoothRms = 0.0;
        m_smoothBass = 0.0;
    }
    update();
}

void VideoDisplay::setAudioSpectrum(const QVector<double> &bands, double rmsLevel)
{
    if (bands.size() == kNumBars)
    {
        m_targetBands = bands;
    }
    else if (!bands.isEmpty())
    {
        m_targetBands.resize(kNumBars);
        for (int i = 0; i < kNumBars; ++i)
        {
            int idx = std::clamp(static_cast<int>((static_cast<double>(i) / kNumBars) * bands.size()), 0, bands.size() - 1);
            m_targetBands[i] = bands[idx];
        }
    }
    m_targetRms = std::clamp(rmsLevel, 0.0, 1.0);
}

void VideoDisplay::setNowPlayingTrack(const QString &artist, const QString &title)
{
    m_nowPlayingArtist = artist.trimmed();
    m_nowPlayingTitle = title.trimmed();
    update();
}

void VideoDisplay::stepAnimation()
{
    if (!m_visualizerActive || m_hasActiveVideo)
        return;

    m_phase += 0.065;
    if (m_phase > 628.3185)
        m_phase -= 628.3185;

    m_smoothRms += (m_targetRms - m_smoothRms) * 0.25;
    // Decay target slightly in case GStreamer spectrum messages pause between frames
    m_targetRms *= 0.94;

    double bassAccum = 0.0;
    const int bassCount = std::min(6, kNumBars);

    for (int i = 0; i < kNumBars; ++i)
    {
        double idleWave = 0.04 + 0.03 * std::sin(m_phase * 1.3 + i * 0.32)
                               + 0.02 * std::cos(m_phase * 0.7 - i * 0.19);
        double target = std::max(m_targetBands[i], idleWave);
        m_targetBands[i] *= 0.90;

        double cur = m_smoothBands[i];
        if (target > cur)
            cur += (target - cur) * 0.55; // fast attack
        else
            cur += (target - cur) * 0.16; // smooth release
        cur = std::clamp(cur, 0.0, 1.0);
        m_smoothBands[i] = cur;

        if (cur >= m_peakBands[i])
        {
            m_peakBands[i] = cur;
            m_peakVel[i] = 0.0;
        }
        else
        {
            m_peakVel[i] += 0.0022;
            m_peakBands[i] = std::max(cur, m_peakBands[i] - m_peakVel[i]);
        }

        if (i < bassCount)
            bassAccum += cur;
    }

    double targetBass = (bassCount > 0) ? (bassAccum / bassCount) : m_smoothRms;
    m_smoothBass += (targetBass - m_smoothBass) * 0.30;

    double speedBoost = 1.0 + m_smoothBass * 2.4;
    for (auto &p : m_particles)
    {
        p.y -= p.speedY * speedBoost;
        p.x += 0.0007 * std::sin(m_phase * 0.8 + p.phase);
        if (p.y < -0.05)
        {
            p.y = 1.05;
            p.x = std::fmod(p.x + 0.37, 1.0);
        }
        if (p.x < 0.0) p.x += 1.0;
        if (p.x > 1.0) p.x -= 1.0;
    }

    update();
}

void VideoDisplay::paintVisualizer(QPainter &painter, const QRect &rect)
{
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform, true);

    const double w = rect.width();
    const double h = rect.height();
    if (w <= 10 || h <= 10)
        return;

    const double minDim = std::min(w, h);

    // 1. Background layer
    if (!m_useDefaultBg && !m_currentBg.isNull())
    {
        painter.drawPixmap(rect, m_currentBg, m_currentBg.rect());
        painter.fillRect(rect, QColor(8, 10, 24, 195));
    }
    else
    {
        QLinearGradient bgGrad(0, 0, 0, h);
        bgGrad.setColorAt(0.0, QColor(7, 9, 20));
        bgGrad.setColorAt(0.5, QColor(15, 18, 40));
        bgGrad.setColorAt(1.0, QColor(6, 7, 16));
        painter.fillRect(rect, bgGrad);
    }

    // Ambient bass-reactive radial glow in the center
    {
        QRadialGradient glowGrad(QPointF(w * 0.5, h * 0.42), minDim * (0.42 + 0.14 * m_smoothBass));
        int glowAlpha = static_cast<int>(std::clamp(45.0 + m_smoothBass * 95.0, 40.0, 145.0));
        glowGrad.setColorAt(0.0, QColor(124, 58, 237, glowAlpha));
        glowGrad.setColorAt(0.45, QColor(6, 182, 212, glowAlpha / 2));
        glowGrad.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glowGrad);
        painter.drawRect(rect);
    }

    // 2. Floating bokeh particles
    painter.setPen(Qt::NoPen);
    for (const auto &p : m_particles)
    {
        double px = p.x * w;
        double py = p.y * h;
        double pr = p.radius * (minDim / 480.0) * (0.85 + 0.45 * m_smoothBass);
        int alpha = std::clamp(static_cast<int>(p.alpha * (0.65 + 0.55 * m_smoothRms)), 20, 190);

        QRadialGradient pGrad(QPointF(px, py), pr * 2.2);
        QColor c = QColor::fromHsv(p.hue % 360, 200, 255, alpha);
        pGrad.setColorAt(0.0, c);
        pGrad.setColorAt(0.4, QColor::fromHsv(p.hue % 360, 220, 255, alpha / 2));
        pGrad.setColorAt(1.0, QColor(0, 0, 0, 0));
        painter.setBrush(pGrad);
        painter.drawEllipse(QPointF(px, py), pr * 2.2, pr * 2.2);
    }

    // 3. Flowing neon audio waves across the middle
    const double waveBaseY = h * 0.54;
    for (int layer = 0; layer < 3; ++layer)
    {
        QPainterPath wavePath;
        const int steps = 48;
        QColor waveColor = (layer == 0) ? QColor(0, 229, 255, 110)
                         : (layer == 1) ? QColor(168, 85, 247, 95)
                                        : QColor(244, 63, 94, 80);
        double freq = 1.6 + layer * 0.55;
        double phaseShift = m_phase * (1.1 - layer * 0.25) + layer * 1.4;
        double amp = (h * 0.035) * (0.45 + 1.15 * m_smoothBass) * (1.0 - layer * 0.18);

        for (int s = 0; s <= steps; ++s)
        {
            double t = static_cast<double>(s) / steps;
            int bandIdx = std::clamp(static_cast<int>(t * (kNumBars - 1)), 0, kNumBars - 1);
            double bandMod = 0.5 + 0.8 * m_smoothBands[bandIdx];
            double x = t * w;
            double y = waveBaseY + std::sin(t * 6.28318 * freq + phaseShift) * amp * bandMod;
            if (s == 0)
                wavePath.moveTo(x, y);
            else
                wavePath.lineTo(x, y);
        }
        QPen wavePen(waveColor, std::max(1.5, minDim * 0.0045));
        wavePen.setCapStyle(Qt::RoundCap);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(wavePen);
        painter.drawPath(wavePath);
    }

    // 4. Upper section: Pulsing Music Emblem & "Now Playing" card
    const bool hasTrackText = !m_nowPlayingTitle.isEmpty() || !m_nowPlayingArtist.isEmpty();
    const double centerX = w * 0.5;
    const double emblemY = hasTrackText ? (h * 0.21) : (h * 0.30);
    const double baseRadius = minDim * (hasTrackText ? 0.095 : 0.13);
    const double pulseRadius = baseRadius * (1.0 + 0.18 * m_smoothBass);

    // Outer pulsing neon rings
    for (int r = 2; r >= 0; --r)
    {
        double ringR = pulseRadius * (1.0 + r * 0.18);
        int ringAlpha = std::clamp(static_cast<int>((150 - r * 45) * (0.55 + 0.65 * m_smoothBass)), 25, 220);
        QPen ringPen(QColor(0, 229, 255, ringAlpha), std::max(1.5, minDim * 0.004));
        painter.setPen(ringPen);
        painter.setBrush(r == 0 ? QColor(15, 23, 42, 200) : Qt::NoBrush);
        painter.drawEllipse(QPointF(centerX, emblemY), ringR, ringR);
    }

    // Render OpenKJ SVG logo or stylized music note inside the pulsing disc
    {
        QSvgRenderer renderer(QString(":icons/Icons/okjlogo.svg"));
        if (renderer.isValid())
        {
#if (QT_VERSION >= QT_VERSION_CHECK(5,15,0))
            renderer.setAspectRatioMode(Qt::KeepAspectRatio);
#endif
            double logoSize = pulseRadius * 1.35;
            QRectF logoRect(centerX - logoSize * 0.5, emblemY - logoSize * 0.5, logoSize, logoSize);
            renderer.render(&painter, logoRect);
        }
    }

    // "Now Playing" Glassmorphism Card
    if (hasTrackText)
    {
        double cardWidth = std::min(w * 0.84, std::max(w * 0.52, minDim * 0.95));
        double cardHeight = std::max(44.0, h * 0.145);
        double cardX = (w - cardWidth) * 0.5;
        double cardY = h * 0.36;
        QRectF cardRect(cardX, cardY, cardWidth, cardHeight);

        QPainterPath cardPath;
        double cornerRadius = std::min(18.0, cardHeight * 0.28);
        cardPath.addRoundedRect(cardRect, cornerRadius, cornerRadius);

        painter.setPen(QPen(QColor(56, 189, 248, 120), std::max(1.0, minDim * 0.0025)));
        painter.setBrush(QColor(13, 18, 36, 195));
        painter.drawPath(cardPath);

        int titlePt = std::clamp(static_cast<int>(cardHeight * 0.24), 9, 28);
        int subPt = std::clamp(static_cast<int>(cardHeight * 0.17), 8, 20);

        QFont titleFont = painter.font();
        titleFont.setPointSize(titlePt);
        titleFont.setBold(true);

        QFont subFont = painter.font();
        subFont.setPointSize(subPt);
        subFont.setBold(false);

        QString displayTitle = m_nowPlayingTitle.isEmpty() ? m_nowPlayingArtist : m_nowPlayingTitle;
        QString displaySubtitle = (!m_nowPlayingTitle.isEmpty() && !m_nowPlayingArtist.isEmpty())
                                ? m_nowPlayingArtist
                                : tr("Фоновая музыка");

        QRectF innerRect = cardRect.adjusted(16, 6, -16, -6);
        painter.setFont(titleFont);
        QFontMetrics fmTitle(titleFont);
        QString elidedTitle = fmTitle.elidedText("♫  " + displayTitle, Qt::ElideRight, static_cast<int>(innerRect.width()));

        painter.setPen(QColor(248, 250, 252));
        QRectF topHalf(innerRect.left(), innerRect.top(), innerRect.width(), innerRect.height() * 0.56);
        painter.drawText(topHalf, Qt::AlignHCenter | Qt::AlignVCenter, elidedTitle);

        painter.setFont(subFont);
        QFontMetrics fmSub(subFont);
        QString elidedSub = fmSub.elidedText(displaySubtitle, Qt::ElideRight, static_cast<int>(innerRect.width()));
        painter.setPen(QColor(125, 211, 252));
        QRectF bottomHalf(innerRect.left(), innerRect.top() + innerRect.height() * 0.52, innerRect.width(), innerRect.height() * 0.48);
        painter.drawText(bottomHalf, Qt::AlignHCenter | Qt::AlignVCenter, elidedSub);
    }

    // 5. 32-Band Neon Spectrum Equalizer + Reflection
    const double marginX = w * 0.07;
    const double availW = w - 2.0 * marginX;
    const double stepX = availW / kNumBars;
    const double barW = std::max(2.0, stepX * 0.66);
    const double baselineY = h * 0.88;
    const double maxBarH = h * 0.31;
    const double maxReflectH = h * 0.08;

    QLinearGradient barGrad(0, baselineY, 0, baselineY - maxBarH);
    barGrad.setColorAt(0.0, QColor(6, 182, 212));    // Cyan
    barGrad.setColorAt(0.5, QColor(139, 92, 246));   // Violet
    barGrad.setColorAt(0.85, QColor(236, 72, 153));  // Pink/Magenta
    barGrad.setColorAt(1.0, QColor(250, 204, 21));   // Warm Gold peak

    QLinearGradient reflectGrad(0, baselineY, 0, baselineY + maxReflectH);
    reflectGrad.setColorAt(0.0, QColor(6, 182, 212, 70));
    reflectGrad.setColorAt(1.0, QColor(6, 182, 212, 0));

    painter.setPen(Qt::NoPen);
    const double radius = std::min(barW * 0.45, 5.0);

    for (int i = 0; i < kNumBars; ++i)
    {
        double val = m_smoothBands[i];
        double barH = std::max(4.0, val * maxBarH);
        double x = marginX + i * stepX + (stepX - barW) * 0.5;
        double y = baselineY - barH;

        // Main bar
        painter.setBrush(barGrad);
        painter.drawRoundedRect(QRectF(x, y, barW, barH), radius, radius);

        // Peak-hold cap
        double peakH = std::max(barH, m_peakBands[i] * maxBarH);
        double capH = std::max(2.0, minDim * 0.006);
        double capY = baselineY - peakH - capH - 2.0;
        painter.setBrush(QColor(224, 242, 254, 230));
        painter.drawRoundedRect(QRectF(x, capY, barW, capH), radius * 0.5, radius * 0.5);

        // Reflection bar below baseline
        double refH = std::max(2.0, val * maxReflectH);
        painter.setBrush(reflectGrad);
        painter.drawRoundedRect(QRectF(x, baselineY + 2.0, barW, refH), radius * 0.5, radius * 0.5);
    }

    // Subtle glowing horizon line at baseline
    painter.setPen(QPen(QColor(56, 189, 248, 95), 1.2));
    painter.drawLine(QPointF(marginX, baselineY + 1.0), QPointF(w - marginX, baselineY + 1.0));
}

void VideoDisplay::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);

    if (m_hasActiveVideo)
    {
        // playing - fix black bars in hw mode (event is not called in sw mode when playing).
        // (perhaps simplify all this by always using fillOnPaint. Performance should be measured though.)
        if (m_fillOnPaint || m_repaintBackgroundOnce)
        {
            painter.fillRect(event->rect(), Qt::black);
            m_repaintBackgroundOnce = false;
        }
    }
    else if (m_visualizerActive)
    {
        paintVisualizer(painter, rect());
    }
    else
    {
        // stopped - draw background image
        painter.fillRect(event->rect(), Qt::black);
        if (m_useDefaultBg)
        {
            QSvgRenderer renderer(QString(":icons/Icons/okjlogo.svg"));
#if (QT_VERSION >= QT_VERSION_CHECK(5,15,0))
            renderer.setAspectRatioMode(Qt::KeepAspectRatio);
#endif
            renderer.render(&painter);
        }
        else
        {
            painter.drawPixmap(rect(), m_currentBg, m_currentBg.rect());
        }
    }
}
