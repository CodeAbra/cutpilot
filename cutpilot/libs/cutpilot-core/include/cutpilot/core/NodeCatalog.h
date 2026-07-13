#pragma once

#include "cutpilot/core/Node.h"

#include <QString>
#include <QVector>

namespace cutpilot::core {

// One taxonomy row: the category the palette groups under and a ready-to-place
// prototype carrying real typed ports. A single catalog serves the command
// palette, the tool pill, and the connector-drop offers, so the creatable node
// set can never drift between entry points.
struct CatalogEntry {
    QString category;
    Node prototype;
};

// The full creatable taxonomy, in presentation order.
const QVector<CatalogEntry> &nodeCatalog();

// The prototype whose title matches, or a Blank node when absent.
Node catalogPrototype(const QString &title);

} // namespace cutpilot::core
