#include "cutpilot/core/AlignmentGuides.h"

#include <QtGlobal>

namespace cutpilot::core {

QVector<AlignmentGuide> computeAlignmentGuides(const QRectF &movingRect,
                                               const QVector<QRectF> &neighbourRects,
                                               qreal worldThreshold)
{
    QVector<AlignmentGuide> guides;

    const qreal movingX[3] = { movingRect.left(), movingRect.center().x(),
                               movingRect.right() };
    const qreal movingY[3] = { movingRect.top(), movingRect.center().y(),
                               movingRect.bottom() };

    for (const QRectF &nb : neighbourRects) {
        const qreal nbX[3] = { nb.left(), nb.center().x(), nb.right() };
        const qreal nbY[3] = { nb.top(), nb.center().y(), nb.bottom() };

        for (int f = 0; f < 3; ++f) {
            if (qAbs(movingX[f] - nbX[f]) <= worldThreshold) {
                AlignmentGuide g;
                g.axis = GuideAxis::Vertical;
                g.coordinate = nbX[f];
                g.spanStart = qMin(movingRect.top(), nb.top());
                g.spanEnd = qMax(movingRect.bottom(), nb.bottom());
                guides.push_back(g);
            }
            if (qAbs(movingY[f] - nbY[f]) <= worldThreshold) {
                AlignmentGuide g;
                g.axis = GuideAxis::Horizontal;
                g.coordinate = nbY[f];
                g.spanStart = qMin(movingRect.left(), nb.left());
                g.spanEnd = qMax(movingRect.right(), nb.right());
                guides.push_back(g);
            }
        }
    }

    return guides;
}

} // namespace cutpilot::core
