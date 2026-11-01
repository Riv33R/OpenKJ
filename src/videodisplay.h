#ifndef CDGDISPLAY_H
#define CDGDISPLAY_H

#include <QWidget>
#include <QBoxLayout>
#include <QResizeEvent>
#include <QTimer>
#include <QVector>
#include <QString>
#include <QSvgRenderer>

class VideoDisplay : public QWidget
{
    Q_OBJECT
private:
    struct Particle {
        double x{0.0};
        double y{0.0};
        double radius{3.0};
        double speedY{0.002};
        double phase{0.0};
        int hue{190};
        int alpha{100};
    };

    QPixmap m_currentBg;
    bool m_useDefaultBg{true};
    bool m_hasActiveVideo{false};
    bool m_fillOnPaint{false};
    bool m_repaintBackgroundOnce{false};

    bool m_visualizerActive{false};
    QTimer m_animTimer;
    QVector<double> m_targetBands;
    QVector<double> m_smoothBands;
    QVector<double> m_peakBands;
    QVector<double> m_peakVel;
    QVector<Particle> m_particles;
    double m_targetRms{0.0};
    double m_smoothRms{0.0};
    double m_smoothBass{0.0};
    double m_phase{0.0};
    QString m_nowPlayingArtist;
    QString m_nowPlayingTitle;
    QSvgRenderer m_logoRenderer;

    void initVisualizerState();
    void stepAnimation();
    void paintVisualizer(QPainter &painter, const QRect &rect);

public:
    explicit VideoDisplay(QWidget *parent = nullptr);
    [[nodiscard]] bool hasActiveVideo() const { return m_hasActiveVideo; }

signals:
    void mouseMoveEvent(QMouseEvent *event) override;

public slots:
    void setBackground(const QPixmap &pixmap);
    void useDefaultBackground();
    void setHasActiveVideo(const bool &value);
    void setVisualizerActive(bool active);
    void setAudioSpectrum(const QVector<double> &bands, double rmsLevel);
    void setNowPlayingTrack(const QString &artist, const QString &title);

    /**
     * @brief Fill with black on paint event when video is playing.
     * Video in HW mode seems to only paint black borders when the control is resized.
     * This causes some glitches in the monitor window when changing between tabs.
     * Set this property to start each paint event with a black fill.
     */
    void setFillOnPaint(const bool &value) { m_fillOnPaint = value; }
protected:
    void paintEvent(QPaintEvent *event) override;
};


#endif // CDGDISPLAY_H
