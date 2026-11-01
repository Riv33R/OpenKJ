#include "okjtranslator.h"
#include <QFile>
#include <QXmlStreamReader>

OkjTranslator::OkjTranslator(QObject *parent) : QTranslator(parent) {
}

bool OkjTranslator::loadTs(const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QXmlStreamReader xml(&file);
    QString currentContext;
    QString currentSource;
    QString currentTranslation;
    bool inMessage = false;

    while (!xml.atEnd() && !xml.hasError()) {
        QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringRef name = xml.name();
            if (name == QLatin1String("context")) {
                currentContext.clear();
            } else if (name == QLatin1String("name") && !inMessage) {
                currentContext = xml.readElementText();
            } else if (name == QLatin1String("message")) {
                inMessage = true;
                currentSource.clear();
                currentTranslation.clear();
            } else if (inMessage && name == QLatin1String("source")) {
                currentSource = xml.readElementText();
            } else if (inMessage && name == QLatin1String("translation")) {
                currentTranslation = xml.readElementText();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (xml.name() == QLatin1String("message")) {
                inMessage = false;
                if (!currentSource.isEmpty() && !currentTranslation.isEmpty()) {
                    if (!currentContext.isEmpty()) {
                        m_contextTranslations[currentContext].insert(currentSource, currentTranslation);
                    }
                    if (!m_fallbackTranslations.contains(currentSource)) {
                        m_fallbackTranslations.insert(currentSource, currentTranslation);
                    }
                }
            }
        }
    }

    return !m_fallbackTranslations.isEmpty();
}

QString OkjTranslator::translate(const char *context, const char *sourceText,
                                 const char *disambiguation, int n) const {
    Q_UNUSED(disambiguation)
    Q_UNUSED(n)
    if (!sourceText || sourceText[0] == '\0') {
        return QString();
    }

    const QString src = QString::fromUtf8(sourceText);
    if (context && context[0] != '\0') {
        const QString ctx = QString::fromUtf8(context);
        auto ctxIt = m_contextTranslations.constFind(ctx);
        if (ctxIt != m_contextTranslations.constEnd()) {
            auto trIt = ctxIt->constFind(src);
            if (trIt != ctxIt->constEnd()) {
                return trIt.value();
            }
        }
    }

    auto fbIt = m_fallbackTranslations.constFind(src);
    if (fbIt != m_fallbackTranslations.constEnd()) {
        return fbIt.value();
    }

    return QString();
}

bool OkjTranslator::isEmpty() const {
    return m_fallbackTranslations.isEmpty();
}
