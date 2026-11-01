#ifndef QRCODEGEN_H
#define QRCODEGEN_H

#include <QString>
#include <QByteArray>
#include <QPixmap>
#include <QPainter>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace okj::qr {

// Compact ISO/IEC 18004 QR Code Generator (Byte Mode, ECC Level L, Versions 1..6)
class QrCode {
private:
    int m_version{1};
    int m_size{21};
    std::vector<uint8_t> m_modules;    // 0 = light, 1 = dark
    std::vector<uint8_t> m_isFunction; // 1 = function module (not masked)

    static uint8_t gfMul(uint8_t x, uint8_t y) {
        uint8_t z = 0;
        for (int i = 7; i >= 0; --i) {
            z = static_cast<uint8_t>((z << 1) ^ ((z >> 7) * 0x11D));
            z ^= ((y >> i) & 1) * x;
        }
        return z;
    }

    static std::vector<uint8_t> rsDivisor(int degree) {
        std::vector<uint8_t> result(degree, 0);
        result[degree - 1] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i) {
            for (int j = 0; j < degree; ++j) {
                result[j] = gfMul(result[j], root);
                if (j + 1 < degree)
                    result[j] ^= result[j + 1];
            }
            root = gfMul(root, 0x02);
        }
        return result;
    }

    static std::vector<uint8_t> rsRemainder(const std::vector<uint8_t> &data, int degree) {
        auto div = rsDivisor(degree);
        std::vector<uint8_t> result(degree, 0);
        for (uint8_t b : data) {
            uint8_t factor = b ^ result[0];
            result.erase(result.begin());
            result.push_back(0);
            for (int i = 0; i < degree; ++i)
                result[i] ^= gfMul(div[i], factor);
        }
        return result;
    }

    void setFunctionModule(int x, int y, bool isDark) {
        if (x < 0 || x >= m_size || y < 0 || y >= m_size)
            return;
        m_modules[y * m_size + x] = isDark ? 1 : 0;
        m_isFunction[y * m_size + x] = 1;
    }

    void drawFinderPattern(int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                int xx = cx + dx, yy = cy + dy;
                if (0 <= xx && xx < m_size && 0 <= yy && yy < m_size)
                    setFunctionModule(xx, yy, dist != 2 && dist != 4);
            }
        }
    }

    void drawAlignmentPattern(int cx, int cy) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                setFunctionModule(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
            }
        }
    }

    void drawFormatBits(int mask) {
        // ECC Level L = 01
        int data = (1 << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = ((data << 10) | rem) ^ 0x5412;

        for (int i = 0; i <= 5; ++i)
            setFunctionModule(8, i, ((bits >> i) & 1) != 0);
        setFunctionModule(8, 7, ((bits >> 6) & 1) != 0);
        setFunctionModule(8, 8, ((bits >> 7) & 1) != 0);
        setFunctionModule(7, 8, ((bits >> 8) & 1) != 0);
        for (int i = 9; i < 15; ++i)
            setFunctionModule(14 - i, 8, ((bits >> i) & 1) != 0);

        for (int i = 0; i < 8; ++i)
            setFunctionModule(m_size - 1 - i, 8, ((bits >> i) & 1) != 0);
        for (int i = 8; i < 15; ++i)
            setFunctionModule(8, m_size - 15 + i, ((bits >> i) & 1) != 0);
        setFunctionModule(8, m_size - 8, true);
    }

    void drawFunctionPatterns() {
        for (int i = 0; i < m_size; ++i) {
            setFunctionModule(6, i, i % 2 == 0);
            setFunctionModule(i, 6, i % 2 == 0);
        }
        drawFinderPattern(3, 3);
        drawFinderPattern(m_size - 4, 3);
        drawFinderPattern(3, m_size - 4);

        if (m_version >= 2) {
            int alignPos = m_size - 7;
            drawAlignmentPattern(alignPos, alignPos);
        }
        drawFormatBits(0);
    }

    void drawCodewords(const std::vector<uint8_t> &data) {
        std::size_t i = 0;
        for (int right = m_size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < m_size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    int x = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int y = upward ? (m_size - 1 - vert) : vert;
                    if (!m_isFunction[y * m_size + x] && i < data.size() * 8) {
                        bool dark = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                        m_modules[y * m_size + x] = dark ? 1 : 0;
                        ++i;
                    }
                }
            }
        }
    }

    void applyMask(int mask) {
        for (int y = 0; y < m_size; ++y) {
            for (int x = 0; x < m_size; ++x) {
                if (m_isFunction[y * m_size + x])
                    continue;
                bool invert = false;
                switch (mask) {
                    case 0: invert = (x + y) % 2 == 0; break;
                    case 1: invert = y % 2 == 0; break;
                    case 2: invert = x % 3 == 0; break;
                    case 3: invert = (x + y) % 3 == 0; break;
                    default: invert = (x + y) % 2 == 0; break;
                }
                if (invert)
                    m_modules[y * m_size + x] ^= 1;
            }
        }
    }

public:
    static QrCode encodeText(const QString &text) {
        QByteArray utf8 = text.toUtf8();
        // Version 1..6 capacities at ECC Level L (total data codewords, ec codewords per block, num blocks)
        struct VerInfo { int ver; int dataCw; int ecCwPerBlock; int numBlocks; };
        static constexpr std::array<VerInfo, 6> kVersions{{
            {1, 19,  7, 1},
            {2, 34, 10, 1},
            {3, 55, 15, 1},
            {4, 80, 20, 1},
            {5, 108, 26, 1},
            {6, 136, 18, 2}
        }};

        VerInfo chosen = kVersions.back();
        for (const auto &v : kVersions) {
            if (utf8.size() + 2 <= v.dataCw) {
                chosen = v;
                break;
            }
        }
        if (utf8.size() + 2 > chosen.dataCw) {
            utf8.truncate(chosen.dataCw - 2);
        }

        // Build bit stream: Byte mode (0100) + 8-bit length + bytes + terminator
        std::vector<uint8_t> bitBuf;
        auto appendBits = [&](uint32_t val, int len) {
            for (int i = len - 1; i >= 0; --i)
                bitBuf.push_back(static_cast<uint8_t>((val >> i) & 1));
        };
        appendBits(0x4, 4);
        appendBits(static_cast<uint32_t>(utf8.size()), 8);
        for (char c : utf8)
            appendBits(static_cast<uint8_t>(c), 8);

        int capacityBits = chosen.dataCw * 8;
        int termBits = std::min(4, capacityBits - static_cast<int>(bitBuf.size()));
        appendBits(0, termBits);
        while (bitBuf.size() % 8 != 0)
            bitBuf.push_back(0);

        std::vector<uint8_t> dataCw(chosen.dataCw, 0);
        for (std::size_t i = 0; i < bitBuf.size(); ++i)
            dataCw[i >> 3] |= static_cast<uint8_t>(bitBuf[i] << (7 - (i & 7)));

        uint8_t padByte = 0xEC;
        for (std::size_t i = bitBuf.size() / 8; i < static_cast<std::size_t>(chosen.dataCw); ++i) {
            dataCw[i] = padByte;
            padByte ^= (0xEC ^ 0x11);
        }

        std::vector<uint8_t> allCw;
        if (chosen.numBlocks == 1) {
            auto ec = rsRemainder(dataCw, chosen.ecCwPerBlock);
            allCw = dataCw;
            allCw.insert(allCw.end(), ec.begin(), ec.end());
        } else {
            int blockSize = chosen.dataCw / chosen.numBlocks;
            std::vector<uint8_t> b1(dataCw.begin(), dataCw.begin() + blockSize);
            std::vector<uint8_t> b2(dataCw.begin() + blockSize, dataCw.end());
            auto ec1 = rsRemainder(b1, chosen.ecCwPerBlock);
            auto ec2 = rsRemainder(b2, chosen.ecCwPerBlock);
            for (int i = 0; i < blockSize; ++i) {
                allCw.push_back(b1[i]);
                allCw.push_back(b2[i]);
            }
            for (int i = 0; i < chosen.ecCwPerBlock; ++i) {
                allCw.push_back(ec1[i]);
                allCw.push_back(ec2[i]);
            }
        }

        QrCode qr;
        qr.m_version = chosen.ver;
        qr.m_size = chosen.ver * 4 + 17;
        qr.m_modules.assign(qr.m_size * qr.m_size, 0);
        qr.m_isFunction.assign(qr.m_size * qr.m_size, 0);
        qr.drawFunctionPatterns();
        qr.drawCodewords(allCw);
        qr.applyMask(0);
        qr.drawFormatBits(0);
        return qr;
    }

    [[nodiscard]] int size() const { return m_size; }
    [[nodiscard]] bool module(int x, int y) const {
        if (x < 0 || x >= m_size || y < 0 || y >= m_size)
            return false;
        return m_modules[y * m_size + x] != 0;
    }
};

inline QPixmap generateQrCodePixmap(const QString &text, int pixelSize = 200, int borderModules = 2) {
    if (text.isEmpty() || pixelSize <= 0)
        return {};
    QrCode qr = QrCode::encodeText(text);
    int totalModules = qr.size() + borderModules * 2;
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::white);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);

    double scale = static_cast<double>(pixelSize) / totalModules;
    for (int y = 0; y < qr.size(); ++y) {
        for (int x = 0; x < qr.size(); ++x) {
            if (qr.module(x, y)) {
                double rx = (x + borderModules) * scale;
                double ry = (y + borderModules) * scale;
                painter.drawRect(QRectF(std::floor(rx), std::floor(ry),
                                        std::ceil(rx + scale) - std::floor(rx),
                                        std::ceil(ry + scale) - std::floor(ry)));
            }
        }
    }
    return pixmap;
}

} // namespace okj::qr

#endif // QRCODEGEN_H

