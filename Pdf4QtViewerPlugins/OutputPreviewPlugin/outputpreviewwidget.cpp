//    Copyright (C) 2021 Jakub Melka
//
//    This file is part of Pdf4Qt.
//
//    Pdf4Qt is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Lesser General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    Pdf4Qt is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public License
//    along with Pdf4Qt.  If not, see <https://www.gnu.org/licenses/>.

#include "outputpreviewwidget.h"

#include "pdfwidgetutils.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>

namespace pdfplugin
{

OutputPreviewWidget::OutputPreviewWidget(QWidget* parent) :
    BaseClass(parent),
    m_inkMapper(nullptr),
    m_displayMode(Separations),
    m_alarmColor(Qt::red),
    m_inkCoverageLimit(3.0),
    m_richBlackLimit(1.0)
{
    setMouseTracking(true);
}

QSize OutputPreviewWidget::sizeHint() const
{
    return pdf::PDFWidgetUtils::scaleDPI(this, QSize(500, 300));
}

QSize OutputPreviewWidget::minimumSizeHint() const
{
    return pdf::PDFWidgetUtils::scaleDPI(this, QSize(400, 300));
}

void OutputPreviewWidget::clear()
{
    m_pageImage = QImage();
    m_originalProcessBitmap = pdf::PDFFloatBitmapWithColorSpace();
    m_pageSizeMM = QSizeF();
    m_infoBoxItems.clear();
    m_imagePointUnderCursor = std::nullopt;
    m_inkCoverageMM.dirty();
    m_alarmCoverageImage.dirty();
    m_alarmRichBlackImage.dirty();
    update();
}

void OutputPreviewWidget::setPageImage(QImage image, pdf::PDFFloatBitmapWithColorSpace originalProcessBitmap, QSizeF pageSizeMM)
{
    m_pageImage = qMove(image);
    m_originalProcessBitmap = qMove(originalProcessBitmap);
    m_pageSizeMM = pageSizeMM;

    if (m_imagePointUnderCursor.has_value())
    {
        QPoint point = m_imagePointUnderCursor.value();
        if (point.x() >= image.width() || point.y() >= image.height())
        {
            m_imagePointUnderCursor = std::nullopt;
        }
    }

    m_inkCoverageMM.dirty();
    m_alarmCoverageImage.dirty();
    m_alarmRichBlackImage.dirty();

    buildInfoBoxItems();
    update();
}

void OutputPreviewWidget::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);

    Q_UNUSED(event);

    QRect rect = this->rect();
    painter.fillRect(rect, Qt::gray);

    QRect contentRect = getContentRect();
    QRect pageImageRect = getPageImageRect(contentRect);

    if (pageImageRect.isValid())
    {
        painter.save();
        painter.setClipRect(pageImageRect, Qt::IntersectClip);

        switch (m_displayMode)
        {
            case Separations:
            {
                if (!m_pageImage.isNull())
                {
                    painter.translate(0, (pageImageRect.height() - m_pageImage.height()) / 2);
                    painter.drawImage(pageImageRect.topLeft(), m_pageImage);
                }
                break;
            }

            case ColorWarningInkCoverage:
            {
                const QImage& image = getAlarmCoverageImage();
                if (!image.isNull())
                {
                    painter.translate(0, (pageImageRect.height() - image.height()) / 2);
                    painter.drawImage(pageImageRect.topLeft(), image);
                }
                break;
            }

            case ColorWarningRichBlack:
            {
                const QImage& image = getAlarmRichBlackImage();
                if (!image.isNull())
                {
                    painter.translate(0, (pageImageRect.height() - image.height()) / 2);
                    painter.drawImage(pageImageRect.topLeft(), image);
                }
                break;
            }

            case InkCoverage:
                break;

            default:
                Q_ASSERT(false);
        }

        painter.restore();
    }

    if (!m_infoBoxItems.empty())
    {
        painter.save();

        int infoBoxWidth = getInfoBoxWidth();
        int itemHorizontalMargin = getInfoBoxContentHorizontalMargin();

        QRect infoBoxRect = contentRect;
        infoBoxRect.setLeft(infoBoxRect.right() - infoBoxWidth);

        painter.setPen(Qt::black);
        painter.setBrush(QBrush(Qt::white));
        painter.drawRect(infoBoxRect);
        painter.setClipRect(infoBoxRect, Qt::IntersectClip);
        painter.setBrush(Qt::NoBrush);

        QFontMetrics fontMetrics(painter.font(), painter.device());
        QRect rowRect = infoBoxRect;
        rowRect.setHeight(fontMetrics.lineSpacing());

        for (const auto& infoBoxItem : m_infoBoxItems)
        {
            switch (infoBoxItem.style)
            {
                case pdfplugin::OutputPreviewWidget::Header:
                {
                    painter.save();

                    QFont font = painter.font();
                    font.setBold(true);
                    painter.setFont(font);

                    painter.drawText(rowRect, Qt::AlignCenter | Qt::TextSingleLine, infoBoxItem.caption);

                    painter.restore();
                    break;
                }

                case pdfplugin::OutputPreviewWidget::Separator:
                    break;

                case pdfplugin::OutputPreviewWidget::ColoredItem:
                {
                    QRect cellRect = rowRect.marginsRemoved(QMargins(itemHorizontalMargin, 0, itemHorizontalMargin, 0));

                    if (infoBoxItem.color.isValid())
                    {
                        QRect ellipseRect = cellRect;
                        ellipseRect.setWidth(ellipseRect.height());
                        cellRect.setLeft(ellipseRect.right() + 1);

                        painter.save();
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(QBrush(infoBoxItem.color));
                        painter.drawEllipse(ellipseRect);
                        painter.restore();
                    }

                    painter.drawText(cellRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, infoBoxItem.caption);
                    painter.drawText(cellRect, Qt::AlignVCenter | Qt::AlignRight | Qt::TextSingleLine, infoBoxItem.value);
                    break;
                }

                case pdfplugin::OutputPreviewWidget::ColorOnly:
                {
                    QRect cellRect = rowRect.marginsRemoved(QMargins(itemHorizontalMargin, 0, itemHorizontalMargin, 0));
                    QPoint center = cellRect.center();
                    cellRect.setWidth(cellRect.width() / 4);
                    cellRect.moveCenter(center);
                    painter.fillRect(cellRect, infoBoxItem.color);
                    break;
                }

                default:
                    Q_ASSERT(false);
                    break;
            }

            rowRect.translate(0, rowRect.height());
        }

        painter.restore();
    }
}

QMargins OutputPreviewWidget::getDrawMargins() const
{
    const int horizontalMargin = pdf::PDFWidgetUtils::scaleDPI_x(this, 5);
    const int verticalMargin = pdf::PDFWidgetUtils::scaleDPI_y(this, 5);

    return QMargins(horizontalMargin, verticalMargin, horizontalMargin, verticalMargin);
}

QRect OutputPreviewWidget::getContentRect() const
{
    QRect rect = this->rect();
    QRect contentRect = rect.marginsRemoved(getDrawMargins());
    return contentRect;
}

QRect OutputPreviewWidget::getPageImageRect(QRect contentRect) const
{
    int infoBoxWidth = getInfoBoxWidth();

    if (infoBoxWidth > 0)
    {
        infoBoxWidth += pdf::PDFWidgetUtils::scaleDPI_x(this, 5);
    }

    contentRect.setRight(contentRect.right() - infoBoxWidth);
    return contentRect;
}

int OutputPreviewWidget::getInfoBoxWidth() const
{
    if (m_infoBoxItems.empty())
    {
        return 0;
    }

    return pdf::PDFWidgetUtils::scaleDPI_x(this, 200);
}

int OutputPreviewWidget::getInfoBoxContentHorizontalMargin() const
{
    return pdf::PDFWidgetUtils::scaleDPI_x(this, 5);
}

void OutputPreviewWidget::buildInfoBoxItems()
{
    m_infoBoxItems.clear();

    switch (m_displayMode)
    {
        case Separations:
        case ColorWarningInkCoverage:
        case ColorWarningRichBlack:
        {
            if (m_originalProcessBitmap.getWidth() > 0 && m_originalProcessBitmap.getHeight() > 0)
            {
                const pdf::PDFPixelFormat pixelFormat = m_originalProcessBitmap.getPixelFormat();
                std::vector<pdf::PDFInkMapper::ColorInfo> separations = m_inkMapper->getSeparations(pixelFormat.getProcessColorChannelCount(), true);

                QStringList colorValues;
                colorValues.reserve(pixelFormat.getColorChannelCount());
                Q_ASSERT(pixelFormat.getColorChannelCount() == separations.size());

                QColor sampleColor;
                std::vector<QColor> inkColors;

                if (m_imagePointUnderCursor.has_value())
                {
                    QPoint point = m_imagePointUnderCursor.value();

                    Q_ASSERT(point.x() >= 0);
                    Q_ASSERT(point.x() < m_originalProcessBitmap.getWidth());
                    Q_ASSERT(point.y() >= 0);
                    Q_ASSERT(point.y() < m_originalProcessBitmap.getHeight());

                    pdf::PDFColorBuffer buffer = m_originalProcessBitmap.getPixel(point.x(), point.y());
                    for (int i = 0; i < pixelFormat.getColorChannelCount(); ++i)
                    {
                        const pdf::PDFColorComponent color = buffer[i] * 100.0f;
                        const int percent = qRound(color);
                        colorValues << QString("%1 %").arg(percent);

                        QColor inkColor = separations[i].color;
                        if (inkColor.isValid())
                        {
                            inkColor.setAlphaF(buffer[i]);
                            inkColors.push_back(inkColor);
                        }
                    }

                    Q_ASSERT(point.x() >= 0);
                    Q_ASSERT(point.x() < m_pageImage.width());
                    Q_ASSERT(point.y() >= 0);
                    Q_ASSERT(point.y() < m_pageImage.height());

                    sampleColor = m_pageImage.pixelColor(point);
                }
                else
                {
                    for (int i = 0; i < pixelFormat.getColorChannelCount(); ++i)
                    {
                        colorValues << QString();
                    }
                }

                // Count process/spot inks

                int processInks = 0;
                int spotInks = 0;

                for (const auto& colorInfo : separations)
                {
                    if (!colorInfo.isSpot)
                    {
                        ++processInks;
                    }
                    else
                    {
                        ++spotInks;
                    }
                }

                int colorValueIndex = 0;
                if (processInks > 0)
                {
                    addInfoBoxSeparator();
                    addInfoBoxHeader(tr("Process Inks"));

                    for (const auto& colorInfo : separations)
                    {
                        if (colorInfo.isSpot)
                        {
                            continue;
                        }

                        addInfoBoxColoredItem(colorInfo.color, colorInfo.textName, colorValues[colorValueIndex++]);
                    }
                }

                if (spotInks > 0)
                {
                    addInfoBoxSeparator();
                    addInfoBoxHeader(tr("Spot Inks"));

                    for (const auto& colorInfo : separations)
                    {
                        if (!colorInfo.isSpot)
                        {
                            continue;
                        }

                        addInfoBoxColoredItem(colorInfo.color, colorInfo.textName, colorValues[colorValueIndex++]);
                    }
                }

                if (sampleColor.isValid())
                {
                    addInfoBoxSeparator();
                    addInfoBoxHeader(tr("Sample Color"));
                    addInfoBoxColoredRect(sampleColor);
                }
            }
            break;
        }

        case InkCoverage:
            break;

        default:
            Q_ASSERT(false);
            break;
    }

    if (m_displayMode == Separations || m_displayMode == InkCoverage)
    {
        if (m_originalProcessBitmap.getWidth() > 0 && m_originalProcessBitmap.getHeight() > 0)
        {
            const pdf::PDFPixelFormat pixelFormat = m_originalProcessBitmap.getPixelFormat();
            std::vector<pdf::PDFInkMapper::ColorInfo> separations = m_inkMapper->getSeparations(pixelFormat.getProcessColorChannelCount(), true);
            const std::vector<pdf::PDFColorComponent>& inkCoverage = getInkCoverage();

            if (!inkCoverage.empty() && inkCoverage.size() == separations.size())
            {
                addInfoBoxSeparator();
                addInfoBoxHeader(tr("Ink Coverage"));

                QLocale locale;

                for (size_t i = 0; i < inkCoverage.size(); ++i)
                {
                    const pdf::PDFColorComponent area = inkCoverage[i];
                    const QColor separationColor = separations[i].color;
                    const QString& name = separations[i].textName;

                    addInfoBoxColoredItem(separationColor, name, QString("%1 mm²").arg(locale.toString(area, 'f', 2)));
                }
            }
        }
    }
}

void OutputPreviewWidget::addInfoBoxHeader(QString caption)
{
    m_infoBoxItems.push_back(InfoBoxItem(Header, QColor(), caption, QString()));
}

void OutputPreviewWidget::addInfoBoxSeparator()
{
    if (!m_infoBoxItems.empty())
    {
        m_infoBoxItems.push_back(InfoBoxItem(Separator, QColor(), QString(), QString()));
    }
}

void OutputPreviewWidget::addInfoBoxColoredItem(QColor color, QString caption, QString value)
{
    m_infoBoxItems.push_back(InfoBoxItem(ColoredItem, color, caption, value));
}

void OutputPreviewWidget::addInfoBoxColoredRect(QColor color)
{
    m_infoBoxItems.push_back(InfoBoxItem(ColorOnly, color, QString(), QString()));
}

const std::vector<pdf::PDFColorComponent>& OutputPreviewWidget::getInkCoverage() const
{
    return m_inkCoverageMM.get(this, &OutputPreviewWidget::getInkCoverageImpl);
}

const QImage& OutputPreviewWidget::getAlarmCoverageImage() const
{
    return m_alarmCoverageImage.get(this, &OutputPreviewWidget::getAlarmCoverageImageImpl);
}

const QImage& OutputPreviewWidget::getAlarmRichBlackImage() const
{
    return m_alarmRichBlackImage.get(this, &OutputPreviewWidget::getAlarmRichBlackImageImpl);
}

std::vector<pdf::PDFColorComponent> OutputPreviewWidget::getInkCoverageImpl() const
{
    std::vector<pdf::PDFColorComponent> result;

    if (m_originalProcessBitmap.getWidth() > 0 && m_originalProcessBitmap.getHeight() > 0)
    {
        pdf::PDFPixelFormat pixelFormat = m_originalProcessBitmap.getPixelFormat();
        pdf::PDFColorComponent totalArea = m_pageSizeMM.width() * m_pageSizeMM.height();
        pdf::PDFColorComponent pixelArea = totalArea / pdf::PDFColorComponent(m_originalProcessBitmap.getWidth() * m_originalProcessBitmap.getHeight());

        const uint8_t colorChannelCount = pixelFormat.getColorChannelCount();
        result.resize(colorChannelCount, 0.0f);

        for (size_t y = 0; y < m_originalProcessBitmap.getHeight(); ++y)
        {
            for (size_t x = 0; x < m_originalProcessBitmap.getWidth(); ++x)
            {
                const pdf::PDFConstColorBuffer buffer = m_originalProcessBitmap.getPixel(x, y);
                const pdf::PDFColorComponent alpha = pixelFormat.hasOpacityChannel() ? buffer[pixelFormat.getOpacityChannelIndex()] : 1.0f;

                for (uint8_t i = 0; i < colorChannelCount; ++i)
                {
                    result[i] += buffer[i] * alpha;
                }
            }
        }

        for (uint8_t i = 0; i < colorChannelCount; ++i)
        {
            result[i] *= pixelArea;
        }
    }

    return result;
}

QImage OutputPreviewWidget::getAlarmCoverageImageImpl() const
{
    QImage alarmImage = m_pageImage;

    const int width = alarmImage.width();
    const int height = alarmImage.height();

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            pdf::PDFColorComponent inkCoverage = m_originalProcessBitmap.getPixelInkCoverage(x, y);
            if (inkCoverage > m_inkCoverageLimit)
            {
                alarmImage.setPixelColor(x, y, m_alarmColor);
            }
        }
    }

    return alarmImage;
}

QImage OutputPreviewWidget::getAlarmRichBlackImageImpl() const
{
    QImage alarmImage = m_pageImage;

    const pdf::PDFPixelFormat pixelFormat = m_originalProcessBitmap.getPixelFormat();
    if (pixelFormat.getProcessColorChannelCount() == 4)
    {
        const int width = alarmImage.width();
        const int height = alarmImage.height();

        const uint8_t blackChannelIndex = pixelFormat.getProcessColorChannelIndexStart() + 3;

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                pdf::PDFConstColorBuffer buffer = m_originalProcessBitmap.getPixel(x, y);
                pdf::PDFColorComponent blackInk = buffer[blackChannelIndex];

                if (blackInk > m_richBlackLimit)
                {
                    pdf::PDFColorComponent inkCoverage = m_originalProcessBitmap.getPixelInkCoverage(x, y);
                    pdf::PDFColorComponent inkCoverageWithoutBlack = inkCoverage - blackInk;

                    if (!qFuzzyIsNull(inkCoverageWithoutBlack))
                    {
                        alarmImage.setPixelColor(x, y, m_alarmColor);
                    }
                }
            }
        }
    }

    return alarmImage;
}

pdf::PDFColorComponent OutputPreviewWidget::getRichBlackLimit() const
{
    return m_richBlackLimit;
}

void OutputPreviewWidget::setRichBlackLimit(pdf::PDFColorComponent richBlackLimit)
{
    if (m_richBlackLimit != richBlackLimit)
    {
        m_richBlackLimit = richBlackLimit;
        m_alarmRichBlackImage.dirty();
        buildInfoBoxItems();
        update();
    }
}

pdf::PDFColorComponent OutputPreviewWidget::getInkCoverageLimit() const
{
    return m_inkCoverageLimit;
}

void OutputPreviewWidget::setInkCoverageLimit(pdf::PDFColorComponent inkCoverageLimit)
{
    if (m_inkCoverageLimit != inkCoverageLimit)
    {
        m_inkCoverageLimit = inkCoverageLimit;
        m_alarmCoverageImage.dirty();
        buildInfoBoxItems();
        update();
    }
}

OutputPreviewWidget::DisplayMode OutputPreviewWidget::getDisplayMode() const
{
    return m_displayMode;
}

void OutputPreviewWidget::setDisplayMode(const DisplayMode& displayMode)
{
    if (m_displayMode != displayMode)
    {
        m_displayMode = displayMode;
        buildInfoBoxItems();
        update();
    }
}

QColor OutputPreviewWidget::getAlarmColor() const
{
    return m_alarmColor;
}

void OutputPreviewWidget::setAlarmColor(const QColor& alarmColor)
{
    if (m_alarmColor != alarmColor)
    {
        m_alarmColor = alarmColor;
        m_alarmCoverageImage.dirty();
        m_alarmRichBlackImage.dirty();
        update();
    }
}

const pdf::PDFInkMapper* OutputPreviewWidget::getInkMapper() const
{
    return m_inkMapper;
}

void OutputPreviewWidget::setInkMapper(const pdf::PDFInkMapper* inkMapper)
{
    m_inkMapper = inkMapper;
}

QSize OutputPreviewWidget::getPageImageSizeHint() const
{
    return getPageImageRect(getContentRect()).size();
}

void OutputPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_imagePointUnderCursor = std::nullopt;

    if (m_pageImage.isNull())
    {
        // Nothing to do...
        return;
    }

    QPoint position = event->pos();
    QRect rect = getPageImageRect(getContentRect());

    if (rect.contains(position))
    {
        int verticalImageOffset = (rect.height() - m_pageImage.height()) / 2;
        QPoint imagePoint = position - rect.topLeft() - QPoint(0, verticalImageOffset);

        if (imagePoint.x() >= 0 && imagePoint.x() < m_pageImage.width() &&
            imagePoint.y() >= 0 && imagePoint.y() < m_pageImage.height())
        {
            m_imagePointUnderCursor = imagePoint;
        }
    }

    buildInfoBoxItems();
    update();
}

}   // pdfplugin
