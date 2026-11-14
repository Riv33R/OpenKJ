#include "cdgimageframe.h"
#include <cstring>
#include <vector>


CdgImageFrame::CdgImageFrame()
{
    m_image = QImage(cdg::FRAME_DIM_FULL, QImage::Format_Indexed8);

    m_curHOffset = 0;
    m_curVOffset = 0;
    QVector<QRgb> palette;
    for (int i=0; i < 16; i++)
        palette.append(QColor(0,0,0).rgb());
    m_image = QImage(cdg::FRAME_DIM_FULL, QImage::Format_Indexed8);
    m_bytesPerPixel = m_image.pixelFormat().bitsPerPixel() / 8;
    m_borderLRBytes = m_bytesPerPixel * 6;
    m_borderRBytesOffset = 294 * m_bytesPerPixel;
    m_image.setColorTable(palette);
    m_image.fill(0);
}

bool CdgImageFrame::applySubCode(const cdg::CDG_SubCode &subCode)
{
    constexpr static char SUBCODE_MASK = 0x3F;
    constexpr static char SUBCODE_COMMAND = 0x09;

    if ((subCode.command & SUBCODE_MASK) != SUBCODE_COMMAND)
        return false;

    bool updated{false};
    switch (subCode.instruction & SUBCODE_MASK)
    {
        case cdg::CmdMemoryPreset:
            updated = cmdMemoryPreset(cdg::CdgMemoryPresetData(subCode.data));
            break;
        case cdg::CmdBorderPreset:
            cmdBorderPreset(cdg::CdgBorderPresetData(subCode.data));
            updated = true;
            break;
        case cdg::CmdTileBlock:
            cmdTileBlock(cdg::CdgTileBlockData(subCode.data), cdg::TileBlockNormal);
            updated = true;
            break;
        case cdg::CmdScrollPreset:
            cmdScroll(subCode.data, cdg::ScrollPreset);
            updated = true;
            break;
        case cdg::CmdScrollCopy:
            cmdScroll(subCode.data, cdg::ScrollCopy);
            updated = true;
            break;
        case cdg::CmdDefineTrans:
            cmdDefineTransparent(subCode.data);
            break;
        case cdg::CmdColorsLow:
            updated = cmdColors(cdg::CdgColorsData(subCode.data), cdg::LowColors);
            break;
        case cdg::CmdColorsHigh:
            updated = cmdColors(cdg::CdgColorsData(subCode.data), cdg::HighColors);
            break;
        case cdg::CmdTileBlockXOR:
            cmdTileBlock(cdg::CdgTileBlockData(subCode.data), cdg::TileBlockXOR);
            updated = true;
            break;
    }
    m_lastCmdWasMempreset = (subCode.instruction == cdg::CmdMemoryPreset);

    return updated;
}

void CdgImageFrame::copyCroppedImagedata(uchar *destbuffer)
{
    uchar* src = m_image.bits();
    uchar* destpos = destbuffer;

    for (auto y=0; y < cdg::FRAME_DIM_CROPPED.height(); y++)
    {
        auto curSrcLineNum = y + m_curVOffset;
        auto srcLineOffset = m_image.bytesPerLine() * (12 + curSrcLineNum); // 12?

        memcpy(destpos, src + srcLineOffset + m_borderLRBytes + m_curHOffset, cdg::FRAME_DIM_CROPPED.width());
        destpos += cdg::FRAME_DIM_CROPPED.width();
    }

    // copy color table
    memcpy(destpos, m_image.colorTable().data(), m_image.colorTable().length() * sizeof(uint));

}

void CdgImageFrame::cmdBorderPreset(const cdg::CdgBorderPresetData &borderPreset)
{
    // Is there a safer C++ way to do these memory copies?
    if (borderPreset.color >= 16)
        return;
    for (auto line=0; line < 216; line++)
    {
        if (line < 12 || line > 202)
            memset(m_image.scanLine(line), borderPreset.color, m_image.bytesPerLine());
        else
        {
            memset(m_image.scanLine(line), borderPreset.color, m_borderLRBytes);
            memset(m_image.scanLine(line) + m_borderRBytesOffset, borderPreset.color, m_borderLRBytes);
        }
    }
}

bool CdgImageFrame::cmdColors(const cdg::CdgColorsData &data, const cdg::CdgColorTables &table)
{
    bool changed{false};
    int curColor = (table == cdg::HighColors) ? 8 : 0;
    std::for_each(data.colors.begin(), data.colors.end(), [&] (auto color) {
        if (m_image.colorTable().at(curColor) != color.rgb())
        {
            changed = true;
            m_image.setColor(curColor, color.rgb());
        }
        curColor++;
    });
    return changed;
}


bool CdgImageFrame::cmdMemoryPreset(const cdg::CdgMemoryPresetData &memoryPreset)
{
    // reject out of range value from corrupted CDG packets
    if (memoryPreset.color >= 16)
        return false;
    if (m_lastCmdWasMempreset && memoryPreset.repeat)
    {
        return false;
    }
    m_image.fill(memoryPreset.color);
    return true;
}


void CdgImageFrame::cmdTileBlock(const cdg::CdgTileBlockData &tileBlockPacket, const cdg::TileBlockType &type)
{
    constexpr static std::array<char,6> MASKS{0x20,0x10,0x08,0x04,0x02,0x01};

    // reject corrupted CDG packets w/ invalid row/column
    if (tileBlockPacket.row >= 18 || tileBlockPacket.column >= 50 || tileBlockPacket.color0 >= 16 || tileBlockPacket.color1 >= 16)
        return;

    // There's probably a better way to do this, needs research
    for (auto y = 0; y < 12; y++)
    {
        auto ptr = m_image.scanLine(y + tileBlockPacket.top);
        auto rowData = tileBlockPacket.tilePixels[y];
        switch (type) {
        case cdg::TileBlockXOR:
            *(ptr + (tileBlockPacket.left * m_bytesPerPixel))       ^= (rowData & MASKS[0]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 1) * m_bytesPerPixel)) ^= (rowData & MASKS[1]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 2) * m_bytesPerPixel)) ^= (rowData & MASKS[2]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 3) * m_bytesPerPixel)) ^= (rowData & MASKS[3]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 4) * m_bytesPerPixel)) ^= (rowData & MASKS[4]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 5) * m_bytesPerPixel)) ^= (rowData & MASKS[5]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            break;
        case cdg::TileBlockNormal:
            *(ptr + (tileBlockPacket.left * m_bytesPerPixel))       = (rowData & MASKS[0]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 1) * m_bytesPerPixel)) = (rowData & MASKS[1]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 2) * m_bytesPerPixel)) = (rowData & MASKS[2]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 3) * m_bytesPerPixel)) = (rowData & MASKS[3]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 4) * m_bytesPerPixel)) = (rowData & MASKS[4]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            *(ptr + ((tileBlockPacket.left + 5) * m_bytesPerPixel)) = (rowData & MASKS[5]) ? tileBlockPacket.color1 : tileBlockPacket.color0;
            break;
        }
    }
}

void CdgImageFrame::cmdScroll(const cdg::CdgScrollCmdData &scrollCmdData, const cdg::ScrollType type)
{
    if (type != cdg::ScrollCopy && scrollCmdData.color >= 16)
        return;

    const int pxBytes = 6 * m_bytesPerPixel;
    const int rowMoveBytes = 294 * m_bytesPerPixel;

    if (scrollCmdData.hSCmd == 2)
    {
        // scroll left 6px
        unsigned char tmpPixels[24];
        if (pxBytes > static_cast<int>(sizeof(tmpPixels)))
            return;
        for (auto i = 0; i < 216; i++)
        {
            auto bits = m_image.scanLine(i);
            memcpy(tmpPixels, bits, pxBytes);
            memmove(bits, bits + pxBytes, rowMoveBytes);
            if (type == cdg::ScrollCopy)
                memcpy(bits + m_borderRBytesOffset, tmpPixels, pxBytes);
            else
                memset(bits + m_borderLRBytes, scrollCmdData.color, pxBytes);
        }
    }
    if (scrollCmdData.hSCmd == 1)
    {
        // scroll right 6px
        unsigned char tmpPixels[24];
        if (pxBytes > static_cast<int>(sizeof(tmpPixels)))
            return;
        for (auto i = 0; i < 216; i++)
        {
            auto bits = m_image.scanLine(i);
            memcpy(tmpPixels, bits + rowMoveBytes, pxBytes);
            memmove(bits + pxBytes, bits, rowMoveBytes);
            if (type == cdg::ScrollCopy)
                memcpy(bits, tmpPixels, pxBytes);
            else
                memset(bits, scrollCmdData.color, pxBytes);
        }
    }
    if (scrollCmdData.vSCmd == 2)
    {
        // scroll up 12px
        auto bits = m_image.bits();
        const int bandBytes = m_image.bytesPerLine() * 12;
        const int bodyBytes = m_image.bytesPerLine() * 204;
        std::vector<unsigned char> tmpLines(static_cast<size_t>(bandBytes));
        memcpy(tmpLines.data(), bits, bandBytes);
        memmove(bits, bits + bandBytes, bodyBytes);
        if (type == cdg::ScrollCopy)
            memcpy(bits + bodyBytes, tmpLines.data(), bandBytes);
        else
            memset(bits + bodyBytes, scrollCmdData.color, bandBytes);
    }
    if (scrollCmdData.vSCmd == 1)
    {
        // scroll down 12px
        auto bits = m_image.bits();
        const int bandBytes = m_image.bytesPerLine() * 12;
        const int bodyBytes = m_image.bytesPerLine() * 204;
        std::vector<unsigned char> tmpLines(static_cast<size_t>(bandBytes));
        memcpy(tmpLines.data(), bits + bodyBytes, bandBytes);
        memmove(bits + bandBytes, bits, bodyBytes);
        if (type == cdg::ScrollCopy)
            memcpy(bits, tmpLines.data(), bandBytes);
        else
            memset(bits, scrollCmdData.color, bandBytes);
    }
    if (m_curVOffset != scrollCmdData.vSOffset)
        m_curVOffset = scrollCmdData.vSOffset;
    if (m_curHOffset != scrollCmdData.hSOffset)
        m_curHOffset = scrollCmdData.hSOffset;
}


void CdgImageFrame::cmdDefineTransparent([[maybe_unused]] const std::array<char,16> &data)
{
    //qInfo() << "libCDG - unsupported DefineTransparent command called";
    // Unused CDG command from redbook spec
    // This is rarely if ever used
    // No idea what the data structure is, it's missing from CDG Revealed
}

