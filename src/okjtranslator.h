#ifndef OKJTRANSLATOR_H
#define OKJTRANSLATOR_H

#include <QTranslator>
#include <QHash>
#include <QString>

class OkjTranslator : public QTranslator {
    Q_OBJECT
public:
    explicit OkjTranslator(QObject *parent = nullptr);
    bool loadTs(const QString &filePath);
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override;
    bool isEmpty() const override;

private:
    QHash<QString, QHash<QString, QString>> m_contextTranslations;
    QHash<QString, QString> m_fallbackTranslations;
};

#endif // OKJTRANSLATOR_H
